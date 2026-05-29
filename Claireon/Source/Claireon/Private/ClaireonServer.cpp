// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonServer.h"
#include "ClaireonAutoSave.h"
#include "ClaireonLog.h"
#include "ClaireonBridge.h"
#include "ClaireonSafeExec.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonTool_MapOpen.h"
#include "Tools/ClaireonTool_MapDuplicate.h"
#include "Tools/ClaireonTool_PIEStart.h"
#include "Tools/ClaireonTool_PIEStop.h"
#include "Tools/ClaireonTool_LiveCodingReload.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "HttpServerResponse.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Editor.h"
#include "Misc/App.h"
#include "ClaireonSettings.h"
#include "ClaireonXmlFormatter.h"
#include "Tools/ClaireonTransactionGroupState.h"
#include "ClaireonWorldReadiness.h"

#include <atomic>

static constexpr uint32 MaxPortRetries = 10;

// Tools exposed directly via MCP tools/list / tools/call. The MCP surface is
// exactly two meta-tools: tool_search and python_execute. Every other
// registered tool (including the transaction_* family) is reachable only via
// the claireon.<tool>(...) Python attribute namespace inside python_execute. The
// proxy advertises the same two bare names; the editor and proxy must stay in
// lock-step on this set.
static const TSet<FString> MCPVisibleTools = {
	TEXT("tool_search"),
	TEXT("python_execute"),
};

FClaireonServer::FClaireonServer()
{
}

FClaireonServer::~FClaireonServer()
{
	if (bIsRunning)
	{
		Stop();
	}
}

bool FClaireonServer::TryStart(uint16 Port)
{
	if (bIsRunning)
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP] Server is already running on port %u"), BoundPort);
		return false;
	}

	FHttpServerModule& HttpModule = FHttpServerModule::Get();
	TSharedPtr<IHttpRouter> Router = HttpModule.GetHttpRouter(
		static_cast<uint32>(Port), /*bFailOnBindFailure=*/ true);
	if (!Router.IsValid())
	{
		UE_LOG(LogClaireon, Verbose,
			TEXT("[MCP] TryStart: bind failed for port %u (caller decides next step)"),
			static_cast<uint32>(Port));
		return false;
	}
	BoundPort = static_cast<uint32>(Port);

	// Bind POST /mcp
	{
		FHttpPath RoutePath(TEXT("/mcp"));
		FHttpRequestHandler Handler = FHttpRequestHandler::CreateRaw(this, &FClaireonServer::HandlePostRequest);
		FHttpRouteHandle Handle = Router->BindRoute(RoutePath, EHttpServerRequestVerbs::VERB_POST, Handler);
		if (Handle)
		{
			RouteHandles.Add(Handle);
		}
		else
		{
			UE_LOG(LogClaireon, Error, TEXT("[MCP] Failed to bind POST /mcp route"));
			Stop();
			return false;
		}
	}
	{
		FHttpPath RoutePath(TEXT("/mcp"));
		FHttpRequestHandler Handler = FHttpRequestHandler::CreateRaw(this, &FClaireonServer::HandleGetRequest);
		FHttpRouteHandle Handle = Router->BindRoute(RoutePath, EHttpServerRequestVerbs::VERB_GET, Handler);
		if (Handle)
		{
			RouteHandles.Add(Handle);
		}
	}
	{
		FHttpPath RoutePath(TEXT("/mcp"));
		FHttpRequestHandler Handler = FHttpRequestHandler::CreateRaw(this, &FClaireonServer::HandleDeleteRequest);
		FHttpRouteHandle Handle = Router->BindRoute(RoutePath, EHttpServerRequestVerbs::VERB_DELETE, Handler);
		if (Handle)
		{
			RouteHandles.Add(Handle);
		}
	}

	HttpModule.StartAllListeners();

	bIsRunning = true;
	bInitialized = false;
	TotalRequestCount = 0;
	ErrorCount = 0;
	StartTime = FDateTime::Now();

	LoadMCPContent();

	WritePortFile();

	UE_LOG(LogClaireon, Display, TEXT("[MCP] Server listening on port %u"), BoundPort);
	if (SessionToken.IsEmpty())
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[MCP] Session token is empty -- token gating DISABLED. ")
			TEXT("This is the direct-connect path; expected only when ")
			TEXT("the always-on proxy is disabled."));
	}
	else
	{
		UE_LOG(LogClaireon, Display,
			TEXT("[MCP] Session token gating ACTIVE (length=%d chars)"), SessionToken.Len());
	}

	return true;
}

uint16 FClaireonServer::StartEphemeral()
{
	// UE's HTTP server module does not expose an "OS-picked port" entrypoint
	// cleanly. We sweep an ephemeral-range scan
	// (the same pattern claireon_proxy.py uses for cross-worktree binds) so a
	// healthy port falls out within a few attempts. The walk caps at 32
	// attempts; a busy box that fails 32 ephemeral binds in a row is broken
	// in a way auto-promote cannot fix.
	if (bIsRunning)
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP] StartEphemeral called while server already running on %u"), BoundPort);
		return 0;
	}

	constexpr uint32 EphemeralBase = 49152;
	constexpr uint32 EphemeralSpan = 16384; // 49152..65535
	// Seed with FPlatformTime to spread retries across the range; the bind
	// itself decides if a port is free.
	const uint32 Seed = static_cast<uint32>(static_cast<uint64>(FPlatformTime::Cycles64()) & 0xffffffffu);
	for (uint32 Attempt = 0; Attempt < 32; ++Attempt)
	{
		const uint32 Candidate = EphemeralBase + ((Seed + Attempt * 1009u) % EphemeralSpan);
		if (TryStart(static_cast<uint16>(Candidate)))
		{
			return static_cast<uint16>(BoundPort);
		}
	}
	UE_LOG(LogClaireon, Error, TEXT("[MCP] StartEphemeral exhausted 32 attempts"));
	return 0;
}

bool FClaireonServer::Start(uint32 Port)
{
	if (bIsRunning)
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP] Server is already running on port %u"), BoundPort);
		return false;
	}

	FHttpServerModule& HttpModule = FHttpServerModule::Get();

	// Legacy increment-on-failure path. Prefer TryStart / StartEphemeral.
	// Try binding to the requested port, incrementing on failure
	TSharedPtr<IHttpRouter> Router;
	uint32 AttemptPort = Port;
	for (uint32 Attempt = 0; Attempt < MaxPortRetries; ++Attempt)
	{
		Router = HttpModule.GetHttpRouter(AttemptPort, /*bFailOnBindFailure=*/true);
		if (Router.IsValid())
		{
			BoundPort = AttemptPort;
			break;
		}
		UE_LOG(LogClaireon, Warning, TEXT("[MCP] Failed to bind port %u, trying %u"), AttemptPort, AttemptPort + 1);
		++AttemptPort;
	}

	if (!Router.IsValid())
	{
		UE_LOG(LogClaireon, Error, TEXT("[MCP] Failed to bind any port in range %u-%u"), Port, Port + MaxPortRetries - 1);
		return false;
	}

	// Bind POST /mcp
	{
		FHttpPath RoutePath(TEXT("/mcp"));
		FHttpRequestHandler Handler = FHttpRequestHandler::CreateRaw(this, &FClaireonServer::HandlePostRequest);
		FHttpRouteHandle Handle = Router->BindRoute(RoutePath, EHttpServerRequestVerbs::VERB_POST, Handler);
		if (Handle)
		{
			RouteHandles.Add(Handle);
		}
		else
		{
			UE_LOG(LogClaireon, Error, TEXT("[MCP] Failed to bind POST /mcp route"));
			Stop();
			return false;
		}
	}

	// Bind GET /mcp (returns 405 for now — SSE deferred)
	{
		FHttpPath RoutePath(TEXT("/mcp"));
		FHttpRequestHandler Handler = FHttpRequestHandler::CreateRaw(this, &FClaireonServer::HandleGetRequest);
		FHttpRouteHandle Handle = Router->BindRoute(RoutePath, EHttpServerRequestVerbs::VERB_GET, Handler);
		if (Handle)
		{
			RouteHandles.Add(Handle);
		}
	}

	// Bind DELETE /mcp (returns 405 for now — session termination deferred)
	{
		FHttpPath RoutePath(TEXT("/mcp"));
		FHttpRequestHandler Handler = FHttpRequestHandler::CreateRaw(this, &FClaireonServer::HandleDeleteRequest);
		FHttpRouteHandle Handle = Router->BindRoute(RoutePath, EHttpServerRequestVerbs::VERB_DELETE, Handler);
		if (Handle)
		{
			RouteHandles.Add(Handle);
		}
	}

	HttpModule.StartAllListeners();

	bIsRunning = true;
	bInitialized = false;
	TotalRequestCount = 0;
	ErrorCount = 0;
	StartTime = FDateTime::Now();

	LoadMCPContent();

	WritePortFile();

	UE_LOG(LogClaireon, Display, TEXT("[MCP] Server listening on port %u"), BoundPort);
	if (SessionToken.IsEmpty())
	{
		// Direct-connect path (no proxy wiring / command-line override).
		// Log once at Warning so an operator who expected the proxy's
		// token-gated path sees it clearly in the log.
		UE_LOG(LogClaireon, Warning,
			TEXT("[MCP] Session token is empty -- token gating DISABLED. ")
			TEXT("This is the direct-connect path; expected only when ")
			TEXT("the always-on proxy is disabled."));
	}
	else
	{
		UE_LOG(LogClaireon, Display,
			TEXT("[MCP] Session token gating ACTIVE (length=%d chars)"), SessionToken.Len());
	}

	return true;
}

