// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonProxyClient.h"
#include "ClaireonLog.h"
#include "ClaireonProxyConstants.h"

#include "GenericPlatform/GenericPlatformMisc.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

// Registration probe is a simple HTTP POST to http://127.0.0.1:PROXY_REG_PORT/editor/register.
// We reuse FHttpModule for the initial reachability probe and all subsequent
// editor/* calls; it is already a dependency of this plugin and gives us
// ProcessRequestUntilComplete() for synchronous completion inside StartServer
// without blocking the game thread meaningfully (responses are tiny and local).

namespace
{
	/** Compose http://127.0.0.1:PROXY_REG_PORT<path> */
	FString MakeRegUrl(const TCHAR* Path)
	{
		return FString::Printf(
			TEXT("http://%s:%d%s"),
			ClaireonProxy::LoopbackHost,
			ClaireonProxy::PROXY_REG_PORT,
			Path);
	}

	/** Issue a blocking HTTP request; returns true and fills OutBody on 2xx responses. */
	bool SyncHttp(
		const FString& Url,
		const TCHAR* Verb,
		const FString& Body,
		int32 TimeoutSeconds,
		int32& OutStatus,
		FString& OutBody)
	{
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
		HttpRequest->SetURL(Url);
		HttpRequest->SetVerb(Verb);
		if (!Body.IsEmpty())
		{
			HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			HttpRequest->SetContentAsString(Body);
		}
		HttpRequest->SetTimeout(static_cast<float>(TimeoutSeconds));

		if (!HttpRequest->ProcessRequest())
		{
			OutStatus = 0;
			return false;
		}

		// Spin the HTTP module until the request completes. FHttpModule is
		// game-thread friendly; we deliberately keep this short (1-2s typical)
		// to avoid stalling StartupModule.
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (HttpRequest->GetStatus() == EHttpRequestStatus::Processing
			&& FPlatformTime::Seconds() < Deadline)
		{
			FHttpModule::Get().GetHttpManager().Tick(0.0f);
			FPlatformProcess::Sleep(0.005f);
		}

		const EHttpRequestStatus::Type Status = HttpRequest->GetStatus();
		if (Status != EHttpRequestStatus::Succeeded)
		{
			OutStatus = 0;
			return false;
		}

		FHttpResponsePtr Response = HttpRequest->GetResponse();
		if (!Response.IsValid())
		{
			OutStatus = 0;
			return false;
		}

		OutStatus = Response->GetResponseCode();
		OutBody = Response->GetContentAsString();
		return OutStatus >= 200 && OutStatus < 300;
	}

	bool SyncPostJson(
		const FString& Url,
		const FString& Body,
		int32 TimeoutSeconds,
		int32& OutStatus,
		FString& OutBody)
	{
		return SyncHttp(Url, TEXT("POST"), Body, TimeoutSeconds, OutStatus, OutBody);
	}

	bool SyncGet(
		const FString& Url,
		int32 TimeoutSeconds,
		int32& OutStatus,
		FString& OutBody)
	{
		return SyncHttp(Url, TEXT("GET"), FString(), TimeoutSeconds, OutStatus, OutBody);
	}

	/** Extract a boolean field from a JSON reply; returns false if missing or not-bool. */
	bool JsonBool(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		bool Value = false;
		return Obj->TryGetBoolField(Field, Value) && Value;
	}

