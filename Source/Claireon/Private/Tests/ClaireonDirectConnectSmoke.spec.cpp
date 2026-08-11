// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Direct-connect smoke spec for FClaireonServer.
//
// Verifies that an FClaireonServer started with SessionToken="" (the
// direct-connect / no-proxy path) serves /mcp correctly:
//   1. initialize JSON-RPC POST returns HTTP 200 with a non-empty
//      result.serverInfo.name.
//   2. tools/list JSON-RPC POST returns the MCP-visible pair, using
//      test-registered tool_search / python_execute stubs (the only two names
//      FClaireonServer::MCPVisibleTools advertises).
//   3. The same port can be re-bound after Stop() (see the note in
//      SmokeStopThenRestartSamePort about what that does and does not prove).
//
// The fixture is intentionally lightweight: it does NOT depend on the
// editor module's full tool catalogue. Two stub IClaireonTool subclasses
// stand in for tool_search / python_execute so the assertions
// have a deterministic surface regardless of what the live module
// registered. This spec doubles as a regression guard: every later
// stage in the multi-worktree-proxy workflow modifies FClaireonServer
// or FClaireonProxyClient; if any stage breaks the no-proxy path, this
// spec is the first thing that fires.
//
// Category: Claireon.DirectConnect.Smoke.* (run with the automation test
// filter "Claireon.DirectConnect.Smoke.").
//
// TWO FLAKE ROOT CAUSES WERE FIXED HERE (2026-07-29). Read these before
// "simplifying" the helpers back:
//   1. Server readiness is not a sleep -- it is an engine tick. UE's HTTP
//      server accepts sockets and dispatches routes only from
//      FHttpServerModule::Tick (a core-ticker object). The old helper
//      busy-waited in the test body and ticked only the CLIENT FHttpManager,
//      so the listener never ran and every /mcp POST died on the client
//      timeout. The specs now co_await Squid::WaitUntil, which hands control
//      back to the Untest runner so the commandlet's engine tick advances the
//      server between polls.
//   2. Ports must be acquired with StartEphemeral, not Start(random). Windows
//      hosts running Hyper-V / WSL / Docker reserve contiguous 100-port blocks
//      inside the ephemeral range. Start() used to retry only the next 10
//      CONSECUTIVE ports and therefore could not escape such a block, producing
//      "Failed to bind any port in range NNNNN-NNNNN" for whichever spec drew
//      a poisoned candidate that run. Start() now strides like StartEphemeral,
//      but StartEphemeral remains the right entrypoint here: it is the path the
//      editor actually takes.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonServer.h"
#include "Tools/IClaireonTool.h"

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SquidTasks/Task.h"

namespace ClaireonDirectConnectSmoke_spec_Private1
{
	// File-local prefix on every helper to avoid collisions under unity
	// batching (anon-namespace helpers can otherwise alias on Linux clang).

	/** Stub tool used to seed the server's tools/list with a known name. */
	class FDirectConnectSmoke_StubTool : public IClaireonTool
	{
	public:
		FDirectConnectSmoke_StubTool(FString InName, FString InDescription)
			: ToolDescription(MoveTemp(InDescription))
		{
			int32 UnderscorePos = INDEX_NONE;
			if (InName.FindChar(TEXT('_'), UnderscorePos))
			{
				CategoryStr = InName.Left(UnderscorePos);
				OperationStr = InName.RightChop(UnderscorePos + 1);
			}
			else
			{
				CategoryStr = InName;
				OperationStr = TEXT("");
			}
		}

		virtual FString GetCategory() const override { return CategoryStr; }
		virtual FString GetOperation() const override { return OperationStr; }
		virtual FString GetDescription() const override { return ToolDescription; }

		virtual TSharedPtr<FJsonObject> GetInputSchema() const override
		{
			TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("object"));
			Schema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
			return Schema;
		}

		virtual FToolResult Execute(const TSharedPtr<FJsonObject>& /*Arguments*/) override
		{
			return MakeSuccessResult(nullptr, TEXT("ok"));
		}