void FClaireonServer::Stop()
{
	if (!bIsRunning)
	{
		return;
	}

	if (FHttpServerModule::IsAvailable())
	{
		TSharedPtr<IHttpRouter> Router = FHttpServerModule::Get().GetHttpRouter(BoundPort, /*bFailOnBindFailure=*/false);
		if (Router.IsValid())
		{
			for (const FHttpRouteHandle& Handle : RouteHandles)
			{
				Router->UnbindRoute(Handle);
			}
		}
	}

	RouteHandles.Empty();
	DeletePortFile();

	if (bUserStopActive)
	{
		ClearUserStop();
	}

	bIsRunning = false;
	bInitialized = false;

	UE_LOG(LogClaireon, Display, TEXT("[MCP] Server stopped"));
}

void FClaireonServer::RegisterTool(TSharedPtr<IClaireonTool> Tool, FName SourceProvider)
{
	check(IsInGameThread());

	if (Tool.IsValid())
	{
		FString Name = Tool->GetName();
		UE_LOG(LogClaireon, Log, TEXT("[MCP] Registered tool: %s (source: %s)"), *Name,
			SourceProvider.IsNone() ? TEXT("unspecified") : *SourceProvider.ToString());
		Tools.Add(Name, Tool);
		if (!SourceProvider.IsNone())
		{
			ToolSourceMap.Add(Name, SourceProvider);
		}
		++ToolListGeneration;
		OnToolsChanged.Broadcast();
	}
}

void FClaireonServer::UnregisterTool(const FString& ToolName)
{
	check(IsInGameThread());

	if (Tools.Remove(ToolName) > 0)
	{
		ToolSourceMap.Remove(ToolName);
		++ToolListGeneration;
		OnToolsChanged.Broadcast();
		UE_LOG(LogClaireon, Log, TEXT("[MCP] Unregistered tool: %s"), *ToolName);
	}
}

void FClaireonServer::UnregisterToolsBySource(FName SourceProvider)
{
	check(IsInGameThread());

	if (SourceProvider.IsNone())
	{
		return;
	}

	TArray<FString> ToolsToRemove;
	for (const auto& Pair : ToolSourceMap)
	{
		if (Pair.Value == SourceProvider)
		{
			ToolsToRemove.Add(Pair.Key);
		}
	}

	if (ToolsToRemove.Num() == 0)
	{
		return;
	}

	for (const FString& ToolName : ToolsToRemove)
	{
		Tools.Remove(ToolName);
		ToolSourceMap.Remove(ToolName);
		UE_LOG(LogClaireon, Log, TEXT("[MCP] Unregistered tool: %s (source: %s)"), *ToolName, *SourceProvider.ToString());
	}

	++ToolListGeneration;
	OnToolsChanged.Broadcast();
}

TSharedPtr<IClaireonTool> FClaireonServer::FindTool(const FString& ToolName) const
{
	const TSharedPtr<IClaireonTool>* Found = Tools.Find(ToolName);
	return Found ? *Found : nullptr;
}

void FClaireonServer::ActivateUserStop()
{
	bUserStopActive = true;
	LastToolsCallTime = FPlatformTime::Seconds();
	OnUserStopChanged.Broadcast(true);
	UE_LOG(LogClaireon, Warning, TEXT("[MCP] User stop activated (Ctrl+.)"));

	// Start cooldown timer
	if (GEditor)
	{
		const float Cooldown = UClaireonSettings::Get()->UserStopCooldownSeconds;
		GEditor->GetTimerManager()->SetTimer(UserStopCooldownHandle,
			FTimerDelegate::CreateLambda([this]()
		{
			double TimeSinceLastCall = FPlatformTime::Seconds() - LastToolsCallTime;
			const float Cooldown = UClaireonSettings::Get()->UserStopCooldownSeconds;
			if (TimeSinceLastCall >= Cooldown)
			{
				ClearUserStop();
			}
			// If a new call came in, the timer was reset — keep checking
		}),
			1.0f, /*bLoop=*/true);
	}
}

void FClaireonServer::ClearUserStop()
{
	if (!bUserStopActive)
		return;
	bUserStopActive = false;
	if (GEditor && UserStopCooldownHandle.IsValid())
	{
		GEditor->GetTimerManager()->ClearTimer(UserStopCooldownHandle);
	}
	OnUserStopChanged.Broadcast(false);
	UE_LOG(LogClaireon, Log, TEXT("[MCP] User stop cleared"));
}

bool FClaireonServer::HandlePostRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Stamp activity time before any work so the toolbar "processing" blink fires
	// even on instant rejections/failures. HTTP server dispatches on the game thread.
	LastRequestTime = FPlatformTime::Seconds();

	if (!ValidateRequestHeaders(Request))
	{
		TSharedPtr<FJsonObject> ErrorResponse = FMCPJsonRpcResponse::MakeError(
			nullptr, FMCPJsonRpcResponse::InvalidRequest, TEXT("Request rejected: invalid Origin or Host header"));
		auto Response = FHttpServerResponse::Create(SerializeJson(ErrorResponse), TEXT("application/json"));
		OnComplete(MoveTemp(Response));
		return true;
	}

	// Token-required middleware. When SessionToken is set (proxy path),
	// reject any request missing a matching Authorization: Bearer header.
	// Responds 401-equivalent at the JSON-RPC level with no body detail to
	// avoid leaking whether the token is merely missing vs. wrong.
	if (!ValidateBearerToken(Request))
	{
		TSharedPtr<FJsonObject> ErrorResponse = FMCPJsonRpcResponse::MakeError(
			nullptr, FMCPJsonRpcResponse::InvalidRequest, TEXT("Unauthorized"));
		auto Response = FHttpServerResponse::Create(SerializeJson(ErrorResponse), TEXT("application/json"));
		Response->Code = EHttpServerResponseCodes::Denied; // 401
		OnComplete(MoveTemp(Response));
		return true;
	}

	// Parse UTF-8 body
	FString RequestBody;
	{
		FUTF8ToTCHAR TCHARData(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
		RequestBody = FString(TCHARData.Length(), TCHARData.Get());
	}

	UE_LOG(LogClaireon, Verbose, TEXT("[MCP] POST /mcp body: %s"), *RequestBody);

	// Parse JSON
	TSharedPtr<FJsonObject> JsonObject;
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestBody);
		if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
		{
			TSharedPtr<FJsonObject> ErrorResponse = FMCPJsonRpcResponse::MakeError(
				nullptr, FMCPJsonRpcResponse::ParseError, TEXT("Failed to parse JSON body"));
			auto Response = FHttpServerResponse::Create(SerializeJson(ErrorResponse), TEXT("application/json"));
			OnComplete(MoveTemp(Response));
			return true;
		}
	}

	// Parse JSON-RPC request
	FMCPJsonRpcRequest RpcRequest;
	if (!FMCPJsonRpcRequest::FromJsonObject(JsonObject, RpcRequest))
	{
		TSharedPtr<FJsonObject> ErrorResponse = FMCPJsonRpcResponse::MakeError(
			nullptr, FMCPJsonRpcResponse::InvalidRequest, TEXT("Invalid JSON-RPC 2.0 request"));
		auto Response = FHttpServerResponse::Create(SerializeJson(ErrorResponse), TEXT("application/json"));
		OnComplete(MoveTemp(Response));
		return true;
	}

	UE_LOG(LogClaireon, Log, TEXT("[MCP] Request: method=%s, id=%s"),
		*RpcRequest.Method,
		RpcRequest.Id.IsValid() ? *RpcRequest.Id->AsString() : TEXT("(notification)"));

	const double RequestStartTime = FPlatformTime::Seconds();

	// Build request context
	FMCPRequestContext Context;
	Context.Request = MoveTemp(RpcRequest);

	// Handle notifications (no response expected)
	if (Context.Request.IsNotification())
	{
		if (Context.Request.Method == TEXT("notifications/initialized"))
		{
			HandleInitialized(Context);
		}
		// All other notifications silently accepted

		// Log notification to diagnostics
		FMCPDiagnosticsEntry Entry;
		Entry.Timestamp = FDateTime::Now();
		Entry.Method = Context.Request.Method;
		Entry.DurationMs = (FPlatformTime::Seconds() - RequestStartTime) * 1000.0;
		Entry.RequestBody = RequestBody.Left(2048);
		AddDiagnosticsEntry(MoveTemp(Entry));

		// Return 202 Accepted for notifications (no body)
		auto Response = FHttpServerResponse::Create(FString(TEXT("")), FString(TEXT("text/plain")));
		Response->Code = EHttpServerResponseCodes::Accepted;
		OnComplete(MoveTemp(Response));
		return true;
	}

	// Dispatch request and produce response
	TSharedPtr<FJsonObject> ResponseJson = DispatchRequest(Context);

	FString ResponseString = SerializeJson(ResponseJson);

	// Log to diagnostics
	{
		FMCPDiagnosticsEntry Entry;
		Entry.Timestamp = FDateTime::Now();
		Entry.Method = Context.Request.Method;
		Entry.DurationMs = (FPlatformTime::Seconds() - RequestStartTime) * 1000.0;
		Entry.bIsError = ResponseJson.IsValid() && ResponseJson->HasField(TEXT("error"));
		Entry.RequestBody = RequestBody.Left(2048);
		Entry.ResponseBody = ResponseString.Left(2048);

		// Extract tool name for tools/call
		if (Context.Request.Method == TEXT("tools/call") && Context.Request.Params.IsValid())
		{
			Context.Request.Params->TryGetStringField(TEXT("name"), Entry.ToolName);
		}

		AddDiagnosticsEntry(MoveTemp(Entry));
	}

	auto Response = FHttpServerResponse::Create(ResponseString, FString(TEXT("application/json")));
	OnComplete(MoveTemp(Response));
	return true;
}

