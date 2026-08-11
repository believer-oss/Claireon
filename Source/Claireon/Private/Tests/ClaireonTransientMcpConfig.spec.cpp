// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Unit spec for the transient MCP config builder introduced in stage 001.
// Exercises Claireon_Test_BuildTransientMcpConfigJson (the WITH_UNTESTED seam
// that wraps ClaireonLaunch::TransientMcpConfig_BuildJson) without any I/O or
// a running editor.
//
// Category: Claireon.TransientMcpConfig.* (run with the automation test
// filter "Claireon.TransientMcpConfig").

#if WITH_UNTESTED

#include "Untest.h"

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Forward-declare the test seam defined in ClaireonModule.cpp.
extern FString Claireon_Test_BuildTransientMcpConfigJson(const FString& ExistingMcpJsonContent, uint32 Port);

namespace ClaireonTransientMcpConfig_spec_Private
{
	// File-local prefix on all helpers to avoid unity-batching symbol collisions.

	/** Parse JSON into a root object; returns nullptr on failure. */
	TSharedPtr<FJsonObject> TransientMcpConfig_ParseJson(const FString& JsonStr)
	{
		TSharedPtr<FJsonObject> Out;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
		if (!FJsonSerializer::Deserialize(Reader, Out) || !Out.IsValid())
		{
			return nullptr;
		}
		return Out;
	}

	/**
	 * Build a minimal .mcp.json string containing the given server key/url pairs
	 * under mcpServers.  Used by several tests to construct project-side JSON.
	 * Returns only a string -- no UNTEST_ASSERT_* calls (those expand to co_return
	 * and cannot live in a non-coroutine helper).
	 */
	FString TransientMcpConfig_MakeProjectJson(const TArray<TPair<FString, FString>>& Entries)
	{
		const TSharedRef<FJsonObject> Servers = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Entry : Entries)
		{
			const TSharedRef<FJsonObject> ServerObj = MakeShared<FJsonObject>();
			ServerObj->SetStringField(TEXT("type"), TEXT("http"));
			ServerObj->SetStringField(TEXT("url"), Entry.Value);
			Servers->SetObjectField(Entry.Key, ServerObj);
		}
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("mcpServers"), Servers);

		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		return Out;
	}
} // namespace
using namespace ClaireonTransientMcpConfig_spec_Private;

// ---------------------------------------------------------------------------
// Key rename: "claireon" present, "unreal-editor" absent (empty project json).
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, TransientMcpConfig, KeyRename, UNTEST_TIMEOUTMS(2000))
{
	const FString Json = Claireon_Test_BuildTransientMcpConfigJson(FString(), 8017u);
	UNTEST_ASSERT_FALSE(Json.IsEmpty());

	const TSharedPtr<FJsonObject> Root = TransientMcpConfig_ParseJson(Json);
	UNTEST_ASSERT_VALID(Root);

	const TSharedPtr<FJsonObject>* ServersPtr = nullptr;
	UNTEST_ASSERT_TRUE(Root->TryGetObjectField(TEXT("mcpServers"), ServersPtr));
	UNTEST_ASSERT_PTR(ServersPtr);

	// "claireon" key must exist.
	UNTEST_ASSERT_TRUE((*ServersPtr)->HasField(TEXT("claireon")));

	// "unreal-editor" key must NOT exist.
	UNTEST_ASSERT_FALSE((*ServersPtr)->HasField(TEXT("unreal-editor")));

	co_return;
}