	private:
		FString CategoryStr;
		FString OperationStr;
		FString ToolDescription;
	};

	/** Seconds a single /mcp round trip is allowed to take. */
	constexpr double DirectConnectSmoke_RequestTimeoutSeconds = 10.0;

	/**
	 * Bring a server up on some bindable ephemeral port and report it.
	 *
	 * ROOT CAUSE this guards (do not simplify back to a single Start(random)):
	 * FClaireonServer::Start() is the legacy entrypoint. It USED to retry only
	 * the next 10 CONSECUTIVE ports on a bind failure. Windows boxes running
	 * Hyper-V / WSL / Docker reserve whole contiguous blocks of the ephemeral
	 * range (see `netsh interface ipv4 show excludedportrange protocol=tcp`;
	 * on this hardware the blocks are 100 ports wide, e.g. 50160-50259 and
	 * 50263-50362). A randomly chosen candidate that lands inside such a block
	 * can never escape it with a +1 walk, so Start() returned false and the
	 * spec failed with "Failed to bind any port in range 50173-50182". Which
	 * member of the spec drew a poisoned port varied run to run, which is why
	 * the failures looked random. Start() now strides by 1009 like
	 * StartEphemeral, so that specific failure mode is fixed at the source --
	 * but this helper still belongs on StartEphemeral, see below.
	 *
	 * FClaireonServer::StartEphemeral() is the production entrypoint (it is
	 * what FClaireonModule falls back to) and it walks the ephemeral range
	 * with a 1009-port stride for 32 attempts, so it steps straight over a
	 * reserved block. Using it here both removes the flake and makes the spec
	 * exercise the path the editor actually takes.
	 */
	uint32 DirectConnectSmoke_StartOnBindablePort(FClaireonServer& Server)
	{
		return static_cast<uint32>(Server.StartEphemeral());
	}

	/**
	 * Kick off a JSON-RPC POST against http://127.0.0.1:Port/mcp and hand back
	 * the in-flight request. Returns an invalid ptr if ProcessRequest failed.
	 *
	 * ROOT CAUSE this split guards (do not fold it back into a blocking
	 * helper): UE's HTTP *server* accepts connections and dispatches route
	 * handlers only from FHttpServerModule::Tick, which runs off the core
	 * ticker as part of the engine tick. The previous version of this helper
	 * busy-waited inside the test body and ticked only FHttpManager (the
	 * CLIENT side), so the engine never ticked while the request was in
	 * flight, the listener never accepted the socket, and the request always
	 * died on the client timeout ("HTTP request timed out after 5.00 seconds
	 * URL=http://127.0.0.1:<port>/mcp", immediately followed after the test by
	 * the server finally waking up and failing to write to the abandoned
	 * socket: "WriteBytes sent -1/92 bytes"). The caller must instead
	 * co_await Squid::WaitUntil(...) so the Untest runner returns control to
	 * the commandlet's engine tick between polls.
	 */
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> DirectConnectSmoke_BeginPostJsonRpc(
		uint32 Port,
		const FString& Body)
	{
		const FString Url = FString::Printf(TEXT("http://127.0.0.1:%u/mcp"), Port);

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request =
			FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(TEXT("POST"));
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
		Request->SetTimeout(static_cast<float>(DirectConnectSmoke_RequestTimeoutSeconds));
		Request->SetContentAsString(Body);

		if (!Request->ProcessRequest())
		{
			return nullptr;
		}
		return Request;
	}

	/** True once the request has left the Processing/NotStarted states. */
	bool DirectConnectSmoke_IsRequestSettled(
		const TSharedPtr<IHttpRequest, ESPMode::ThreadSafe>& Request)
	{
		if (!Request.IsValid())
		{
			return true;
		}
		const EHttpRequestStatus::Type Status = Request->GetStatus();
		return Status != EHttpRequestStatus::Processing
			&& Status != EHttpRequestStatus::NotStarted;
	}

	/**
	 * Harvest a settled request. Returns true with OutStatus / OutBody
	 * populated when a response arrived. Kept out of any UNTEST_* lambda --
	 * UNTEST macros expand to co_return and cannot be used inside
	 * non-coroutine callables.
	 */
	bool DirectConnectSmoke_FinishPostJsonRpc(
		const TSharedPtr<IHttpRequest, ESPMode::ThreadSafe>& Request,
		int32& OutStatus,
		FString& OutBody)
	{
		OutStatus = 0;
		OutBody.Reset();
		if (!Request.IsValid())
		{
			return false;
		}
		FHttpResponsePtr Response = Request->GetResponse();
		if (!Response.IsValid())
		{
			return false;
		}
		OutStatus = Response->GetResponseCode();
		OutBody = Response->GetContentAsString();
		return true;
	}

	/**
	 * Build an MCP initialize JSON-RPC request body. Schema mirrors the
	 * spec: jsonrpc=2.0, id=1, method=initialize, params with
	 * protocolVersion/clientInfo.
	 */
	FString DirectConnectSmoke_BuildInitializeBody()
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Root->SetNumberField(TEXT("id"), 1);
		Root->SetStringField(TEXT("method"), TEXT("initialize"));

		TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("protocolVersion"), TEXT("2024-11-05"));
		TSharedRef<FJsonObject> ClientInfo = MakeShared<FJsonObject>();
		ClientInfo->SetStringField(TEXT("name"), TEXT("ClaireonDirectConnectSmoke"));
		ClientInfo->SetStringField(TEXT("version"), TEXT("0.0.0"));
		Params->SetObjectField(TEXT("clientInfo"), ClientInfo);
		Params->SetObjectField(TEXT("capabilities"), MakeShared<FJsonObject>());
		Root->SetObjectField(TEXT("params"), Params);

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		return Out;
	}

	FString DirectConnectSmoke_BuildToolsListBody()
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Root->SetNumberField(TEXT("id"), 2);
		Root->SetStringField(TEXT("method"), TEXT("tools/list"));
		Root->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		return Out;
	}

	/** Parse a JSON object body. Returns nullptr on failure. */
	TSharedPtr<FJsonObject> DirectConnectSmoke_ParseJson(const FString& Body)
	{
		TSharedPtr<FJsonObject> Out;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
		if (!FJsonSerializer::Deserialize(Reader, Out) || !Out.IsValid())
		{
			return nullptr;
		}
		return Out;
	}
} // namespace
using namespace ClaireonDirectConnectSmoke_spec_Private1;