bool FClaireonServer::HandleGetRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// SSE streaming deferred — return 405 Method Not Allowed
	auto Response = FHttpServerResponse::Create(FString(TEXT("")), FString(TEXT("text/plain")));
	Response->Code = EHttpServerResponseCodes::BadMethod;
	OnComplete(MoveTemp(Response));
	return true;
}

bool FClaireonServer::HandleDeleteRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	// Session termination deferred — return 405 Method Not Allowed
	auto Response = FHttpServerResponse::Create(FString(TEXT("")), FString(TEXT("text/plain")));
	Response->Code = EHttpServerResponseCodes::BadMethod;
	OnComplete(MoveTemp(Response));
	return true;
}

TSharedPtr<FJsonObject> FClaireonServer::DispatchRequest(const FMCPRequestContext& Context)
{
	const FString& Method = Context.Request.Method;
	const TSharedPtr<FJsonValue>& Id = Context.Request.Id;

	if (Method == TEXT("initialize"))
	{
		return HandleInitialize(Context);
	}
	else if (Method == TEXT("tools/list"))
	{
		return HandleToolsList(Context);
	}
	else if (Method == TEXT("tools/call"))
	{
		return HandleToolsCall(Context);
	}
	else if (Method == TEXT("ping"))
	{
		return HandlePing(Context);
	}
	// UPDATE_HERE_WHEN_ADDING_NEW_MCP_METHOD: keep ClaireonServer.cpp
	// DispatchRequest and claireon_proxy.py FORWARDED_METHODS in sync.
	else if (Method == TEXT("prompts/list"))
	{
		return HandlePromptsList(Context);
	}
	else if (Method == TEXT("prompts/get"))
	{
		return HandlePromptsGet(Context);
	}
	else if (Method == TEXT("resources/list"))
	{
		return HandleResourcesList(Context);
	}
	else if (Method == TEXT("resources/read"))
	{
		return HandleResourcesRead(Context);
	}
	else if (Method == TEXT("resources/templates/list"))
	{
		return HandleResourceTemplatesList(Context);
	}

	return FMCPJsonRpcResponse::MakeError(Id, FMCPJsonRpcResponse::MethodNotFound,
		FString::Printf(TEXT("Unknown method: %s"), *Method));
}

TSharedPtr<FJsonObject> FClaireonServer::HandleInitialize(const FMCPRequestContext& Context)
{
	// Build capabilities
	TSharedPtr<FJsonObject> ToolsCap = MakeShared<FJsonObject>();
	ToolsCap->SetBoolField(TEXT("listChanged"), true);

	TSharedPtr<FJsonObject> PromptsCap = MakeShared<FJsonObject>();
	PromptsCap->SetBoolField(TEXT("listChanged"), false);

	TSharedPtr<FJsonObject> ResourcesCap = MakeShared<FJsonObject>();
	ResourcesCap->SetBoolField(TEXT("subscribe"), false);
	ResourcesCap->SetBoolField(TEXT("listChanged"), false);

	TSharedPtr<FJsonObject> Capabilities = MakeShared<FJsonObject>();
	Capabilities->SetObjectField(TEXT("tools"), ToolsCap);
	Capabilities->SetObjectField(TEXT("prompts"), PromptsCap);
	Capabilities->SetObjectField(TEXT("resources"), ResourcesCap);

	// Build server info
	TSharedPtr<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
	ServerInfo->SetStringField(TEXT("name"), TEXT("claireon-unreal-editor"));
	ServerInfo->SetStringField(TEXT("version"), TEXT("1.0.0"));

	// Build result
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("protocolVersion"), TEXT("2025-03-26"));
	Result->SetObjectField(TEXT("capabilities"), Capabilities);
	Result->SetObjectField(TEXT("serverInfo"), ServerInfo);
	Result->SetStringField(TEXT("instructions"),
		TEXT("MCP server running inside the Unreal Editor. Call tools/list for the full tool catalog."));

	UE_LOG(LogClaireon, Display, TEXT("[MCP] Initialize handshake completed"));

	return FMCPJsonRpcResponse::MakeResult(Context.Request.Id, Result);
}

void FClaireonServer::HandleInitialized(const FMCPRequestContext& Context)
{
	bInitialized = true;

	// Reset per-session feedback nudge state
	SessionToolCallCount = 0;
	SessionToolErrorCount = 0;
	bFeedbackNudgeSent = false;
	bFeedbackSubmittedThisSession = false;

	// Reset transaction group state (close any leaked open groups)
	ClaireonTransactionGroupState::ResetGroupState();

	UE_LOG(LogClaireon, Display, TEXT("[MCP] Client initialized"));
}