	FString JsonString(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
	{
		FString Value;
		if (Obj.IsValid())
		{
			Obj->TryGetStringField(Field, Value);
		}
		return Value;
	}

	TSharedPtr<FJsonObject> ParseJson(const FString& Body)
	{
		TSharedPtr<FJsonObject> Out;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
		FJsonSerializer::Deserialize(Reader, Out);
		return Out;
	}
} // namespace

FClaireonProxyClient::FClaireonProxyClient()
{
}

FClaireonProxyClient::~FClaireonProxyClient()
{
	StopHeartbeatTicker();
}

int64 FClaireonProxyClient::GetSelfStartTimeNs()
{
	// D8 newest-wins discriminator. Mirrors claireon_proxy.py
	// _process_start_time_ns() so the (pid, start_time_ns) tuple compares
	// cleanly across editor and proxy on the same host. Returns 0 on
	// failure; the proxy treats 0 as a valid value (pid alone is unique
	// on a live host).
#if PLATFORM_WINDOWS
	HANDLE Self = GetCurrentProcess();
	FILETIME Creation{};
	FILETIME Exit{};
	FILETIME Kernel{};
	FILETIME User{};
	if (!GetProcessTimes(Self, &Creation, &Exit, &Kernel, &User))
	{
		return 0;
	}
	const int64 High = static_cast<int64>(Creation.dwHighDateTime);
	const int64 Low = static_cast<int64>(Creation.dwLowDateTime);
	return (High << 32) | (Low & 0xFFFFFFFFLL);
#elif PLATFORM_LINUX || PLATFORM_MAC
	// /proc/self/stat field 22 (1-indexed) is the process start time in
	// clock ticks since boot. We read the file by hand because
	// FFileHelper::LoadFileToString may chase symlinks differently on
	// Linux's /proc.
	FString Stat;
	if (!FFileHelper::LoadFileToString(Stat, TEXT("/proc/self/stat")))
	{
		return 0;
	}
	// /proc/self/stat: "<pid> (<comm>) <state> ..."; comm can contain
	// spaces and parentheses, so split on the LAST ')' to skip past it.
	const int32 LastParen = Stat.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (LastParen == INDEX_NONE || LastParen + 1 >= Stat.Len())
	{
		return 0;
	}
	const FString After = Stat.Mid(LastParen + 1);
	TArray<FString> Fields;
	After.ParseIntoArray(Fields, TEXT(" "), /*CullEmpty=*/ true);
	// After ")" the next field is "state" (index 0 here); start_time is
	// field 22 in /proc/<pid>/stat (1-indexed) -> index 19 in Fields
	// (0-indexed) since Fields starts at "state".
	if (Fields.Num() <= 19)
	{
		return 0;
	}
	return FCString::Atoi64(*Fields[19]);
#else
	return 0;
#endif
}

FString FClaireonProxyClient::GetRegistrationBaseUrl()
{
	return FString::Printf(TEXT("http://%s:%d"),
		ClaireonProxy::LoopbackHost, ClaireonProxy::PROXY_REG_PORT);
}

bool FClaireonProxyClient::PingProxyHealth()
{
	// Stage 010 auto-promote probe: GET /admin/health with a tight timeout.
	// The response shape (per claireon_proxy.py::handle_admin_health) is
	// {version_hash, pid, worktrees:[...]}. A non-Claireon occupant of 43017
	// will either fail to answer or answer without that shape.
	const FString Url = FString::Printf(
		TEXT("http://%s:%d/admin/health"),
		ClaireonProxy::LoopbackHost,
		ClaireonProxy::PROXY_REG_PORT);

	int32 Status = 0;
	FString Body;
	if (!SyncGet(Url, /*TimeoutSeconds=*/ 1, Status, Body))
	{
		return false;
	}
	if (Status < 200 || Status >= 300)
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Resp = ParseJson(Body);
	if (!Resp.IsValid())
	{
		return false;
	}
	FString VersionHash;
	if (!Resp->TryGetStringField(TEXT("version_hash"), VersionHash) || VersionHash.IsEmpty())
	{
		// Some other listener answered our request shape; do NOT auto-promote.
		return false;
	}
	double PidNum = 0.0;
	if (Resp->TryGetNumberField(TEXT("pid"), PidNum) && PidNum > 0.0)
	{
		CachedProxyPid = static_cast<uint32>(PidNum);
	}
	return true;
}

bool FClaireonProxyClient::EnsureWorktreeBound(const FString& WorktreeRoot)
{
	// Synchronously ask the proxy to bind this worktree's SHA port BEFORE the
	// editor tries to bind it locally. Without this, the proxy only owns
	// PROXY_REG_PORT (43017) and the SHA port is unowned; the editor's
	// TryStart wins the race, takes DirectConnect mode, and a later
	// /admin/ensure_worktree from Initialize-WorktreeMCP.ps1 lands a second
	// loopback bind on top of the editor's wildcard bind (Windows allows it,
	// the proxy then forwards to itself, traffic loops to 10048/10055).
	const FString Url = FString::Printf(
		TEXT("http://%s:%d/admin/ensure_worktree"),
		ClaireonProxy::LoopbackHost,
		ClaireonProxy::PROXY_REG_PORT);

	FString Body;
	{
		TSharedRef<TJsonWriter<TCHAR>> W = TJsonWriterFactory<TCHAR>::Create(&Body);
		W->WriteObjectStart();
		W->WriteValue(TEXT("worktree_root"), WorktreeRoot);
		W->WriteObjectEnd();
		W->Close();
	}

	int32 Status = 0;
	FString Resp;
	if (!SyncPostJson(Url, Body, /*TimeoutSeconds=*/ 3, Status, Resp))
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[MCP Proxy] /admin/ensure_worktree failed: status=%d body=%s"),
			Status, *Resp);
		return false;
	}
	const TSharedPtr<FJsonObject> Parsed = ParseJson(Resp);
	if (!JsonBool(Parsed, TEXT("bound")))
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[MCP Proxy] /admin/ensure_worktree did not report bound=true: %s"),
			*Resp);
		return false;
	}
	return true;
}