// ---------------------------------------------------------------------------
// Initialize handshake: HTTP 200 + non-empty serverInfo.name.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, DirectConnect, SmokeInitialize, UNTEST_TIMEOUTMS(30000))
{
	auto Server = MakeShared<FClaireonServer>();
	// Empty token = direct-connect / no-proxy path. The server logs a
	// Warning on Start and accepts unauthenticated /mcp traffic.
	Server->SetSessionToken(FString());

	const uint32 StartedPort = DirectConnectSmoke_StartOnBindablePort(*Server);
	UNTEST_ASSERT_TRUE(StartedPort > 0);
	UNTEST_ASSERT_TRUE(Server->IsRunning());

	const uint32 BoundPort = Server->GetPort();
	UNTEST_ASSERT_EQ(BoundPort, StartedPort);
	UNTEST_ASSERT_TRUE(BoundPort > 0);

	const FString Body = DirectConnectSmoke_BuildInitializeBody();
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request =
		DirectConnectSmoke_BeginPostJsonRpc(BoundPort, Body);
	UNTEST_ASSERT_VALID(Request);

	// Yield to the engine tick between polls. The HTTP server only services
	// sockets from FHttpServerModule::Tick, so a busy-wait here starves it and
	// the request can never be answered (see the helper's comment).
	const double Deadline =
		FPlatformTime::Seconds() + DirectConnectSmoke_RequestTimeoutSeconds;
	co_await Squid::WaitUntil([Request, Deadline]()
	{
		return DirectConnectSmoke_IsRequestSettled(Request)
			|| FPlatformTime::Seconds() >= Deadline;
	});

	int32 Status = 0;
	FString ResponseBody;
	const bool bOk = DirectConnectSmoke_FinishPostJsonRpc(Request, Status, ResponseBody);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_ASSERT_EQ(Status, 200);

	const TSharedPtr<FJsonObject> Json = DirectConnectSmoke_ParseJson(ResponseBody);
	UNTEST_ASSERT_VALID(Json);

	const TSharedPtr<FJsonObject>* ResultObj = nullptr;
	UNTEST_ASSERT_TRUE(Json->TryGetObjectField(TEXT("result"), ResultObj));
	UNTEST_ASSERT_PTR(ResultObj);

	const TSharedPtr<FJsonObject>* ServerInfoObj = nullptr;
	UNTEST_ASSERT_TRUE((*ResultObj)->TryGetObjectField(TEXT("serverInfo"), ServerInfoObj));
	UNTEST_ASSERT_PTR(ServerInfoObj);

	FString Name;
	UNTEST_ASSERT_TRUE((*ServerInfoObj)->TryGetStringField(TEXT("name"), Name));
	UNTEST_ASSERT_FALSE(Name.IsEmpty());

	Server->Stop();
	UNTEST_ASSERT_FALSE(Server->IsRunning());
	co_return;
}