TSharedPtr<FJsonObject> FClaireonServer::HandleToolsList(const FMCPRequestContext& Context)
{
	TArray<TSharedPtr<FJsonValue>> ToolArray;

	// Expose exactly the two-tool MCP surface (tool_search, python_execute). Internal tools
	// stay in the Tools map for `tool_search` to enumerate and `python_execute` to invoke
	// via `claireon.*(...)`, but they are not advertised here.
	for (const FString& VisibleToolName : MCPVisibleTools)
	{
		TSharedPtr<IClaireonTool>* Tool = Tools.Find(VisibleToolName);
		if (!Tool || !Tool->IsValid())
			continue;

		TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
		ToolObj->SetStringField(TEXT("name"), (*Tool)->GetName());

		if (VisibleToolName == TEXT("python_execute"))
		{
			// The MCP-visible description for python_execute names the categories
			// (no per-category examples) and steers callers at tool_search.
			FString CategoryList = FClaireonXmlFormatter::GenerateCategoryList(Tools);
			FString Description = FString::Printf(
				TEXT("Run Python code in the Unreal Editor with access to the `claireon` Python module. ")
				TEXT("Hundreds of tools are exposed under `claireon.<tool_name>(...)`, organised into these categories: %s. ")
				TEXT("Use `tool_search` to find specific tools by name or topic, and `tool_search(tool_name=\"...\")` ")
				TEXT("to fetch a tool's full input schema and example usage before calling it."),
				*CategoryList);
			ToolObj->SetStringField(TEXT("description"), Description);
		}
		else
		{
			ToolObj->SetStringField(TEXT("description"), (*Tool)->GetDescription());
		}

		TSharedPtr<FJsonObject> InputSchema = (*Tool)->GetInputSchema();
		if (InputSchema.IsValid())
		{
			ToolObj->SetObjectField(TEXT("inputSchema"), InputSchema);
		}

		ToolArray.Add(MakeShared<FJsonValueObject>(ToolObj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("tools"), ToolArray);

	return FMCPJsonRpcResponse::MakeResult(Context.Request.Id, Result);
}

// --- Async SEH helper for game-thread tool execution (Site 2) ---
// This is file-static, NOT part of ClaireonSafeExec, because it is tightly coupled
// to the async TPromise/TFuture pattern in HandleToolsCall.

#if PLATFORM_WINDOWS

#include "Windows/AllowWindowsPlatformTypes.h"
#include <excpt.h>
#include "Windows/HideWindowsPlatformTypes.h"

// Context for the async tool execution trampoline.
struct FAsyncToolSEHContext
{
	IClaireonTool* Tool;
	const TSharedPtr<FJsonObject>* Args;
	TPromise<IClaireonTool::FToolResult>* Promise;
	bool* bPromiseFulfilled;
};

namespace ClaireonServerInternal
{

// Trampoline: executes the tool and fulfills the promise.
// C++ temporaries (FToolResult) exist on THIS frame, not the __try frame.
void AsyncToolTrampoline(void* Context)
{
	FAsyncToolSEHContext* Ctx = static_cast<FAsyncToolSEHContext*>(Context);
	Ctx->Promise->SetValue(Ctx->Tool->Execute(*Ctx->Args));
	*Ctx->bPromiseFulfilled = true;
}

// Fulfills the promise with an error FToolResult after an SEH exception.
// Separated from the __try/__except function to avoid C2712 (cannot use __try
// in functions that require object unwinding).
void FulfillPromiseWithError(
	TPromise<IClaireonTool::FToolResult>* Promise,
	bool* bPromiseFulfilled,
	uint32 Code,
	const TCHAR* ExceptionMsg)
{
	if (!*bPromiseFulfilled)
	{
		IClaireonTool::FToolResult ErrorResult;
		ErrorResult.bIsError = true;
		ErrorResult.ErrorMessage = FString::Printf(
			TEXT("FATAL: Caught SEH exception 0x%08X during async tool execution. ")
				TEXT("Editor state may be corrupted -- restart recommended. %s"),
			Code, ExceptionMsg);
		Promise->SetValue(MoveTemp(ErrorResult));
		*bPromiseFulfilled = true;
	}
}

}  // namespace ClaireonServerInternal

// Pure-SEH inner function for async path. No C++ objects with destructors
// anywhere in this function (including __except block) to avoid C2712.
__declspec(noinline) static uint32 ExecuteToolAsyncSEH(
	void (*Fn)(void*),
	void* FnContext,
	bool* bPromiseFulfilled,
	TPromise<IClaireonTool::FToolResult>* Promise,
	TCHAR* OutExceptionMsg,
	int32 ExceptionMsgLen)
{
	__try
	{
		Fn(FnContext);
		return 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		uint32 Code = GetExceptionCode();
		FCString::Strncpy(OutExceptionMsg, GErrorHist, ExceptionMsgLen);

		// Fulfill the promise with an error result if not already fulfilled.
		// Done in a separate function to avoid C++ objects on this frame.
		ClaireonServerInternal::FulfillPromiseWithError(Promise, bPromiseFulfilled, Code, OutExceptionMsg);
		return Code;
	}
}

#endif // PLATFORM_WINDOWS

TSharedPtr<FJsonObject> FClaireonServer::HandleToolsCall(const FMCPRequestContext& Context)
{
	const TSharedPtr<FJsonValue>& Id = Context.Request.Id;
	const TSharedPtr<FJsonObject>& Params = Context.Request.Params;

	// Check user stop mode
	if (bUserStopActive)
	{
		LastToolsCallTime = FPlatformTime::Seconds(); // Reset cooldown on each incoming call
		return FMCPJsonRpcResponse::MakeError(Context.Request.Id, -32000,
			TEXT("User manually requested stop. Wait for the user to resume."));
	}

	if (!Params.IsValid())
	{
		return FMCPJsonRpcResponse::MakeError(Id, FMCPJsonRpcResponse::InvalidParams,
			TEXT("Missing params for tools/call"));
	}

	FString ToolName;
	if (!Params->TryGetStringField(TEXT("name"), ToolName))
	{
		return FMCPJsonRpcResponse::MakeError(Id, FMCPJsonRpcResponse::InvalidParams,
			TEXT("Missing 'name' in tools/call params"));
	}

	TSharedPtr<IClaireonTool>* FoundTool = Tools.Find(ToolName);
	if (!FoundTool || !FoundTool->IsValid())
	{
		return FMCPJsonRpcResponse::MakeError(Id, FMCPJsonRpcResponse::InvalidParams,
			FString::Printf(TEXT("Unknown tool: %s"), *ToolName));
	}

	// Reject tools that are not MCP-visible (they must be called via python_execute)
	if (!MCPVisibleTools.Contains(ToolName))
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP] Rejected direct MCP call to non-visible tool: %s"), *ToolName);
		return FMCPJsonRpcResponse::MakeError(Id, FMCPJsonRpcResponse::MethodNotFound,
			FString::Printf(TEXT("Tool '%s' is not directly callable via MCP. ")
							TEXT("Call it from Python via `claireon.%s(...)` inside python_execute."),
				*ToolName, *ToolName));
	}

	// Block tools that require no PIE session
	if ((*FoundTool)->RequiresNoPIE() && GEditor && GEditor->IsPlaySessionInProgress())
	{
		return FMCPJsonRpcResponse::MakeError(Id, -32000,
			TEXT("This tool cannot be used while Play In Editor (PIE) is running. "
				 "Stop PIE first with editor.pie.stop, then retry."));
	}

	// Block tools that require an editor world
	if ((*FoundTool)->RequiresEditorWorld())
	{
		FClaireonWorldReadinessResult Result = FClaireonWorldReadiness::Check();
		if (!Result.bReady)
		{
			return FMCPJsonRpcResponse::MakeError(Id, -32000, Result.Message + TEXT(" ") + Result.RecoveryHint);
		}
	}

	TSharedPtr<FJsonObject> Arguments;
	if (Params->HasField(TEXT("arguments")))
	{
		Arguments = Params->GetObjectField(TEXT("arguments"));
	}
	else
	{
		Arguments = MakeShared<FJsonObject>();
	}

	// Check for suppress_output flag before execution
	bool bSuppressOutput = false;
	if (Arguments.IsValid())
	{
		Arguments->TryGetBoolField(TEXT("suppress_output"), bSuppressOutput);
	}

	UE_LOG(LogClaireon, Log, TEXT("[MCP] Calling tool: %s%s"), *ToolName,
		bSuppressOutput ? TEXT(" (suppress_output)") : TEXT(""));

	// Execute on game thread with serialization guard
	IClaireonTool::FToolResult ToolResult;
	{
		FScopeLock Lock(&GameThreadCriticalSection);

		if (IsInGameThread())
		{
			FClaireonSafeExecResult SafeResult = ClaireonSafeExec::ExecuteTool(FoundTool->Get(), Arguments);
			ToolResult = MoveTemp(SafeResult.ToolResult);
		}
		else
		{
			// Marshal to game thread
			TPromise<IClaireonTool::FToolResult> Promise;
			TFuture<IClaireonTool::FToolResult> Future = Promise.GetFuture();

			IClaireonTool* ToolPtr = FoundTool->Get();
			TSharedPtr<FJsonObject> ArgsCopy = Arguments;
			std::atomic<uint32> AsyncExceptionCode{ 0 };

#if PLATFORM_WINDOWS
			AsyncTask(ENamedThreads::GameThread, [ToolPtr, &ArgsCopy, &Promise, &AsyncExceptionCode]()
			{
				TCHAR ExMsg[2048] = {};
				bool bPromiseFulfilled = false;
				FAsyncToolSEHContext Ctx;
				Ctx.Tool = ToolPtr;
				Ctx.Args = &ArgsCopy;
				Ctx.Promise = &Promise;
				Ctx.bPromiseFulfilled = &bPromiseFulfilled;
				AsyncExceptionCode.store(
					ExecuteToolAsyncSEH(&ClaireonServerInternal::AsyncToolTrampoline, &Ctx, &bPromiseFulfilled,
						&Promise, ExMsg, UE_ARRAY_COUNT(ExMsg)),
					std::memory_order_release);
			});
#else
			AsyncTask(ENamedThreads::GameThread, [ToolPtr, &ArgsCopy, &Promise, &AsyncExceptionCode]()
			{
				try
				{
					Promise.SetValue(ToolPtr->Execute(ArgsCopy));
				}
				catch (...)
				{
					IClaireonTool::FToolResult ErrorResult;
					ErrorResult.bIsError = true;
					ErrorResult.ErrorMessage = TEXT("FATAL: Caught unknown C++ exception during async tool execution. "
													"Editor state may be corrupted -- restart recommended.");
					Promise.SetValue(MoveTemp(ErrorResult));
					AsyncExceptionCode.store(1, std::memory_order_release);
				}
			});
#endif

			ToolResult = Future.Get();
			if (AsyncExceptionCode.load(std::memory_order_acquire) != 0)
			{
				ClaireonSafeExec::SetCrashFlag();
			}
		}
	}

	// Drain deferred world-transition actions (map open, PIE start/stop, etc.).
	// MCP direct tool calls bypass Python, so this dispatch happens here as well
	// as in ExecutePython's post-execution hook.
	if (FClaireonBridge::HasDeferredActions())
	{
		// Auto-save before world-transition actions (map load, PIE, etc.)
		FClaireonAutoSave::SaveIfNeeded(/*bIsPythonExecution=*/false);

		TArray<FClaireonDeferredAction> Actions = FClaireonBridge::DrainDeferredActions();
		for (const FClaireonDeferredAction& Action : Actions)
		{
			FClaireonSafeActionResult ActionResult = ClaireonSafeExec::ExecuteAction([&Action]()
			{
				switch (Action.Type)
				{
					case EClaireonDeferredActionType::LoadMap:
						ClaireonTool_MapOpen::ExecuteDeferredLoadMap(Action.Payload);
						break;
					case EClaireonDeferredActionType::PIEStart:
						ClaireonTool_PIEStart::ExecuteDeferredPIEStart(Action.Payload);
						break;
					case EClaireonDeferredActionType::PIEStop:
						ClaireonTool_PIEStop::ExecuteDeferredPIEStop();
						break;
					case EClaireonDeferredActionType::LiveCodingReload:
						ClaireonTool_LiveCodingReload::ExecuteDeferredLiveCodingReload(Action.Payload);
						break;
					case EClaireonDeferredActionType::DuplicateAndOpenMap:
						ClaireonTool_MapDuplicate::ExecuteDeferredDuplicateAndOpenMap(Action.Payload);
						break;
				}
			});

			if (ActionResult.bCaughtFatalException)
			{
				UE_LOG(LogClaireon, Error, TEXT("Deferred action %d crashed: %s"),
					(int32)Action.Type, *ActionResult.ExceptionDescription);
				break;
			}
		}
	}

	// Generic suppress_output fallback: if the tool didn't handle it internally,
	// replace verbose output with a minimal acknowledgment
	if (bSuppressOutput && !ToolResult.bIsError)
	{
		ToolResult = IClaireonTool::MakeSuccessResult(nullptr, TEXT("ok"));
	}

	// Track session tool call counts for feedback nudge
	++SessionToolCallCount;
	if (ToolResult.bIsError)
	{
		++SessionToolErrorCount;
	}
	if (ToolName == TEXT("feedback_submit"))
	{
		bFeedbackSubmittedThisSession = true;
	}

	// Build MCP tool result — all tools return structured FToolResult fields.
	// The MCP HTTP path regenerates XML at the transport boundary.
	FString ContentText = FClaireonXmlFormatter::FormatExecuteResult(ToolResult);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> ContentObj = MakeShared<FJsonObject>();
	ContentObj->SetStringField(TEXT("type"), TEXT("text"));
	ContentObj->SetStringField(TEXT("text"), ContentText);
	TArray<TSharedPtr<FJsonValue>> Content;
	Content.Add(MakeShared<FJsonValueObject>(ContentObj));

	// Inject feedback nudge once thresholds are hit
	if (!bFeedbackNudgeSent && !bFeedbackSubmittedThisSession
		&& (SessionToolErrorCount >= FeedbackNudgeErrorThreshold
			|| SessionToolCallCount >= FeedbackNudgeTotalThreshold))
	{
		bFeedbackNudgeSent = true;
		TSharedPtr<FJsonObject> NudgeObj = MakeShared<FJsonObject>();
		NudgeObj->SetStringField(TEXT("type"), TEXT("text"));
		NudgeObj->SetStringField(TEXT("text"),
			TEXT("<system-reminder>You have been using the MCP tools for a while. "
				 "Before this session ends, please call feedback_submit with "
				 "observations about tool ergonomics, workflow friction, bugs encountered, "
				 "or feature suggestions. One submission is enough.</system-reminder>"));
		Content.Add(MakeShared<FJsonValueObject>(NudgeObj));
		UE_LOG(LogClaireon, Display, TEXT("[MCP] Feedback nudge injected (calls=%d, errors=%d)"),
			SessionToolCallCount, SessionToolErrorCount);
	}

	Result->SetArrayField(TEXT("content"), Content);

	if (ToolResult.bIsError)
	{
		Result->SetBoolField(TEXT("isError"), true);
	}

	return FMCPJsonRpcResponse::MakeResult(Id, Result);
}