FString FClaireonProxyClient::ResolveVendoredPythonExe()
{
	const FString Path = FPaths::EngineDir()
		/ TEXT("Binaries/ThirdParty/Python3/Win64/python.exe");
	const FString Absolute = FPaths::ConvertRelativePathToFull(Path);
	if (FPaths::FileExists(Absolute))
	{
		return Absolute;
	}
	return FString();
}

FString FClaireonProxyClient::ResolveProxyScriptPath()
{
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Claireon"));
	if (!Plugin.IsValid())
	{
		return FString();
	}
	const FString Path = Plugin->GetContentDir() / TEXT("Python/claireon_proxy.py");
	return FPaths::ConvertRelativePathToFull(Path);
}

FString FClaireonProxyClient::ResolveOrCreateRuntimeScriptPath()
{
	const FString SourcePath = ResolveProxyScriptPath();
	if (SourcePath.IsEmpty() || !FPaths::FileExists(SourcePath))
	{
		return FString();
	}

	const FString RuntimeDir = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectSavedDir() / TEXT("Claireon/runtime"));
	const FString RuntimePath = RuntimeDir / TEXT("claireon_proxy.py");

	TArray<uint8> SourceBytes;
	if (!FFileHelper::LoadFileToArray(SourceBytes, *SourcePath))
	{
		UE_LOG(LogClaireon, Error,
			TEXT("[MCP Proxy] Failed to read source script %s"), *SourcePath);
		return FString();
	}

	bool bNeedsCopy = true;
	if (FPaths::FileExists(RuntimePath))
	{
		TArray<uint8> RuntimeBytes;
		if (FFileHelper::LoadFileToArray(RuntimeBytes, *RuntimePath)
			&& RuntimeBytes == SourceBytes)
		{
			bNeedsCopy = false;
		}
	}

	if (bNeedsCopy)
	{
		IFileManager::Get().MakeDirectory(*RuntimeDir, /*Tree=*/ true);
		if (!FFileHelper::SaveArrayToFile(SourceBytes, *RuntimePath))
		{
			UE_LOG(LogClaireon, Error,
				TEXT("[MCP Proxy] Failed to write runtime script %s"), *RuntimePath);
			return FString();
		}
		UE_LOG(LogClaireon, Display,
			TEXT("[MCP Proxy] Copied claireon_proxy.py to runtime dir: %s"), *RuntimePath);
	}

	return RuntimePath;
}

FString FClaireonProxyClient::ComputeProxyScriptHash()
{
	const FString ScriptPath = ResolveProxyScriptPath();
	if (ScriptPath.IsEmpty() || !FPaths::FileExists(ScriptPath))
	{
		return FString();
	}

	TArray<uint8> Contents;
	if (!FFileHelper::LoadFileToArray(Contents, *ScriptPath))
	{
		return FString();
	}

	// SHA-1 of the script bytes; FPlatformMisc::GetSHA256Signature is an
	// unimplemented stub on Windows (it asserts), and SHA-1 is sufficient
	// here because the hash is only a version-tag match with the proxy-side
	// compute_proxy_version_hash() (see Claireon/Content/Python/
	// claireon_proxy.py). No security boundary, just content fingerprinting.
	FSHAHash Hash;
	FSHA1::HashBuffer(Contents.GetData(), Contents.Num(), Hash.Hash);
	return Hash.ToString().ToLower();
}