// ---------------------------------------------------------------------------
// tools/list returns the Code Mode pair (claireon_search + claireon_execute).
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, DirectConnect, SmokeToolsList, UNTEST_TIMEOUTMS(30000))
{
	auto Server = MakeShared<FClaireonServer>();
	Server->SetSessionToken(FString());

	// Register the Code Mode pair stubs so tools/list has a deterministic
	// surface independent of any module-level tool collection.
	//
	// The names MUST be exactly tool_search / python_execute. FClaireonServer's
	// MCPVisibleTools allow-list advertises only those two names in tools/list;
	// anything else is reachable only through python_execute. This spec used to
	// register claireon_search / claireon_execute and assert they appeared,
	// which could never have held -- the stale names went unnoticed because the
	// request never completed (see readiness root cause at the top of the file),
	// so the loop below always ran over an empty array on a failed assert.
	Server->RegisterTool(MakeShared<FDirectConnectSmoke_StubTool>(
		TEXT("tool_search"),
		TEXT("Search Claireon's editor tool catalogue (smoke stub).")));
	Server->RegisterTool(MakeShared<FDirectConnectSmoke_StubTool>(
		TEXT("python_execute"),
		TEXT("Execute Python inside the editor (smoke stub).")));

	const uint32 StartedPort = DirectConnectSmoke_StartOnBindablePort(*Server);
	UNTEST_ASSERT_TRUE(StartedPort > 0);

	const uint32 BoundPort = Server->GetPort();

	const FString Body = DirectConnectSmoke_BuildToolsListBody();
	TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Request =
		DirectConnectSmoke_BeginPostJsonRpc(BoundPort, Body);
	UNTEST_ASSERT_VALID(Request);

	// Same readiness contract as SmokeInitialize: the engine must tick while
	// the request is in flight or the HTTP server never accepts the socket.
	const double Deadline =
		FPlatformTime::Seconds() + DirectConnectSmoke_RequestTimeoutSeconds;
	co_await Squid::WaitUntil([Request, Deadline]()
	{
		return DirectConnectSmoke_IsRequestSettled(Request)
			|| FPlatformTime::Seconds() >= Deadline;
	});

	int32 Status = 0;
	FString ResponseBody;
	const bool bOk = DirectConnectSmoke_FinishPostJsonRpc(Request, Status, ResponseBody);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_ASSERT_EQ(Status, 200);

	const TSharedPtr<FJsonObject> Json = DirectConnectSmoke_ParseJson(ResponseBody);
	UNTEST_ASSERT_VALID(Json);

	const TSharedPtr<FJsonObject>* ResultObj = nullptr;
	UNTEST_ASSERT_TRUE(Json->TryGetObjectField(TEXT("result"), ResultObj));
	UNTEST_ASSERT_PTR(ResultObj);

	const TArray<TSharedPtr<FJsonValue>>* ToolsArr = nullptr;
	UNTEST_ASSERT_TRUE((*ResultObj)->TryGetArrayField(TEXT("tools"), ToolsArr));
	UNTEST_ASSERT_PTR(ToolsArr);

	bool bSawSearch = false;
	bool bSawExecute = false;
	for (const TSharedPtr<FJsonValue>& Entry : *ToolsArr)
	{
		const TSharedPtr<FJsonObject>* ToolObj = nullptr;
		if (!Entry.IsValid() || !Entry->TryGetObject(ToolObj) || !ToolObj)
		{
			continue;
		}
		FString ToolName;
		if (!(*ToolObj)->TryGetStringField(TEXT("name"), ToolName))
		{
			continue;
		}
		if (ToolName == TEXT("tool_search"))
		{
			bSawSearch = true;
		}
		else if (ToolName == TEXT("python_execute"))
		{
			bSawExecute = true;
		}
	}
	UNTEST_ASSERT_TRUE(bSawSearch);
	UNTEST_ASSERT_TRUE(bSawExecute);

	Server->Stop();
	co_return;
}

