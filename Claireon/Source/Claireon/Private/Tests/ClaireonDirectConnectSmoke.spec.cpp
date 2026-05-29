// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Direct-connect smoke spec for FClaireonServer.
//
// Verifies that an FClaireonServer started with SessionToken="" (the
// direct-connect / no-proxy path) serves /mcp correctly:
//   1. initialize JSON-RPC POST returns HTTP 200 with a non-empty
//      result.serverInfo.name.
//   2. tools/list JSON-RPC POST returns the registered tools, including
//      the test-registered claireon_search and claireon_execute stubs that
//      mirror the Code Mode pair.
//   3. The bound port is released after Stop() (verified by re-binding
//      the same port).
//
// The fixture is intentionally lightweight: it does NOT depend on the
// editor module's full tool catalogue. Two stub IClaireonTool subclasses
// stand in for search / python_execute so the assertions
// have a deterministic surface regardless of what the live module
// registered. This spec doubles as a regression guard: every later
// stage in the multi-worktree-proxy workflow modifies FClaireonServer
// or FClaireonProxyClient; if any stage breaks the no-proxy path, this
// spec is the first thing that fires.
//
// Category: Claireon.DirectConnect.Smoke.* (run via
// `Scripts\Testing\Invoke-UntestTests.ps1 -TestFilter "Claireon.DirectConnect.Smoke."`).

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonServer.h"
#include "Tools/IClaireonTool.h"

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Math/RandomStream.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
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

	/**
	 * Pick a candidate ephemeral port. The platform's IHttpRouter does not
	 * expose a true bind(0) primitive, so we approximate it: choose a high
	 * port from the IANA dynamic range (49152-65000) seeded from the
	 * current cycle counter, then let FClaireonServer::Start auto-increment
	 * on collision (it retries up to 10 times). Collision probability is
	 * ~0 in practice because the test harness runs a single fixture at a
	 * time.
	 */
	uint32 DirectConnectSmoke_PickEphemeralPort()
	{
		FRandomStream Stream(static_cast<int32>(FPlatformTime::Cycles()));
		const uint32 Min = 49152u;
		const uint32 Max = 65000u;
		return Min + (Stream.GetUnsignedInt() % (Max - Min));
	}

	/**
	 * Single-shot synchronous HTTP POST against http://127.0.0.1:Port/mcp.
	 * Drives the FHttpManager tick loop until the request completes or the
	 * deadline elapses. Returns true with OutStatus / OutBody populated on
	 * success; false otherwise. This MUST live outside any UNTEST_*
	 * assertion lambda -- UNTEST macros expand to co_return and cannot be
	 * used inside non-coroutine callables.
	 */
	bool DirectConnectSmoke_PostJsonRpc(
		uint32 Port,
		const FString& Body,
		double TimeoutSeconds,
		int32& OutStatus,
		FString& OutBody)
	{
		OutStatus = 0;
		OutBody.Reset();

		const FString Url = FString::Printf(TEXT("http://127.0.0.1:%u/mcp"), Port);

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request =
			FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(TEXT("POST"));
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
		Request->SetTimeout(static_cast<float>(TimeoutSeconds));
		Request->SetContentAsString(Body);

		if (!Request->ProcessRequest())
		{
			return false;
		}

		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			const EHttpRequestStatus::Type Status = Request->GetStatus();
			if (Status != EHttpRequestStatus::Processing
				&& Status != EHttpRequestStatus::NotStarted)
			{
				break;
			}
			FHttpModule::Get().GetHttpManager().Tick(0.0f);
			FPlatformProcess::Sleep(0.005f);
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

// ---------------------------------------------------------------------------
// Initialize handshake: HTTP 200 + non-empty serverInfo.name.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, DirectConnect, SmokeInitialize, UNTEST_TIMEOUTMS(15000))
{
	auto Server = MakeShared<FClaireonServer>();
	// Empty token = direct-connect / no-proxy path. The server logs a
	// Warning on Start and accepts unauthenticated /mcp traffic.
	Server->SetSessionToken(FString());

	const uint32 Candidate = DirectConnectSmoke_PickEphemeralPort();
	const bool bStarted = Server->Start(Candidate);
	UNTEST_ASSERT_TRUE(bStarted);
	UNTEST_ASSERT_TRUE(Server->IsRunning());

	const uint32 BoundPort = Server->GetPort();
	UNTEST_ASSERT_TRUE(BoundPort > 0);

	const FString Body = DirectConnectSmoke_BuildInitializeBody();
	int32 Status = 0;
	FString ResponseBody;
	const bool bOk = DirectConnectSmoke_PostJsonRpc(
		BoundPort, Body, /*TimeoutSeconds=*/ 5.0, Status, ResponseBody);
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

UNTEST_UNIT_OPTS(Claireon, DirectConnect, SmokeToolsList, UNTEST_TIMEOUTMS(15000))
{
	auto Server = MakeShared<FClaireonServer>();
	Server->SetSessionToken(FString());

	// Register the Code Mode pair stubs so tools/list has a deterministic
	// surface independent of any module-level tool collection.
	Server->RegisterTool(MakeShared<FDirectConnectSmoke_StubTool>(
		TEXT("claireon_search"),
		TEXT("Search Claireon's editor tool catalogue (smoke stub).")));
	Server->RegisterTool(MakeShared<FDirectConnectSmoke_StubTool>(
		TEXT("claireon_execute"),
		TEXT("Execute Python inside the editor (smoke stub).")));

	const uint32 Candidate = DirectConnectSmoke_PickEphemeralPort();
	const bool bStarted = Server->Start(Candidate);
	UNTEST_ASSERT_TRUE(bStarted);

	const uint32 BoundPort = Server->GetPort();

	const FString Body = DirectConnectSmoke_BuildToolsListBody();
	int32 Status = 0;
	FString ResponseBody;
	const bool bOk = DirectConnectSmoke_PostJsonRpc(
		BoundPort, Body, /*TimeoutSeconds=*/ 5.0, Status, ResponseBody);
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
		if (ToolName == TEXT("claireon_search"))
		{
			bSawSearch = true;
		}
		else if (ToolName == TEXT("claireon_execute"))
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
// Stop releases the bound port: a fresh server can rebind it without the
// auto-increment retry kicking in. This guards against route-handle leaks
// that would silently steal ports from later test runs.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, DirectConnect, SmokePortReleased, UNTEST_TIMEOUTMS(10000))
{
	const uint32 Candidate = DirectConnectSmoke_PickEphemeralPort();

	auto First = MakeShared<FClaireonServer>();
	First->SetSessionToken(FString());
	UNTEST_ASSERT_TRUE(First->Start(Candidate));
	const uint32 BoundFirst = First->GetPort();
	First->Stop();
	UNTEST_ASSERT_FALSE(First->IsRunning());

	auto Second = MakeShared<FClaireonServer>();
	Second->SetSessionToken(FString());
	UNTEST_ASSERT_TRUE(Second->Start(BoundFirst));
	UNTEST_ASSERT_EQ(Second->GetPort(), BoundFirst);
	Second->Stop();
	co_return;
}

#endif // WITH_UNTESTED