bool FClaireonProxyClient::SpawnDetachedProxy()
{
	const FString PythonExe = ResolveVendoredPythonExe();
	if (PythonExe.IsEmpty())
	{
		UE_LOG(LogClaireon, Error,
			TEXT("[MCP Proxy] Vendored Python missing at expected path %s. ")
			TEXT("Rebuild the engine or reinstall to restore it."),
			*(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3/Win64/python.exe")));
		return false;
	}

	// Run python from a Saved/ runtime copy of the script, NOT from the
	// git-tracked plugin content. Antivirus holds a scan handle on the file
	// python loads as __main__; if that file is git-tracked, `git pull` then
	// fails to update it. Saved/ is gitignored, so AV's handle never
	// collides with git updates to the source script.
	const FString ScriptPath = ResolveOrCreateRuntimeScriptPath();
	if (ScriptPath.IsEmpty() || !FPaths::FileExists(ScriptPath))
	{
		UE_LOG(LogClaireon, Error,
			TEXT("[MCP Proxy] claireon_proxy.py missing under plugin Content/Python or runtime copy could not be created."));
		return false;
	}

	// Stage 009 (D2/D3): the singleton serves every worktree; --worktree-root
	// is no longer a CLI argument. /admin/ensure_worktree adds worktrees on
	// demand, and /editor/register implicitly creates the slot for the
	// editor's own worktree on first connect.
	const FString CmdLine = FString::Printf(TEXT("\"%s\""), *ScriptPath);

	// Spawn python with its working directory set to the runtime dir under
	// Saved/, NOT the worktree root that the editor inherited as CWD. A
	// process holds a directory handle on its CWD, and any imports / file
	// scans python or AV does relative to that CWD will keep handles inside
	// the worktree -- which can wedge `git pull`. Pointing CWD at the
	// gitignored runtime dir keeps all of that activity outside the
	// git-tracked tree.
	const FString WorkingDir = FPaths::GetPath(ScriptPath);

	uint32 OutProcID = 0;
	FProcHandle Handle = FPlatformProcess::CreateProc(
		*PythonExe,
		*CmdLine,
		/*bLaunchDetached=*/ true,
		/*bLaunchHidden=*/ true,
		/*bLaunchReallyHidden=*/ true,
		&OutProcID,
		/*PriorityModifier=*/ 0,
		*WorkingDir,
		nullptr,
		nullptr);

	if (Handle.IsValid())
	{
		ProxyPid = OutProcID;
		FPlatformProcess::CloseProc(Handle);
		UE_LOG(LogClaireon, Display,
			TEXT("[MCP Proxy] Spawned claireon_proxy.py (pid=%u) via CreateProc"), ProxyPid);
		return true;
	}

	// Fallback path documented in ALWAYS_ON_MCP_PROXY_CPP.md: on Windows,
	// `cmd.exe /c start /b "" python.exe <script> <args>` reliably orphans
	// the child if DETACHED_PROCESS alone fails to detach it from the
	// editor's job object. Only attempted if the primary spawn failed.
#if PLATFORM_WINDOWS
	UE_LOG(LogClaireon, Warning,
		TEXT("[MCP Proxy] CreateProc failed; attempting cmd.exe /c start /b fallback"));

	const FString ComSpec = FPlatformMisc::GetEnvironmentVariable(TEXT("ComSpec"));
	const FString CmdExe = ComSpec.IsEmpty() ? TEXT("cmd.exe") : ComSpec;
	const FString FallbackArgs = FString::Printf(
		TEXT("/c start /b \"\" \"%s\" \"%s\""),
		*PythonExe, *ScriptPath);

	uint32 FallbackPid = 0;
	FProcHandle FallbackHandle = FPlatformProcess::CreateProc(
		*CmdExe,
		*FallbackArgs,
		/*bLaunchDetached=*/ true,
		/*bLaunchHidden=*/ true,
		/*bLaunchReallyHidden=*/ true,
		&FallbackPid,
		0,
		*WorkingDir, nullptr, nullptr);

	if (FallbackHandle.IsValid())
	{
		// cmd.exe's PID is NOT the python.exe PID. We treat ProxyPid as
		// unknown in this case; version_mismatch kill-path will have to
		// fall back to reading proxy.lock for the real PID.
		ProxyPid = 0;
		FPlatformProcess::CloseProc(FallbackHandle);
		UE_LOG(LogClaireon, Display,
			TEXT("[MCP Proxy] Spawned claireon_proxy.py via cmd.exe fallback"));
		return true;
	}
#endif

	UE_LOG(LogClaireon, Error,
		TEXT("[MCP Proxy] Failed to spawn claireon_proxy.py via any method"));
	return false;
}

bool FClaireonProxyClient::WaitForProxyReachable()
{
	// Probe GET /health on the registration port. This endpoint is
	// side-effect-free, so we can hammer it without polluting proxy.log
	// with "register rejected reason=malformed_request" warnings on every
	// editor boot. Any HTTP response >= 200 counts as "listener up".
	const FString Url = MakeRegUrl(ClaireonProxy::HealthEndpoint);
	float Delay = 0.1f;
	for (int32 Attempt = 0; Attempt < 10; ++Attempt)
	{
		int32 Status = 0;
		FString Body;
		if (SyncGet(Url, /*TimeoutSeconds=*/ 2, Status, Body))
		{
			return true;
		}
		if (Status >= 200)
		{
			return true;
		}
		FPlatformProcess::Sleep(Delay);
		Delay = FMath::Min(Delay * 2.0f, 1.6f);
	}
	return false;
}

bool FClaireonProxyClient::EnsureProxyRunning()
{
	// Cheap attach check first: if the listener is already up (previous
	// editor session left the proxy behind) we skip the spawn entirely.
	// GET /health is side-effect-free; see WaitForProxyReachable() above.
	const FString Url = MakeRegUrl(ClaireonProxy::HealthEndpoint);
	int32 Status = 0;
	FString Body;
	if (SyncGet(Url, /*TimeoutSeconds=*/ 1, Status, Body) || Status >= 200)
	{
		UE_LOG(LogClaireon, Display,
			TEXT("[MCP Proxy] Attached to existing proxy on port %d"),
			ClaireonProxy::PROXY_REG_PORT);
		return true;
	}

	if (!SpawnDetachedProxy())
	{
		return false;
	}

	if (!WaitForProxyReachable())
	{
		UE_LOG(LogClaireon, Error,
			TEXT("[MCP Proxy] Proxy did not become reachable on port %d within budget"),
			ClaireonProxy::PROXY_REG_PORT);
		return false;
	}

	return true;
}

bool FClaireonProxyClient::EnsureWorktreeBound(const FString& WorktreeRoot, int32 MCPPortHint)
{
	if (WorktreeRoot.IsEmpty())
	{
		return false;
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("worktree_root"), WorktreeRoot);
	if (MCPPortHint > 0 && MCPPortHint <= 65535)
	{
		Payload->SetNumberField(TEXT("mcp_port"), MCPPortHint);
	}

	FString Body;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(Payload, Writer);

	int32 Status = 0;
	FString ResponseBody;
	const bool bOk = SyncPostJson(
		MakeRegUrl(ClaireonProxy::EnsureWorktreeEndpoint),
		Body, /*TimeoutSeconds=*/ 3, Status, ResponseBody);

	if (!bOk)
	{
		const TSharedPtr<FJsonObject> Resp = ParseJson(ResponseBody);
		const FString Reason = JsonString(Resp, TEXT("reason"));
		UE_LOG(LogClaireon, Warning,
			TEXT("[MCP Proxy] /admin/ensure_worktree did not bind worktree=%s status=%d reason=%s"),
			*WorktreeRoot, Status, *Reason);
		return false;
	}

	UE_LOG(LogClaireon, Display,
		TEXT("[MCP Proxy] Asked proxy to bind worktree=%s mcp_port_hint=%d"),
		*WorktreeRoot, MCPPortHint);
	return true;
}

ERegisterResult FClaireonProxyClient::RegisterOnce()
{
	if (CachedEditorMCPToken.Len() < 32)
	{
		UE_LOG(LogClaireon, Error,
			TEXT("[MCP Proxy] Refusing to register: EditorMCPToken must be >= 32 chars"));
		return ERegisterResult::TerminalAuthOrMalformed;
	}

	const FString ProxyHash = ComputeProxyScriptHash();
	const FString WorktreeRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	// Stage 005 multi-tenant wire: identify the session by (pid,
	// start_time_ns); session_uuid was dropped. StartTimeNs was populated
	// in BeginRetryRegister and is the same across every retry, so the
	// proxy can compare identities deterministically. start_time_ns is
	// sent as a decimal string -- the value is a Windows FILETIME (~1.3e17
	// today) which exceeds JSON Number / IEEE-754 double precision, and
	// UE's TJsonWriter would emit it in scientific notation otherwise.
	// The proxy stores the string verbatim and uses string equality for
	// session-identity matching (no parse round-trip needed).
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
	Payload->SetStringField(TEXT("worktree_root"), WorktreeRoot);
	Payload->SetStringField(TEXT("start_time_ns"), FString::Printf(TEXT("%lld"), StartTimeNs));
	Payload->SetStringField(TEXT("build_id"), CachedBuildId);
	Payload->SetStringField(TEXT("proxy_version"), ProxyHash);
	Payload->SetNumberField(TEXT("editor_mcp_port"), CachedEditorMCPPort);
	Payload->SetStringField(TEXT("editor_mcp_token"), CachedEditorMCPToken);

	FString Body;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(Payload, Writer);

	int32 Status = 0;
	FString ResponseBody;
	const bool bOk = SyncPostJson(
		MakeRegUrl(ClaireonProxy::RegisterEndpoint),
		Body, /*TimeoutSeconds=*/ 5, Status, ResponseBody);

	TSharedPtr<FJsonObject> ResponseJson = ParseJson(ResponseBody);

	// Transport failures (no response, 5xx) are transient. 4xx responses
	// carry a structured accepted/reason body and are handled below.
	if (!bOk && (Status == 0 || Status >= 500))
	{
		return ERegisterResult::Transient;
	}

	const bool bAccepted = JsonBool(ResponseJson, TEXT("accepted"));
	const FString Reason = JsonString(ResponseJson, TEXT("reason"));

	if (bAccepted)
	{
		bIsRegistered = true;
		ProxyState = EClaireonProxyState::Registered;
		StartHeartbeatTicker();
		CurrentBackoffSeconds = 0.25;
		UE_LOG(LogClaireon, Display,
			TEXT("[MCP Proxy] Registered editor (pid=%u start_time_ns=%lld build=%s)"),
			FPlatformProcess::GetCurrentProcessId(), StartTimeNs, *CachedBuildId);
		return ERegisterResult::Accepted;
	}

	UE_LOG(LogClaireon, Warning,
		TEXT("[MCP Proxy] /editor/register rejected: reason=%s status=%d"),
		*Reason, Status);

	// Stage 005: version_mismatch is gone (advisory on the proxy side, D5),
	// active_session is gone (newest-wins, D6), worktree_mismatch is gone
	// (multi-tenant). Only auth/malformed remain as 4xx terminal cases.
	if (Reason == TEXT("malformed_request") || Reason == TEXT("bad_request")
		|| Reason == TEXT("unauthorized") || Reason == TEXT("auth_required"))
	{
		bIsRegistered = false;
		return ERegisterResult::TerminalAuthOrMalformed;
	}

	// Anything else (unexpected reason string, etc.) is transient.
	bIsRegistered = false;
	return ERegisterResult::Transient;
}

bool FClaireonProxyClient::Register(int32 EditorMCPPort, const FString& EditorMCPToken, const FString& BuildId)
{
	CachedEditorMCPPort = EditorMCPPort;
	CachedEditorMCPToken = EditorMCPToken;
	CachedBuildId = BuildId;
	// Stage 005: ensure StartTimeNs is populated before RegisterOnce uses it.
	// BeginRetryRegister latches it once for the production path; this
	// synchronous wrapper (used by the smoke test) needs the same guarantee.
	if (StartTimeNs == 0)
	{
		StartTimeNs = GetSelfStartTimeNs();
	}
	const ERegisterResult Result = RegisterOnce();
	return Result == ERegisterResult::Accepted;
}

void FClaireonProxyClient::BeginRetryRegister(int32 EditorMCPPort, const FString& EditorMCPToken, const FString& BuildId)
{
	CachedEditorMCPPort = EditorMCPPort;
	CachedEditorMCPToken = EditorMCPToken;
	CachedBuildId = BuildId;

	// Stage 005: latch this editor's start_time_ns once. Every retry uses
	// the same tuple, so the proxy can compare identities deterministically
	// across the RetryRegister loop. Reads 0 if the OS query fails -- still
	// valid, the proxy treats pid alone as unique on a live host.
	if (StartTimeNs == 0)
	{
		StartTimeNs = GetSelfStartTimeNs();
	}

	// Move into the RetryRegister state; the heartbeat ticker handles the
	// actual /editor/register attempt. Initialise NextRegisterAttemptSeconds
	// to "now" so the first tick fires immediately.
	ProxyState = EClaireonProxyState::RetryRegister;
	NextRegisterAttemptSeconds = FPlatformTime::Seconds();
	LastWarnedAtSeconds = 0.0;
	CurrentBackoffSeconds = 0.25;
	StartHeartbeatTicker();
}

void FClaireonProxyClient::RequestReconnect()
{
	if (ProxyState == EClaireonProxyState::Unstarted)
	{
		// Nothing to reconnect: BeginRetryRegister never ran.
		return;
	}
	UE_LOG(LogClaireon, Display,
		TEXT("[MCP Proxy] Manual reconnect requested (state was %d)"),
		static_cast<int32>(ProxyState));
	ProxyState = EClaireonProxyState::RetryRegister;
	NextRegisterAttemptSeconds = FPlatformTime::Seconds();
	LastWarnedAtSeconds = 0.0;
	CurrentBackoffSeconds = 0.25;
	bIsRegistered = false;
	StartHeartbeatTicker();
}

void FClaireonProxyClient::ScheduleRetry(double NowSeconds)
{
	NextRegisterAttemptSeconds = NowSeconds + CurrentBackoffSeconds;
	CurrentBackoffSeconds = FMath::Min(CurrentBackoffSeconds * 2.0, 5.0);

	// Log once per minute so a sustained outage stays visible without
	// flooding the log when the ticker fires on its 5s cadence.
	constexpr double WarnIntervalSeconds = 60.0;
	if (NowSeconds - LastWarnedAtSeconds >= WarnIntervalSeconds)
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[MCP Proxy] RetryRegister in progress: proxy unreachable or rejecting transiently. ")
			TEXT("Next attempt in %.2fs."),
			CurrentBackoffSeconds);
		LastWarnedAtSeconds = NowSeconds;
	}
}

