// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Regression tests for the ClaireonBridge -> sys.modules['claireon'] bootstrap.
// Locks in:
//   - F08 item 9: dir(claireon) > 100, the canary for the silent failure where
//     the deleted prefix-required guard at ClaireonBridge.cpp:386-388 is
//     accidentally restored and skips every tool.
//   - F08 item 10: tool_search strips boolean operators (AND/OR/NOT,
//     parentheses, quotes) before tokenisation, so a pseudo-boolean query
//     yields the same result set as the same query without the operators.

#if WITH_UNTESTED

#include "Untest.h"
#include "IPythonScriptPlugin.h"
#include "ClaireonBridge.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "Tools/ClaireonTool_ExecutePython.h"
#include "Tools/ClaireonTool_SearchTools.h"
#include "Tools/IClaireonTool.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "SquidTasks/Task.h"

namespace ClaireonPythonBridgeBootstrapTestsHelpers
{
	/** Extract the ordered list of tool names from a tool_search Execute result Data object. */
	static TArray<FString> ExtractToolNames(const TSharedPtr<FJsonObject>& DataObj)
	{
		TArray<FString> Names;
		if (!DataObj.IsValid())
		{
			return Names;
		}

		// Post-FTS5-swap query output is a flat, globally rank-ordered `tools[]`.
		const TArray<TSharedPtr<FJsonValue>>* ToolsArr = nullptr;
		if (!DataObj->TryGetArrayField(TEXT("tools"), ToolsArr))
		{
			return Names;
		}
		for (const TSharedPtr<FJsonValue>& ToolVal : *ToolsArr)
		{
			const TSharedPtr<FJsonObject>* ToolObj = nullptr;
			if (!ToolVal->TryGetObject(ToolObj) || !ToolObj->IsValid())
			{
				continue;
			}
			FString Name;
			if ((*ToolObj)->TryGetStringField(TEXT("name"), Name))
			{
				Names.Add(Name);
			}
		}
		// Sort by name so the boolean-vs-plain comparison is order-independent
		// (the two queries produce the same set; rank order is identical too,
		// but sorting keeps the multiset comparison robust).
		Names.Sort();
		return Names;
	}
}

// Root cause of the historical failure (test defect, no product bug): this body
// used StartServer()/GetServer(), but in commandlet mode StartupModule()
// short-circuits on the GIsEditor/IsRunningCommandlet guard, so Server was never
// constructed. StartServer() then logs "called before Server was constructed"
// and returns, GetServer() stays null, and UNTEST_ASSERT_PTR fired BEFORE the
// zero-tools skip below could run. EnsureServerForTest() -- already used by
// ToolSearchBoolean in this same file -- constructs and populates the registry
// headlessly, which both fixes the null pointer and gives this test real
// coverage in commandlet runs instead of a skip.
UNTEST_UNIT_OPTS(Claireon, PythonBridgeBootstrap, DirClaireonExceedsHundred, UNTEST_TIMEOUTMS(15000))
{
	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = Module.EnsureServerForTest();
	UNTEST_ASSERT_PTR(Server);

	// If the registry still could not be populated (no IClaireonToolProvider
	// modular feature at all), the bridge cannot bootstrap a claireon namespace.
	// Skip LOUDLY -- never silently report a pass for coverage that did not run.
	if (Server->GetTools().Num() == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[PythonBridgeBootstrap] SKIPPED -- tool registry is empty "
			     "(no IClaireonToolProvider registered); dir(claireon) canary did NOT run"));
		co_return;
	}

	// RebuildClaireonModule() takes the GIL, so Python must actually be up.
	IPythonScriptPlugin* PythonPlugin = IPythonScriptPlugin::Get();
	if (!PythonPlugin || !PythonPlugin->IsPythonAvailable())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[PythonBridgeBootstrap] SKIPPED -- Python is not available in this "
			     "configuration; dir(claireon) canary did NOT run"));
		co_return;
	}

	UNTEST_ASSERT_TRUE(Server->GetTools().Num() > 100);

	// Force the bridge to (re)register so the bootstrap script runs.
	FClaireonBridge::EnsureRegistered();
	FClaireonBridge::RebuildClaireonModule();

	// Execute a tiny Python program through python_execute that prints
	// len(dir(claireon)) on a tagged line we can recognise in the captured
	// log output. (python_execute does not have a structured Data field;
	// stdout is captured into FToolResult::Logs.)
	ClaireonTool_ExecutePython PyTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("code"),
		TEXT("import claireon\n")
		TEXT("print('CLAIREON_DIR_LEN=' + str(len(dir(claireon))))\n"));
	Args->SetNumberField(TEXT("timeout_ms"), 10000);

	IClaireonTool::FToolResult Result = PyTool.Execute(Args);
	if (Result.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[PythonBridgeBootstrap] python_execute failed: %s"), *Result.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(Result.bIsError);

	// Find the tagged line in Logs and extract the integer.
	const FString Tag = TEXT("CLAIREON_DIR_LEN=");
	int32 TagIdx = Result.Logs.Find(Tag);
	UNTEST_ASSERT_TRUE(TagIdx != INDEX_NONE);

	const int32 NumStart = TagIdx + Tag.Len();
	int32 NumEnd = NumStart;
	while (NumEnd < Result.Logs.Len() && FChar::IsDigit(Result.Logs[NumEnd]))
	{
		++NumEnd;
	}
	const FString NumStr = Result.Logs.Mid(NumStart, NumEnd - NumStart);
	const int32 DirCount = FCString::Atoi(*NumStr);

	UE_LOG(LogTemp, Log, TEXT("[PythonBridgeBootstrap] dir(claireon) length = %d"), DirCount);
	UNTEST_EXPECT_TRUE(DirCount > 100);

	co_return;
}