TSharedPtr<FJsonObject> FClaireonServer::HandlePing(const FMCPRequestContext& Context)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	return FMCPJsonRpcResponse::MakeResult(Context.Request.Id, Result);
}

bool FClaireonServer::ValidateRequestHeaders(const FHttpServerRequest& Request) const
{
	// Loopback enforcement lives at the request layer because
	// FHttpServerModule::GetHttpRouter(Port, bFailOnBindFailure) does not
	// expose a bind-address parameter -- the listener always accepts any
	// interface. See Engine/Source/Runtime/Online/HTTPServer/Public/
	// HttpServerModule.h:55. The listener therefore could, in principle,
	// receive a non-loopback request; we defend in depth via:
	//   (1) Origin and Host header checks below, AND
	//   (2) the bearer-token gate (ValidateBearerToken).
	//
	// UE 5.5's FHttpServerRequest does not expose a stable peer-address
	// accessor (PeerAddress is not part of the public headers used by this
	// plugin). Token gating + Origin/Host is therefore the security
	// boundary. If/when a peer-address accessor becomes available this
	// method should add a reject-if-not-127.0.0.1/::1 check.

	// Check Origin header if present — must be localhost.
	// Do NOT reject requests missing the Origin header (MCP clients are not browsers).
	for (const auto& Header : Request.Headers)
	{
		if (Header.Key.Equals(TEXT("Origin"), ESearchCase::IgnoreCase))
		{
			const TArray<FString>& Values = Header.Value;
			if (Values.Num() > 0)
			{
				const FString& Origin = Values[0];
				if (!Origin.Contains(TEXT("localhost")) && !Origin.Contains(TEXT("127.0.0.1")))
				{
					UE_LOG(LogClaireon, Warning, TEXT("[MCP] Rejected request with non-localhost Origin: %s"), *Origin);
					return false;
				}
			}
			break;
		}
	}

	// Check Host header if present — must be localhost.
	for (const auto& Header : Request.Headers)
	{
		if (Header.Key.Equals(TEXT("Host"), ESearchCase::IgnoreCase))
		{
			const TArray<FString>& Values = Header.Value;
			if (Values.Num() > 0)
			{
				const FString& Host = Values[0];
				if (!Host.Contains(TEXT("localhost")) && !Host.Contains(TEXT("127.0.0.1")))
				{
					UE_LOG(LogClaireon, Warning, TEXT("[MCP] Rejected request with non-localhost Host: %s"), *Host);
					return false;
				}
			}
			break;
		}
	}

	return true;
}

bool FClaireonServer::ValidateBearerToken(const FHttpServerRequest& Request) const
{
	// Gating disabled -- direct-connect dev path, accept all (already logged at Start).
	if (SessionToken.IsEmpty())
	{
		return true;
	}

	FString AuthHeader;
	for (const auto& Header : Request.Headers)
	{
		if (Header.Key.Equals(TEXT("Authorization"), ESearchCase::IgnoreCase))
		{
			if (Header.Value.Num() > 0)
			{
				AuthHeader = Header.Value[0];
			}
			break;
		}
	}

	if (AuthHeader.IsEmpty())
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP] Rejected request: missing Authorization header"));
		return false;
	}

	const FString BearerPrefix = TEXT("Bearer ");
	if (!AuthHeader.StartsWith(BearerPrefix, ESearchCase::IgnoreCase))
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP] Rejected request: Authorization not Bearer scheme"));
		return false;
	}

	const FString Presented = AuthHeader.RightChop(BearerPrefix.Len());

	// Constant-time compare. Always walk the full expected-token length so
	// that timing does not leak how many characters matched. Length mismatch
	// still short-circuits because a shorter presented token cannot match,
	// but the mismatch bit is folded in.
	if (Presented.Len() != SessionToken.Len())
	{
		// Do not log the presented token (could be a close approximation of
		// the real one). Logging that there was a mismatch is enough.
		return false;
	}

	uint32 Diff = 0;
	const TCHAR* A = *Presented;
	const TCHAR* B = *SessionToken;
	for (int32 Idx = 0; Idx < SessionToken.Len(); ++Idx)
	{
		Diff |= static_cast<uint32>(A[Idx] ^ B[Idx]);
	}
	return Diff == 0;
}

FString FClaireonServer::SerializeJson(const TSharedPtr<FJsonObject>& JsonObject)
{
	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
	return OutputString;
}

void FClaireonServer::WritePortFile() const
{
	FString PortFilePath = GetPortFilePath();
	FString TempFilePath = PortFilePath + TEXT(".tmp");

	// Ensure parent directory exists (Saved/Claireon/)
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(PortFilePath), /*Tree=*/true);

	// Write JSON with port and PID
	TSharedPtr<FJsonObject> PortInfo = MakeShared<FJsonObject>();
	PortInfo->SetNumberField(TEXT("port"), BoundPort);
	PortInfo->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());

	FString JsonContent = SerializeJson(PortInfo);

	// Atomic write: write to temp file, then rename
	if (FFileHelper::SaveStringToFile(JsonContent, *TempFilePath))
	{
		IFileManager::Get().Move(*PortFilePath, *TempFilePath, /*bReplace=*/true);
		UE_LOG(LogClaireon, Log, TEXT("[MCP] Port file written: %s"), *PortFilePath);
	}
	else
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP] Failed to write port file: %s"), *TempFilePath);
	}
}

void FClaireonServer::DeletePortFile() const
{
	FString PortFilePath = GetPortFilePath();
	if (FPaths::FileExists(PortFilePath))
	{
		IFileManager::Get().Delete(*PortFilePath);
		UE_LOG(LogClaireon, Log, TEXT("[MCP] Port file deleted: %s"), *PortFilePath);
	}
}

FString FClaireonServer::GetPortFilePath() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Claireon"), TEXT("MCPServer.json"));
}