void FClaireonProxyClient::Unregister()
{
	StopHeartbeatTicker();

	if (!bIsRegistered)
	{
		bIsRegistered = false;
		ProxyState = EClaireonProxyState::Unstarted;
		return;
	}

	// Stage 005 multi-tenant wire: identify the session by (worktree_root,
	// pid, start_time_ns). The proxy clears the slot only if the tuple
	// matches the current session, so a racing newest-wins eviction is
	// safe. start_time_ns is sent as a decimal string for the same reason
	// as register (see the register payload comment).
	const FString WorktreeRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("worktree_root"), WorktreeRoot);
	Payload->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
	Payload->SetStringField(TEXT("start_time_ns"), FString::Printf(TEXT("%lld"), StartTimeNs));

	FString Body;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(Payload, Writer);

	int32 Status = 0;
	FString ResponseBody;
	SyncPostJson(MakeRegUrl(ClaireonProxy::UnregisterEndpoint),
		Body, /*TimeoutSeconds=*/ 2, Status, ResponseBody);

	UE_LOG(LogClaireon, Display,
		TEXT("[MCP Proxy] Unregistered editor (pid=%u start_time_ns=%lld status=%d)"),
		FPlatformProcess::GetCurrentProcessId(), StartTimeNs, Status);

	bIsRegistered = false;
	ProxyState = EClaireonProxyState::Unstarted;
}