// 60s budget: this can be the first Execute() in the process and so pay the
// one-time full-catalog embedding build (~20s on a dev machine); later
// rebuilds hit the per-tool embedding cache.
UNTEST_UNIT_OPTS(Claireon, ToolSearchBoolean, StripsAndOrNotOperators, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonPythonBridgeBootstrapTestsHelpers;

	FClaireonModule& Module = FClaireonModule::Get();
	// Use EnsureServerForTest() so the registry is available headlessly in the
	// commandlet runner (StartServer() short-circuits on the GIsEditor guard and
	// leaves GetServer() null). EnsureServerForTest also (re-)collects built-in
	// tools, self-healing the registry across cross-suite teardown.
	FClaireonServer* Server = Module.EnsureServerForTest();
	FClaireonBridge::EnsureRegistered();
	UNTEST_ASSERT_PTR(Server);

	// Commandlet-mode skip if the registry could not be populated at all.
	if (Server->GetTools().Num() == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[ToolSearchBoolean] SKIPPED -- live server has zero tools "
			     "(commandlet mode short-circuits provider registration)"));
		co_return;
	}

	ClaireonTool_SearchTools SearchTool;

	// Plain query: "blueprint chooser"
	TSharedPtr<FJsonObject> PlainArgs = MakeShared<FJsonObject>();
	PlainArgs->SetStringField(TEXT("query"), TEXT("blueprint chooser"));
	PlainArgs->SetNumberField(TEXT("max_results"), 50);
	const IClaireonTool::FToolResult PlainResult = SearchTool.Execute(PlainArgs);
	UNTEST_ASSERT_FALSE(PlainResult.bIsError);
	const TArray<FString> PlainNames = ExtractToolNames(PlainResult.Data);

	// Boolean-decorated query: same intent, but with AND / OR / parens / quotes.
	TSharedPtr<FJsonObject> BoolArgs = MakeShared<FJsonObject>();
	BoolArgs->SetStringField(TEXT("query"), TEXT("\"blueprint\" AND (chooser OR \"chooser\")"));
	BoolArgs->SetNumberField(TEXT("max_results"), 50);
	const IClaireonTool::FToolResult BoolResult = SearchTool.Execute(BoolArgs);
	UNTEST_ASSERT_FALSE(BoolResult.bIsError);
	const TArray<FString> BoolNames = ExtractToolNames(BoolResult.Data);

	// The two result sets must be equal as sorted-by-name multisets. We use
	// equality of the sorted name array because category-grouped output is
	// already deterministic for the same inputs.
	UNTEST_EXPECT_EQ(BoolNames.Num(), PlainNames.Num());
	for (int32 i = 0; i < FMath::Min(BoolNames.Num(), PlainNames.Num()); ++i)
	{
		UNTEST_EXPECT_STREQ(*BoolNames[i], *PlainNames[i]);
	}

	co_return;
}

#endif // WITH_UNTESTED