// ---------------------------------------------------------------------------
// Content-preview extraction helper
// ---------------------------------------------------------------------------
static FString ExtractContentPreviewFromEntry(const FString& RequestBody, const FString& ToolName)
{
	if (RequestBody.IsEmpty())
	{
		return ToolName;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestBody);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return ToolName;
	}

	// Navigate params.arguments
	const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
	if (!Root->TryGetObjectField(TEXT("params"), ParamsPtr))
	{
		return ToolName;
	}
	const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
	if (!(*ParamsPtr)->TryGetObjectField(TEXT("arguments"), ArgsPtr))
	{
		return ToolName;
	}

	// Priority key list for meaningful content
	static const TCHAR* PriorityKeys[] = {
		TEXT("code"), TEXT("query"), TEXT("text"), TEXT("content"),
		TEXT("message"), TEXT("feedback_text"), TEXT("prompt"), TEXT("expression")
	};

	FString Candidate;
	for (const TCHAR* Key : PriorityKeys)
	{
		FString Val;
		if ((*ArgsPtr)->TryGetStringField(Key, Val) && !Val.TrimStartAndEnd().IsEmpty())
		{
			Candidate = Val;
			break;
		}
	}

	// Fallback: first string-valued field
	if (Candidate.IsEmpty())
	{
		for (const auto& Pair : (*ArgsPtr)->Values)
		{
			FString Val;
			if (Pair.Value.IsValid() &&
				Pair.Value->TryGetString(Val) &&
				!Val.TrimStartAndEnd().IsEmpty())
			{
				Candidate = Val;
				break;
			}
		}
	}

	if (Candidate.IsEmpty())
	{
		return ToolName;
	}

	// Take first non-empty line, cap at 120 chars
	TArray<FString> Lines;
	Candidate.ParseIntoArrayLines(Lines, false);
	for (const FString& Line : Lines)
	{
		FString Trimmed = Line.TrimStartAndEnd();
		if (!Trimmed.IsEmpty())
		{
			return Trimmed.Left(120);
		}
	}
	return ToolName;
}

void FClaireonServer::AddDiagnosticsEntry(FMCPDiagnosticsEntry&& Entry)
{
	++TotalRequestCount;
	TotalDurationMs += Entry.DurationMs;
	if (Entry.bIsError)
	{
		++ErrorCount;
	}

	// Populate derived display fields if not already set
	if (Entry.ContentPreview.IsEmpty())
	{
		Entry.ContentPreview = ExtractContentPreviewFromEntry(Entry.RequestBody, Entry.ToolName);
	}
	if (!Entry.bIsFeedbackCall)
	{
		Entry.bIsFeedbackCall =
			Entry.ToolName.Contains(TEXT("feedback"), ESearchCase::IgnoreCase) ||
			Entry.RequestBody.Contains(TEXT("feedback("), ESearchCase::IgnoreCase);
	}

	if (DiagnosticsEntries.Num() >= MaxDiagnosticsEntries)
	{
		DiagnosticsEntries.RemoveAt(0);
	}
	DiagnosticsEntries.Add(MoveTemp(Entry));

	OnDiagnosticsEntryAdded.Broadcast(DiagnosticsEntries.Last());
}

void FClaireonServer::PostSystemMessage(const FString& Text)
{
	FMCPDiagnosticsEntry Entry;
	Entry.Timestamp     = FDateTime::Now();
	Entry.Method        = TEXT("system");
	Entry.ContentPreview= Text;
	Entry.bIsSystemMessage = true;

	// System messages do not count as requests or errors -- bypass AddDiagnosticsEntry
	// (which increments TotalRequestCount) and write directly to the ring buffer.
	if (DiagnosticsEntries.Num() >= MaxDiagnosticsEntries)
	{
		DiagnosticsEntries.RemoveAt(0);
	}
	DiagnosticsEntries.Add(MoveTemp(Entry));
	OnDiagnosticsEntryAdded.Broadcast(DiagnosticsEntries.Last());
}

TSharedPtr<FJsonObject> FClaireonServer::HandlePromptsList(const FMCPRequestContext& Context)
{
	TArray<TSharedPtr<FJsonValue>> PromptArray;

	// 1. File-backed prompts loaded from Content/MCP/Prompts/. Sorted for stable output.
	{
		TArray<FString> SortedNames;
		LoadedPrompts.GetKeys(SortedNames);
		SortedNames.Sort();

		for (const FString& Name : SortedNames)
		{
			const FPromptTemplate& Tmpl = LoadedPrompts[Name];
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Name);
			Entry->SetStringField(TEXT("description"), Tmpl.Description);
			PromptArray.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	// 2. Dynamic prompts generated from the live tool registry: one per tool category.
	//    These are not file-backed because they enumerate runtime registrations.
	{
		TSet<FString> Categories;
		for (const auto& Pair : Tools)
		{
			if (Pair.Value.IsValid())
			{
				FString Category = Pair.Value->GetCategory();
				if (Category != TEXT("all"))
				{
					Categories.Add(Category);
				}
			}
		}

		TArray<FString> SortedCategories = Categories.Array();
		SortedCategories.Sort();

		for (const FString& Category : SortedCategories)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), FString::Printf(TEXT("domain/%s"), *Category));
			Entry->SetStringField(TEXT("description"), FString::Printf(TEXT("Tools and type signatures for the %s category"), *Category));
			PromptArray.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("prompts"), PromptArray);
	return FMCPJsonRpcResponse::MakeResult(Context.Request.Id, Result);
}

TSharedPtr<FJsonObject> FClaireonServer::HandlePromptsGet(const FMCPRequestContext& Context)
{
	if (!Context.Request.Params.IsValid() || !Context.Request.Params->HasField(TEXT("name")))
	{
		return FMCPJsonRpcResponse::MakeError(Context.Request.Id, FMCPJsonRpcResponse::InvalidParams,
			TEXT("Missing required parameter: name"));
	}

	FString Name = Context.Request.Params->GetStringField(TEXT("name"));
	FString PromptDescription;
	FString PromptText;
	FString PromptRole = TEXT("user");

	// 1. File-backed prompts (static text with {{placeholder}} substitution).
	if (const FPromptTemplate* Tmpl = LoadedPrompts.Find(Name))
	{
		PromptDescription = Tmpl->Description;
		PromptRole = Tmpl->Role;
		PromptText = SubstitutePlaceholders(Tmpl->TextTemplate, BuildRuntimeVariables());
	}
	// 2. Dynamic domain/{category}: enumerate the tool registry.
	else if (Name.StartsWith(TEXT("domain/")))
	{
		FString Category = Name.Mid(7);
		PromptDescription = FString::Printf(TEXT("Tools and type signatures for the %s category"), *Category);

		FString ToolListing;
		int32 MatchCount = 0;
		for (const auto& Pair : Tools)
		{
			if (Pair.Value.IsValid() && Pair.Value->GetCategory() == Category)
			{
				FString TypeSig = FClaireonXmlFormatter::GenerateTypeSignature(Pair.Value->GetName(), Pair.Value->GetInputSchema());
				ToolListing += FString::Printf(TEXT("%s\n  %s\n  %s\n"),
					*Pair.Value->GetName(), *Pair.Value->GetBriefDescription(), *TypeSig);
				++MatchCount;
			}
		}

		if (MatchCount == 0)
		{
			return FMCPJsonRpcResponse::MakeError(Context.Request.Id, FMCPJsonRpcResponse::InvalidParams,
				FString::Printf(TEXT("Unknown prompt: domain/%s"), *Category));
		}

		PromptText = ToolListing;
	}
	else
	{
		return FMCPJsonRpcResponse::MakeError(Context.Request.Id, FMCPJsonRpcResponse::InvalidParams,
			FString::Printf(TEXT("Unknown prompt: %s"), *Name));
	}

	// Build MCP prompt response
	TSharedPtr<FJsonObject> Content = MakeShared<FJsonObject>();
	Content->SetStringField(TEXT("type"), TEXT("text"));
	Content->SetStringField(TEXT("text"), PromptText);

	TSharedPtr<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("role"), PromptRole);
	Message->SetObjectField(TEXT("content"), Content);

	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeShared<FJsonValueObject>(Message));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("description"), PromptDescription);
	Result->SetArrayField(TEXT("messages"), Messages);

	return FMCPJsonRpcResponse::MakeResult(Context.Request.Id, Result);
}

TSharedPtr<FJsonObject> FClaireonServer::HandleResourcesList(const FMCPRequestContext& Context)
{
	TArray<TSharedPtr<FJsonValue>> ResourceArray;

	// 1. File-backed resources loaded from Content/MCP/Resources/. Sorted for stable output.
	{
		TArray<FString> SortedUris;
		LoadedResources.GetKeys(SortedUris);
		SortedUris.Sort();

		for (const FString& Uri : SortedUris)
		{
			const FResourceTemplate& Tmpl = LoadedResources[Uri];
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("uri"), Uri);
			Entry->SetStringField(TEXT("name"), Tmpl.Name);
			Entry->SetStringField(TEXT("description"), Tmpl.Description);
			Entry->SetStringField(TEXT("mimeType"), Tmpl.MimeType);
			ResourceArray.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	// 2. Dynamic resources generated from the live tool registry.
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("uri"), TEXT("claireon://tools/all"));
		Entry->SetStringField(TEXT("name"), TEXT("All Tools"));
		Entry->SetStringField(TEXT("description"), TEXT("Full tool catalog: all tools, all categories, brief descriptions"));
		Entry->SetStringField(TEXT("mimeType"), TEXT("text/plain"));
		ResourceArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("resources"), ResourceArray);
	return FMCPJsonRpcResponse::MakeResult(Context.Request.Id, Result);
}