// ---------------------------------------------------------------------------
// Legacy key strip: "unreal-editor" and "claireon" from project json are
// filtered; a non-editor key passes through.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, TransientMcpConfig, LegacyKeyStrip, UNTEST_TIMEOUTMS(2000))
{
	TArray<TPair<FString, FString>> Entries;
	Entries.Add(TPair<FString, FString>(TEXT("unreal-editor"), TEXT("http://127.0.0.1:9002/mcp")));
	Entries.Add(TPair<FString, FString>(TEXT("claireon"),      TEXT("http://127.0.0.1:9003/mcp")));
	Entries.Add(TPair<FString, FString>(TEXT("bugsplat-mcp"),  TEXT("http://127.0.0.1:9004/mcp")));

	const FString ProjectJson = TransientMcpConfig_MakeProjectJson(Entries);
	const FString Json = Claireon_Test_BuildTransientMcpConfigJson(ProjectJson, 8017u);
	UNTEST_ASSERT_FALSE(Json.IsEmpty());

	const TSharedPtr<FJsonObject> Root = TransientMcpConfig_ParseJson(Json);
	UNTEST_ASSERT_VALID(Root);

	const TSharedPtr<FJsonObject>* ServersPtr = nullptr;
	UNTEST_ASSERT_TRUE(Root->TryGetObjectField(TEXT("mcpServers"), ServersPtr));
	UNTEST_ASSERT_PTR(ServersPtr);

	// Filtered editor key must not appear.
	UNTEST_ASSERT_FALSE((*ServersPtr)->HasField(TEXT("unreal-editor")));

	// The fresh "claireon" entry must be present (added by the function, not forwarded from project).
	UNTEST_ASSERT_TRUE((*ServersPtr)->HasField(TEXT("claireon")));

	// Non-editor passthrough key must survive.
	UNTEST_ASSERT_TRUE((*ServersPtr)->HasField(TEXT("bugsplat-mcp")));

	co_return;
}

// ---------------------------------------------------------------------------
// Parse failure: invalid project JSON -> claireon-only config returned,
// no crash.  The warning is verified by checking the editor log after the run;
// capturing it via FClaireonLogCapture is unreliable in commandlet mode
// (GLog device registration may be deferred, causing the capture to miss
// messages emitted immediately after AddOutputDevice returns).
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, TransientMcpConfig, ParseFailureContinues, UNTEST_TIMEOUTMS(2000))
{
	const FString Json = Claireon_Test_BuildTransientMcpConfigJson(
		TEXT("{ this is not valid json !!! }"), 8017u);

	// Must return a non-empty JSON string (claireon-only config, no crash).
	UNTEST_ASSERT_FALSE(Json.IsEmpty());

	// The returned config must be valid JSON with exactly a "claireon" server
	// and no other servers (the bad project JSON contributed nothing).
	const TSharedPtr<FJsonObject> Root = TransientMcpConfig_ParseJson(Json);
	UNTEST_ASSERT_VALID(Root);

	const TSharedPtr<FJsonObject>* ServersPtr = nullptr;
	UNTEST_ASSERT_TRUE(Root->TryGetObjectField(TEXT("mcpServers"), ServersPtr));
	UNTEST_ASSERT_PTR(ServersPtr);
	UNTEST_ASSERT_TRUE((*ServersPtr)->HasField(TEXT("claireon")));

	// No spurious keys from the bad project JSON should appear.
	UNTEST_ASSERT_EQ((*ServersPtr)->Values.Num(), 1);

	co_return;
}

// ---------------------------------------------------------------------------
// Port in URL: generated JSON contains the passed port in claireon.url.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, TransientMcpConfig, PortInUrl, UNTEST_TIMEOUTMS(2000))
{
	const uint32 TestPort = 59583u;
	const FString Json = Claireon_Test_BuildTransientMcpConfigJson(FString(), TestPort);
	UNTEST_ASSERT_FALSE(Json.IsEmpty());

	const TSharedPtr<FJsonObject> Root = TransientMcpConfig_ParseJson(Json);
	UNTEST_ASSERT_VALID(Root);

	const TSharedPtr<FJsonObject>* ServersPtr = nullptr;
	UNTEST_ASSERT_TRUE(Root->TryGetObjectField(TEXT("mcpServers"), ServersPtr));
	UNTEST_ASSERT_PTR(ServersPtr);

	const TSharedPtr<FJsonObject>* ClaireonEntryPtr = nullptr;
	UNTEST_ASSERT_TRUE((*ServersPtr)->TryGetObjectField(TEXT("claireon"), ClaireonEntryPtr));
	UNTEST_ASSERT_PTR(ClaireonEntryPtr);

	FString Url;
	UNTEST_ASSERT_TRUE((*ClaireonEntryPtr)->TryGetStringField(TEXT("url"), Url));

	const FString ExpectedPortStr = FString::Printf(TEXT(":%u/"), TestPort);
	UNTEST_ASSERT_TRUE(Url.Contains(ExpectedPortStr));

	co_return;
}

#endif // WITH_UNTESTED