// ---------------------------------------------------------------------------
// Stop() lets a FRESH FClaireonServer instance reuse the same port number,
// in-process, without the auto-increment retry kicking in. This guards
// against route-handle leaks that would silently steal ports from later
// test runs or from a later Start() in the same process.
//
// NOT a port-release test. Investigated for the C2 hardening item: Stop()
// unbinds routes only. FHttpServerModule owns one process-wide listener per
// port and keeps it alive (still bound, still listening at the OS level) for
// the life of the process; there is no per-port stop/destroy API reachable
// from plugin code, only a process-wide StopAllListeners() that would also
// take down every other consumer of the shared module in this editor process
// (FSRemoteStatusSubsystem, FriendshipperHttpRouter, FSAssetImporter,
// PragmaActiveDebugServer all bind their own ports on the same singleton --
// see the note on FClaireonServer::Stop()'s implementation). So what
// Second->Start(BoundFirst) below actually exercises is "the still-listening
// FHttpListener for this port is found again via GetHttpRouter() and handed
// fresh routes" -- never a release-and-reacquire of the OS socket, which
// never happens until the process exits.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, DirectConnect, SmokeStopThenRestartSamePort, UNTEST_TIMEOUTMS(10000))
{
	auto First = MakeShared<FClaireonServer>();
	First->SetSessionToken(FString());
	// StartEphemeral, not Start(random): StartEphemeral is the production
	// entrypoint, and it sweeps the ephemeral range instead of anchoring on a
	// single caller-chosen candidate.
	UNTEST_ASSERT_TRUE(DirectConnectSmoke_StartOnBindablePort(*First) > 0);
	const uint32 BoundFirst = First->GetPort();
	First->Stop();
	UNTEST_ASSERT_FALSE(First->IsRunning());

	auto Second = MakeShared<FClaireonServer>();
	Second->SetSessionToken(FString());
	// Deliberately Start(BoundFirst), not StartEphemeral: the point is that the
	// exact same port comes back. Start()'s first attempt is always exactly the
	// requested port, and BoundFirst is already proven bindable, so there is no
	// reserved-block flake here; if the rebind slipped to another port (which
	// only happens if attempt 0 failed) the GetPort() assertion below catches it.
	UNTEST_ASSERT_TRUE(Second->Start(BoundFirst));
	UNTEST_ASSERT_EQ(Second->GetPort(), BoundFirst);
	Second->Stop();
	co_return;
}

// ---------------------------------------------------------------------------
// WritePortFile schema: JSON contains publicPort, mode, and preserves port/pid.
// ---------------------------------------------------------------------------

namespace ClaireonDirectConnectSmoke_spec_Private2
{
	/**
	 * Read and parse the Claireon port file. Returns nullptr if the file does
	 * not exist or is not valid JSON. Named with file-local prefix to avoid
	 * unity-build anon-NS collisions.
	 */
	TSharedPtr<FJsonObject> DirectConnectSmoke_ReadPortFileJson()
	{
		const FString PortFilePath = FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("Claireon"), TEXT("MCPServer.json"));
		FString Contents;
		if (!FFileHelper::LoadFileToString(Contents, *PortFilePath))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Out;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
		if (!FJsonSerializer::Deserialize(Reader, Out) || !Out.IsValid())
		{
			return nullptr;
		}
		return Out;
	}

	/**
	 * Delete the Claireon port file if it exists (test cleanup helper).
	 * Named with file-local prefix to avoid unity-build anon-NS collisions.
	 */
	void DirectConnectSmoke_DeletePortFile()
	{
		const FString PortFilePath = FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("Claireon"), TEXT("MCPServer.json"));
		if (FPaths::FileExists(PortFilePath))
		{
			IFileManager::Get().Delete(*PortFilePath);
		}
	}
} // namespace
using namespace ClaireonDirectConnectSmoke_spec_Private2;