void FClaireonProxyClient::StartHeartbeatTicker()
{
	StopHeartbeatTicker();
	HeartbeatTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FClaireonProxyClient::HeartbeatTick),
		static_cast<float>(ClaireonProxy::HEARTBEAT_INTERVAL_SECONDS));
}

void FClaireonProxyClient::StopHeartbeatTicker()
{
	if (HeartbeatTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(HeartbeatTickerHandle);
		HeartbeatTickerHandle.Reset();
	}
}

FHeartbeatResult FClaireonProxyClient::SendHeartbeat()
{
	FHeartbeatResult Out;

	// Stage 005 multi-tenant wire: heartbeat body is (worktree_root, pid,
	// start_time_ns). The proxy uses (pid, start_time_ns) as the session
	// identity; an absent local StartTimeNs is OK -- the proxy treats "0"
	// as a valid value. start_time_ns is sent as a decimal string for the
	// same reason as register (see the register payload comment).
	const FString WorktreeRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("worktree_root"), WorktreeRoot);
	Payload->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
	Payload->SetStringField(TEXT("start_time_ns"), FString::Printf(TEXT("%lld"), StartTimeNs));

	FString Body;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(Payload, Writer);

	int32 Status = 0;
	FString ResponseBody;
	const bool bOk = SyncPostJson(
		MakeRegUrl(ClaireonProxy::HeartbeatEndpoint),
		Body, /*TimeoutSeconds=*/ 2, Status, ResponseBody);

	if (!bOk)
	{
		Out.Reason = EHeartbeatReason::TransportFailure;
		return Out;
	}

	const TSharedPtr<FJsonObject> ResponseJson = ParseJson(ResponseBody);
	if (JsonBool(ResponseJson, TEXT("ok")))
	{
		Out.Ok = true;
		return Out;
	}

	const FString Reason = JsonString(ResponseJson, TEXT("reason"));

	// D6 newest-wins: an evicted_by sub-object on the heartbeat reply means
	// another editor took our worktree slot. Read it defensively -- today's
	// per-worktree proxy never sets it, so an absent field is treated as
	// staleness (the correct fallback for the legacy proxy).
	const TSharedPtr<FJsonObject>* EvictedByObj = nullptr;
	if (ResponseJson.IsValid()
		&& ResponseJson->TryGetObjectField(TEXT("evicted_by"), EvictedByObj)
		&& EvictedByObj && (*EvictedByObj).IsValid())
	{
		Out.Reason = EHeartbeatReason::UnknownSessionEvicted;
		double PidNum = 0.0;
		if ((*EvictedByObj)->TryGetNumberField(TEXT("pid"), PidNum))
		{
			Out.EvictedByPid = static_cast<uint32>(PidNum);
		}
		FString StartTimeStr;
		if ((*EvictedByObj)->TryGetStringField(TEXT("start_time_ns"), StartTimeStr))
		{
			Out.EvictedByStartTimeNs = FCString::Atoi64(*StartTimeStr);
		}
		else
		{
			double StartTimeNum = 0.0;
			if ((*EvictedByObj)->TryGetNumberField(TEXT("start_time_ns"), StartTimeNum))
			{
				Out.EvictedByStartTimeNs = static_cast<int64>(StartTimeNum);
			}
		}
		return Out;
	}

	// Unknown_session without evicted_by, or any other rejection -> staleness.
	Out.Reason = EHeartbeatReason::UnknownSessionStale;
	(void)Reason; // Reason is captured in logs by the caller via state.
	return Out;
}