TSharedPtr<FJsonObject> FClaireonServer::HandleResourcesRead(const FMCPRequestContext& Context)
{
	if (!Context.Request.Params.IsValid() || !Context.Request.Params->HasField(TEXT("uri")))
	{
		return FMCPJsonRpcResponse::MakeError(Context.Request.Id, FMCPJsonRpcResponse::InvalidParams,
			TEXT("Missing required parameter: uri"));
	}

	FString Uri = Context.Request.Params->GetStringField(TEXT("uri"));
	FString ContentText;
	FString MimeType = TEXT("text/plain");

	// 1. File-backed resources (static text with {{placeholder}} substitution).
	if (const FResourceTemplate* Tmpl = LoadedResources.Find(Uri))
	{
		MimeType = Tmpl->MimeType;
		ContentText = SubstitutePlaceholders(Tmpl->TextTemplate, BuildRuntimeVariables());
	}
	// 2. Dynamic claireon://tools/all: enumerate the full tool registry grouped by category.
	else if (Uri == TEXT("claireon://tools/all"))
	{
		TMap<FString, TArray<TSharedPtr<IClaireonTool>>> GroupedTools;
		for (const auto& Pair : Tools)
		{
			if (Pair.Value.IsValid())
			{
				GroupedTools.FindOrAdd(Pair.Value->GetCategory()).Add(Pair.Value);
			}
		}

		TArray<FString> SortedCategories;
		GroupedTools.GetKeys(SortedCategories);
		SortedCategories.Sort();

		for (const FString& Category : SortedCategories)
		{
			ContentText += FString::Printf(TEXT("[%s]\n"), *Category);
			for (const TSharedPtr<IClaireonTool>& Tool : GroupedTools[Category])
			{
				FString TypeSig = FClaireonXmlFormatter::GenerateTypeSignature(Tool->GetName(), Tool->GetInputSchema());
				ContentText += FString::Printf(TEXT("  %s - %s\n    %s\n"),
					*Tool->GetName(), *Tool->GetBriefDescription(), *TypeSig);
			}
		}
	}
	// 3. Dynamic claireon://tools/{category}: template-expanded per category.
	else if (Uri.StartsWith(TEXT("claireon://tools/")))
	{
		FString Category = Uri.Mid(15);

		int32 MatchCount = 0;
		ContentText += FString::Printf(TEXT("[%s]\n"), *Category);
		for (const auto& Pair : Tools)
		{
			if (Pair.Value.IsValid() && Pair.Value->GetCategory() == Category)
			{
				FString TypeSig = FClaireonXmlFormatter::GenerateTypeSignature(Pair.Value->GetName(), Pair.Value->GetInputSchema());
				ContentText += FString::Printf(TEXT("  %s - %s\n    %s\n"),
					*Pair.Value->GetName(), *Pair.Value->GetBriefDescription(), *TypeSig);
				++MatchCount;
			}
		}

		if (MatchCount == 0)
		{
			return FMCPJsonRpcResponse::MakeError(Context.Request.Id, FMCPJsonRpcResponse::InvalidParams,
				FString::Printf(TEXT("Unknown tool category: %s"), *Category));
		}
	}
	else
	{
		return FMCPJsonRpcResponse::MakeError(Context.Request.Id, FMCPJsonRpcResponse::InvalidParams,
			FString::Printf(TEXT("Unknown resource: %s"), *Uri));
	}

	// Build MCP resource response
	TSharedPtr<FJsonObject> ContentObj = MakeShared<FJsonObject>();
	ContentObj->SetStringField(TEXT("uri"), Uri);
	ContentObj->SetStringField(TEXT("mimeType"), MimeType);
	ContentObj->SetStringField(TEXT("text"), ContentText);

	TArray<TSharedPtr<FJsonValue>> Contents;
	Contents.Add(MakeShared<FJsonValueObject>(ContentObj));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("contents"), Contents);
	return FMCPJsonRpcResponse::MakeResult(Context.Request.Id, Result);
}

TSharedPtr<FJsonObject> FClaireonServer::HandleResourceTemplatesList(const FMCPRequestContext& Context)
{
	TSharedPtr<FJsonObject> Template = MakeShared<FJsonObject>();
	Template->SetStringField(TEXT("uriTemplate"), TEXT("claireon://tools/{category}"));
	Template->SetStringField(TEXT("name"), TEXT("Tools by Category"));
	Template->SetStringField(TEXT("description"), TEXT("Tools filtered by category"));
	Template->SetStringField(TEXT("mimeType"), TEXT("text/plain"));

	TArray<TSharedPtr<FJsonValue>> TemplateArray;
	TemplateArray.Add(MakeShared<FJsonValueObject>(Template));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("resourceTemplates"), TemplateArray);
	return FMCPJsonRpcResponse::MakeResult(Context.Request.Id, Result);
}

void FClaireonServer::ClearDiagnostics()
{
	DiagnosticsEntries.Empty();
}

void FClaireonServer::LoadMCPContent()
{
	LoadedPrompts.Empty();
	LoadedResources.Empty();

	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Claireon"));
	if (!Plugin.IsValid())
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP] Claireon plugin not found -- no content loaded"));
		return;
	}

	const FString ContentRoot = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Content"), TEXT("MCP"));
	LoadPromptsFromDirectory(FPaths::Combine(ContentRoot, TEXT("Prompts")));
	LoadResourcesFromDirectory(FPaths::Combine(ContentRoot, TEXT("Resources")));
	LoadInstructionsFromDirectory(FPaths::Combine(ContentRoot, TEXT("Instructions")));

	UE_LOG(LogClaireon, Display, TEXT("[MCP] Loaded %d prompt(s) and %d resource(s) from %s"),
		LoadedPrompts.Num(), LoadedResources.Num(), *ContentRoot);
}

void FClaireonServer::LoadPromptsFromDirectory(const FString& Directory)
{
	if (!FPaths::DirectoryExists(Directory))
	{
		UE_LOG(LogClaireon, Log, TEXT("[MCP] Prompts directory not found: %s"), *Directory);
		return;
	}

	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.json"), /*bFiles=*/true, /*bDirs=*/false);

	for (const FString& AbsPath : Files)
	{
		FString Contents;
		if (!FFileHelper::LoadFileToString(Contents, *AbsPath))
		{
			UE_LOG(LogClaireon, Warning, TEXT("[MCP] Failed to read prompt file: %s"), *AbsPath);
			continue;
		}

		TSharedPtr<FJsonObject> RootObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
		if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
		{
			UE_LOG(LogClaireon, Warning, TEXT("[MCP] Failed to parse prompt JSON: %s"), *AbsPath);
			continue;
		}

		// Key = relative path minus extension, using forward slashes (e.g. "workflow/bulk-edit").
		FString RelPath = AbsPath;
		FPaths::MakePathRelativeTo(RelPath, *(Directory / TEXT("")));
		FString PromptName = FPaths::GetPath(RelPath) / FPaths::GetBaseFilename(RelPath);
		PromptName.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (PromptName.StartsWith(TEXT("/")))
		{
			PromptName.RightChopInline(1, /*bAllowShrinking=*/EAllowShrinking::No);
		}

		FPromptTemplate Tmpl;
		Tmpl.Description = RootObject->GetStringField(TEXT("description"));
		Tmpl.Role = RootObject->HasField(TEXT("role")) ? RootObject->GetStringField(TEXT("role")) : TEXT("user");
		Tmpl.TextTemplate = RootObject->GetStringField(TEXT("text"));
		Tmpl.SourcePath = AbsPath;

		LoadedPrompts.Add(PromptName, MoveTemp(Tmpl));
	}
}

void FClaireonServer::LoadResourcesFromDirectory(const FString& Directory)
{
	if (!FPaths::DirectoryExists(Directory))
	{
		UE_LOG(LogClaireon, Log, TEXT("[MCP] Resources directory not found: %s"), *Directory);
		return;
	}

	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.json"), /*bFiles=*/true, /*bDirs=*/false);

	for (const FString& AbsPath : Files)
	{
		FString Contents;
		if (!FFileHelper::LoadFileToString(Contents, *AbsPath))
		{
			UE_LOG(LogClaireon, Warning, TEXT("[MCP] Failed to read resource file: %s"), *AbsPath);
			continue;
		}

		TSharedPtr<FJsonObject> RootObject;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
		if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
		{
			UE_LOG(LogClaireon, Warning, TEXT("[MCP] Failed to parse resource JSON: %s"), *AbsPath);
			continue;
		}

		// URI = "claireon://" + relative path minus extension, using forward slashes.
		FString RelPath = AbsPath;
		FPaths::MakePathRelativeTo(RelPath, *(Directory / TEXT("")));
		FString ResourcePath = FPaths::GetPath(RelPath) / FPaths::GetBaseFilename(RelPath);
		ResourcePath.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (ResourcePath.StartsWith(TEXT("/")))
		{
			ResourcePath.RightChopInline(1, /*bAllowShrinking=*/EAllowShrinking::No);
		}
		const FString Uri = FString::Printf(TEXT("claireon://%s"), *ResourcePath);

		FResourceTemplate Tmpl;
		Tmpl.Name = RootObject->GetStringField(TEXT("name"));
		Tmpl.Description = RootObject->GetStringField(TEXT("description"));
		Tmpl.MimeType = RootObject->HasField(TEXT("mimeType")) ? RootObject->GetStringField(TEXT("mimeType")) : TEXT("text/plain");
		Tmpl.TextTemplate = RootObject->GetStringField(TEXT("text"));
		Tmpl.SourcePath = AbsPath;

		LoadedResources.Add(Uri, MoveTemp(Tmpl));
	}
}