// ---------------------------------------------------------------------------
// WritePortFile direct mode: JSON has port/pid preserved, publicPort == port,
// mode == "direct".
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, DirectConnect, WritePortFileDirect, UNTEST_TIMEOUTMS(5000))
{
	// RESIDUAL RISK (not a defect in this spec): Saved/Claireon/MCPServer.json is
	// a per-project, cross-process singleton. If a real editor with a live
	// Claireon server is running against this same project directory while the
	// test commandlet executes, it can rewrite the file between WritePortFile and
	// the read below and the port/pid assertions will legitimately disagree.
	// Run the suite without a co-resident editor on the same project dir.
	DirectConnectSmoke_DeletePortFile();

	auto Server = MakeShared<FClaireonServer>();
	Server->SetSessionToken(FString());
	// StartEphemeral, not Start(random): a random candidate used to land inside a
	// Windows-reserved 100-port block that Start()'s old +1 retry could not
	// escape, which is what made this test fail intermittently.
	UNTEST_ASSERT_TRUE(DirectConnectSmoke_StartOnBindablePort(*Server) > 0);
	const uint16 BoundPort = static_cast<uint16>(Server->GetPort());

	Server->WritePortFile(BoundPort, /*bProxyAttached=*/false);

	const TSharedPtr<FJsonObject> Json = DirectConnectSmoke_ReadPortFileJson();
	UNTEST_ASSERT_VALID(Json);

	// "port" must equal the bound port.
	int32 ReadPort = 0;
	UNTEST_ASSERT_TRUE(Json->TryGetNumberField(TEXT("port"), ReadPort));
	UNTEST_ASSERT_EQ(ReadPort, static_cast<int32>(BoundPort));

	// "pid" must be present and non-zero.
	int32 ReadPid = 0;
	UNTEST_ASSERT_TRUE(Json->TryGetNumberField(TEXT("pid"), ReadPid));
	UNTEST_ASSERT_GT(ReadPid, 0);

	// "publicPort" must equal port in direct mode.
	int32 ReadPublicPort = 0;
	UNTEST_ASSERT_TRUE(Json->TryGetNumberField(TEXT("publicPort"), ReadPublicPort));
	UNTEST_ASSERT_EQ(ReadPublicPort, ReadPort);

	// "mode" must be "direct".
	FString ReadMode;
	UNTEST_ASSERT_TRUE(Json->TryGetStringField(TEXT("mode"), ReadMode));
	UNTEST_ASSERT_STREQ(ReadMode, TEXT("direct"));

	Server->Stop();
	DirectConnectSmoke_DeletePortFile();
	co_return;
}

// ---------------------------------------------------------------------------
// WritePortFile proxy mode: publicPort != port, mode == "proxy".
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, DirectConnect, WritePortFileProxy, UNTEST_TIMEOUTMS(5000))
{
	DirectConnectSmoke_DeletePortFile();

	// Bind-free: WritePortFile only serializes BoundPort (0 when unstarted)
	// and pid, so the proxy-mode schema can be verified without a live
	// listener. Starting a second server back-to-back with the direct-mode
	// test races the HTTP listener teardown and flakes on port binds.
	auto Server = MakeShared<FClaireonServer>();
	Server->SetSessionToken(FString());

	// Simulate proxy mode: effective (SHA) port differs from the (unbound,
	// zero) ephemeral port.
	const uint16 ShaPort = 18213;

	Server->WritePortFile(ShaPort, /*bProxyAttached=*/true);

	const TSharedPtr<FJsonObject> Json = DirectConnectSmoke_ReadPortFileJson();
	UNTEST_ASSERT_VALID(Json);

	// "port" reflects the unbound server's BoundPort (0).
	int32 ReadPort = -1;
	UNTEST_ASSERT_TRUE(Json->TryGetNumberField(TEXT("port"), ReadPort));
	UNTEST_ASSERT_EQ(ReadPort, 0);

	// "pid" must be present and non-zero.
	int32 ReadPid = 0;
	UNTEST_ASSERT_TRUE(Json->TryGetNumberField(TEXT("pid"), ReadPid));
	UNTEST_ASSERT_GT(ReadPid, 0);

	// "publicPort" must equal the SHA port, which differs from bound port.
	int32 ReadPublicPort = 0;
	UNTEST_ASSERT_TRUE(Json->TryGetNumberField(TEXT("publicPort"), ReadPublicPort));
	UNTEST_ASSERT_EQ(ReadPublicPort, static_cast<int32>(ShaPort));
	UNTEST_ASSERT_NE(ReadPublicPort, ReadPort);

	// "mode" must be "proxy".
	FString ReadMode;
	UNTEST_ASSERT_TRUE(Json->TryGetStringField(TEXT("mode"), ReadMode));
	UNTEST_ASSERT_STREQ(ReadMode, TEXT("proxy"));

	DirectConnectSmoke_DeletePortFile();
	co_return;
}

#endif // WITH_UNTESTED