void FClaireonProxyClient::SetTransportOverrides_TestOnly(
	TFunction<ERegisterResult()> InRegisterOverride,
	TFunction<FHeartbeatResult()> InHeartbeatOverride)
{
	RegisterTransportOverride = MoveTemp(InRegisterOverride);
	HeartbeatTransportOverride = MoveTemp(InHeartbeatOverride);
}

void FClaireonProxyClient::TickForTest(double NowSeconds)
{
	switch (ProxyState)
	{
	case EClaireonProxyState::Unstarted:
	case EClaireonProxyState::Failed:
		return;

	case EClaireonProxyState::RetryRegister:
	{
		if (NowSeconds < NextRegisterAttemptSeconds)
		{
			return;
		}
		const ERegisterResult Result = RegisterTransportOverride
			? RegisterTransportOverride()
			: RegisterOnce();
		switch (Result)
		{
		case ERegisterResult::Accepted:
			ProxyState = EClaireonProxyState::Registered;
			bIsRegistered = true;
			CurrentBackoffSeconds = 0.25;
			LastWarnedAtSeconds = 0.0;
			break;
		case ERegisterResult::TerminalAuthOrMalformed:
			ProxyState = EClaireonProxyState::Failed;
			UE_LOG(LogClaireon, Error,
				TEXT("[MCP Proxy] /editor/register rejected terminally; ")
				TEXT("manual reconnect required (use diagnostics tab)."));
			break;
		case ERegisterResult::Transient:
		default:
			ScheduleRetry(NowSeconds);
			break;
		}
		return;
	}

	case EClaireonProxyState::Registered:
	{
		const FHeartbeatResult Result = HeartbeatTransportOverride
			? HeartbeatTransportOverride()
			: SendHeartbeat();
		if (Result.Ok)
		{
			return;
		}
		if (Result.Reason == EHeartbeatReason::UnknownSessionEvicted)
		{
			ProxyState = EClaireonProxyState::Failed;
			bIsRegistered = false;
			UE_LOG(LogClaireon, Warning,
				TEXT("[MCP Proxy] Session evicted by newer editor (pid=%u, start_time_ns=%lld). ")
				TEXT("Use diagnostics-tab Reconnect to retry."),
				Result.EvictedByPid, Result.EvictedByStartTimeNs);
			return;
		}

		UE_LOG(LogClaireon, Warning,
			TEXT("[MCP Proxy] Heartbeat failed (reason=%d); bouncing to RetryRegister"),
			static_cast<int32>(Result.Reason));
		ProxyState = EClaireonProxyState::RetryRegister;
		bIsRegistered = false;
		// Reset backoff to the floor so the very first retry attempt fires
		// promptly after staleness. Sustained churn re-doubles via
		// ScheduleRetry below.
		CurrentBackoffSeconds = 0.25;
		ScheduleRetry(NowSeconds);
		return;
	}
	}
}

bool FClaireonProxyClient::HeartbeatTick(float /*DeltaTime*/)
{
	const double NowSeconds = FPlatformTime::Seconds();

	// In RetryRegister, the production path also probes EnsureProxyRunning
	// before attempting RegisterOnce. The test-only TickForTest skips that
	// because tests inject the register outcome directly.
	if (ProxyState == EClaireonProxyState::RetryRegister)
	{
		if (NowSeconds < NextRegisterAttemptSeconds)
		{
			return true;
		}
		if (!RegisterTransportOverride && !EnsureProxyRunning())
		{
			ScheduleRetry(NowSeconds);
			return true;
		}
	}

	TickForTest(NowSeconds);
	return true;
}