void FClaireonServer::LoadInstructionsFromDirectory(const FString& Directory)
{
	if (!FPaths::DirectoryExists(Directory))
	{
		UE_LOG(LogClaireon, Log, TEXT("[MCP] Instructions directory not found: %s"), *Directory);
		return;
	}

	TArray<FString> Files;
	IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.md"), /*bFiles=*/true, /*bDirs=*/false);

	int32 NumPrompts = 0;
	int32 NumResources = 0;

	for (const FString& AbsPath : Files)
	{
		FString Contents;
		if (!FFileHelper::LoadFileToString(Contents, *AbsPath))
		{
			UE_LOG(LogClaireon, Warning, TEXT("[MCP] Failed to read instruction file: %s"), *AbsPath);
			continue;
		}

		// --- Parse YAML frontmatter (flat key: value pairs between two "---" lines) ---
		TMap<FString, FString> FM;
		FString Body;

		// Accept both LF and CRLF after the opening dashes.
		const bool bLF   = Contents.StartsWith(TEXT("---\n"));
		const bool bCRLF = !bLF && Contents.StartsWith(TEXT("---\r\n"));
		if (bLF || bCRLF)
		{
			const int32 HeaderLen = bLF ? 4 : 5; // "---\n" or "---\r\n"
			// Find the closing "---" on its own line.
			const int32 ClosePos = Contents.Find(TEXT("\n---"), ESearchCase::CaseSensitive,
				ESearchDir::FromStart, HeaderLen);
			if (ClosePos != INDEX_NONE)
			{
				const FString FMBlock = Contents.Mid(HeaderLen, ClosePos - HeaderLen);

				// Skip past "\n---" (4 chars) plus an optional trailing newline.
				int32 BodyStart = ClosePos + 4;
				if (BodyStart < Contents.Len() && Contents[BodyStart] == TEXT('\r')) ++BodyStart;
				if (BodyStart < Contents.Len() && Contents[BodyStart] == TEXT('\n')) ++BodyStart;
				Body = Contents.Mid(BodyStart);

				TArray<FString> Lines;
				FMBlock.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);
				for (const FString& Line : Lines)
				{
					int32 ColonIdx;
					if (Line.FindChar(TEXT(':'), ColonIdx) && ColonIdx > 0)
					{
						const FString Key   = Line.Left(ColonIdx).TrimStartAndEnd().ToLower();
						const FString Value = Line.Mid(ColonIdx + 1).TrimStartAndEnd();
						if (!Key.IsEmpty() && !Value.IsEmpty())
						{
							FM.Add(Key, Value);
						}
					}
				}
			}
		}

		if (FM.IsEmpty())
		{
			UE_LOG(LogClaireon, Warning,
				TEXT("[MCP] Instruction file has no YAML frontmatter, skipping: %s"), *AbsPath);
			continue;
		}
		if (Body.IsEmpty())
		{
			// Malformed closing ---; treat entire file as body so we don't silently drop content.
			Body = Contents;
		}

		const FString TypeField = FM.FindRef(TEXT("type"));
		const bool bIsResource  = TypeField.Equals(TEXT("resource"), ESearchCase::IgnoreCase);

		if (bIsResource)
		{
			const FString Uri         = FM.FindRef(TEXT("uri"));
			const FString Name        = FM.FindRef(TEXT("name"));
			const FString Description = FM.FindRef(TEXT("description"));

			if (Uri.IsEmpty())
			{
				UE_LOG(LogClaireon, Warning,
					TEXT("[MCP] Resource instruction missing 'uri' frontmatter field, skipping: %s"), *AbsPath);
				continue;
			}

			FResourceTemplate Tmpl;
			Tmpl.Name        = Name.IsEmpty() ? FPaths::GetBaseFilename(AbsPath) : Name;
			Tmpl.Description = Description;
			Tmpl.MimeType    = TEXT("text/markdown");
			Tmpl.TextTemplate = Body;
			Tmpl.SourcePath  = AbsPath;
			LoadedResources.Add(Uri, MoveTemp(Tmpl));
			++NumResources;
		}
		else // type: prompt (default)
		{
			const FString Name        = FM.FindRef(TEXT("name"));
			const FString Description = FM.FindRef(TEXT("description"));

			if (Name.IsEmpty())
			{
				UE_LOG(LogClaireon, Warning,
					TEXT("[MCP] Prompt instruction missing 'name' frontmatter field, skipping: %s"), *AbsPath);
				continue;
			}

			FPromptTemplate Tmpl;
			Tmpl.Description  = Description;
			Tmpl.Role         = FM.Contains(TEXT("role")) ? FM[TEXT("role")] : TEXT("user");
			Tmpl.TextTemplate = Body;
			Tmpl.SourcePath   = AbsPath;
			LoadedPrompts.Add(Name, MoveTemp(Tmpl));
			++NumPrompts;
		}
	}

	UE_LOG(LogClaireon, Display,
		TEXT("[MCP] Instructions: loaded %d prompt(s) and %d resource(s) from %s"),
		NumPrompts, NumResources, *Directory);
}

FString FClaireonServer::SubstitutePlaceholders(const FString& Template, const TMap<FString, FString>& Variables)
{
	FString Output;
	Output.Reserve(Template.Len());

	int32 Cursor = 0;
	while (Cursor < Template.Len())
	{
		const int32 Open = Template.Find(TEXT("{{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Cursor);
		if (Open == INDEX_NONE)
		{
			Output.Append(*Template + Cursor, Template.Len() - Cursor);
			break;
		}

		// Copy literal text preceding the placeholder.
		Output.Append(*Template + Cursor, Open - Cursor);

		const int32 Close = Template.Find(TEXT("}}"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open + 2);
		if (Close == INDEX_NONE)
		{
			// Unterminated placeholder -- copy the rest literally.
			Output.Append(*Template + Open, Template.Len() - Open);
			break;
		}

		const FString Key = Template.Mid(Open + 2, Close - (Open + 2)).TrimStartAndEnd();
		if (const FString* Value = Variables.Find(Key))
		{
			Output.Append(*Value);
		}
		else
		{
			// Unknown placeholder -- leave token intact for diagnosability.
			Output.Append(*Template + Open, (Close + 2) - Open);
		}

		Cursor = Close + 2;
	}

	return Output;
}

TMap<FString, FString> FClaireonServer::BuildRuntimeVariables() const
{
	TMap<FString, FString> Vars;

	// Project scope.
	Vars.Add(TEXT("project.name"), FApp::GetProjectName());
	Vars.Add(TEXT("project.engine_version"), FApp::GetBuildVersion());

	FString MapName = TEXT("No map loaded");
	if (GEditor)
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (World)
		{
			MapName = World->GetMapName();
		}
	}
	Vars.Add(TEXT("project.current_map"), MapName);

	TArray<FName> ModuleNames;
	FModuleManager::Get().FindModules(TEXT("*"), ModuleNames);
	Vars.Add(TEXT("project.module_count"), FString::FromInt(ModuleNames.Num()));

	// Tool category summary (used by quick-start).
	{
		TMap<FString, int32> CategoryCounts;
		for (const auto& Pair : Tools)
		{
			if (Pair.Value.IsValid())
			{
				CategoryCounts.FindOrAdd(Pair.Value->GetCategory())++;
			}
		}

		TArray<FString> SortedCategories;
		CategoryCounts.GetKeys(SortedCategories);
		SortedCategories.Sort();

		FString CategoryList;
		for (const FString& Category : SortedCategories)
		{
			CategoryList += FString::Printf(TEXT("  - %s: %d tools\n"), *Category, CategoryCounts[Category]);
		}
		Vars.Add(TEXT("project.category_list"), CategoryList);
	}

	// Server scope.
	const FTimespan Uptime = FDateTime::Now() - StartTime;
	Vars.Add(TEXT("server.uptime_hms"), FString::Printf(TEXT("%dh %dm %ds"),
		Uptime.GetHours(), Uptime.GetMinutes(), Uptime.GetSeconds()));
	Vars.Add(TEXT("server.total_requests"), FString::FromInt(TotalRequestCount));
	Vars.Add(TEXT("server.errors"), FString::FromInt(ErrorCount));
	Vars.Add(TEXT("server.active_tools"), FString::FromInt(Tools.Num()));
	Vars.Add(TEXT("server.tool_list_generation"), FString::Printf(TEXT("%u"), ToolListGeneration));

	return Vars;
}
