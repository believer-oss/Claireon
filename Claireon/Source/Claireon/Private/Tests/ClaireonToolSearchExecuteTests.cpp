// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Integration tests for ClaireonTool_SearchTools::Execute.
// Exercises the full search pipeline: exact-name precedence, near-exact
// (Levenshtein) precedence, category-filter interaction with pinned entries,
// defensive substring-fallback merge, OnToolsChanged auto-invalidation,
// and the name= / tool_name= parameter alias semantics.
//
// Strategy: each case ensures the live FClaireonServer is running, seeds it
// with fixture FStubSearchTool instances, calls Execute() directly with a
// constructed argument JSON object, then asserts on the FToolResult JSON
// payload. Stub tools are unregistered in teardown to keep the live registry
// clean for the next test.

#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonLogCapture.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "Tools/ClaireonTool_SearchTools.h"
#include "Tools/IClaireonTool.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "SquidTasks/Task.h"

namespace ClaireonToolSearchExecuteTestsNS
{
	// File-local discriminator to avoid translation-unit collisions under unity builds.

	/** Configurable stub tool: callers pick Category / Operation explicitly so
	 *  GetName() yields the desired wire name without underscore-splitting heuristics. */
	class FStubSearchTool : public IClaireonTool
	{
	public:
		FStubSearchTool(FString InCategory, FString InOperation, FString InDescription)
			: CategoryStr(MoveTemp(InCategory))
			, OperationStr(MoveTemp(InOperation))
			, DescriptionStr(MoveTemp(InDescription))
		{
		}

		virtual FString GetCategory() const override { return CategoryStr; }
		virtual FString GetOperation() const override { return OperationStr; }
		virtual FString GetDescription() const override { return DescriptionStr; }

		virtual TSharedPtr<FJsonObject> GetInputSchema() const override
		{
			TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("object"));
			TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
			Schema->SetObjectField(TEXT("properties"), Properties);
			TArray<TSharedPtr<FJsonValue>> Required;
			Schema->SetArrayField(TEXT("required"), Required);
			return Schema;
		}

		virtual FToolResult Execute(const TSharedPtr<FJsonObject>& /*Arguments*/) override
		{
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			return MakeSuccessResult(Data, TEXT("stub ok"));
		}

	private:
		FString CategoryStr;
		FString OperationStr;
		FString DescriptionStr;
	};

	static TSharedPtr<IClaireonTool> MakeTool(const FString& Category, const FString& Operation, const FString& Description = TEXT("stub tool for ClaireonToolSearchExecuteTests"))
	{
		return MakeShared<FStubSearchTool>(Category, Operation, Description);
	}

	static bool EnsureServer(FClaireonModule& Module)
	{
		if (!Module.IsServerRunning())
		{
			Module.StartServer();
			return true;
		}
		return false;
	}

	/** Find a tool entry by name in the Execute() response's `categories[].tools[]` shape.
	 *  Returns the entry's JSON object or null if absent. */
	static TSharedPtr<FJsonObject> FindToolInResult(const TSharedPtr<FJsonObject>& Data, const FString& Name)
	{
		if (!Data.IsValid()) { return nullptr; }
		const TArray<TSharedPtr<FJsonValue>>* Cats = nullptr;
		if (!Data->TryGetArrayField(TEXT("categories"), Cats) || !Cats) { return nullptr; }
		for (const TSharedPtr<FJsonValue>& CatVal : *Cats)
		{
			const TSharedPtr<FJsonObject>* CatObj = nullptr;
			if (!CatVal->TryGetObject(CatObj) || !CatObj || !(*CatObj).IsValid()) { continue; }
			const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
			if (!(*CatObj)->TryGetArrayField(TEXT("tools"), Tools) || !Tools) { continue; }
			for (const TSharedPtr<FJsonValue>& ToolVal : *Tools)
			{
				const TSharedPtr<FJsonObject>* ToolObj = nullptr;
				if (!ToolVal->TryGetObject(ToolObj) || !ToolObj || !(*ToolObj).IsValid()) { continue; }
				FString N;
				if ((*ToolObj)->TryGetStringField(TEXT("name"), N) && N == Name)
				{
					return *ToolObj;
				}
			}
		}
		return nullptr;
	}

	/** Return the first tool name across the response's categories (categories are sorted
	 *  alphabetically by name; the pinned entry's category contains it at position 0). */
	static FString FirstToolName(const TSharedPtr<FJsonObject>& Data)
	{
		// The Execute response sorts MatchingTools with pinned-name short-circuit,
		// so the pinned entry is the first element of the per-category tools array
		// for the pinned tool's own category.
		const TArray<TSharedPtr<FJsonValue>>* Cats = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(TEXT("categories"), Cats) || !Cats || Cats->Num() == 0) { return FString(); }
		for (const TSharedPtr<FJsonValue>& CatVal : *Cats)
		{
			const TSharedPtr<FJsonObject>* CatObj = nullptr;
			if (!CatVal->TryGetObject(CatObj) || !CatObj || !(*CatObj).IsValid()) { continue; }
			const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
			if (!(*CatObj)->TryGetArrayField(TEXT("tools"), Tools) || !Tools || Tools->Num() == 0) { continue; }
			const TSharedPtr<FJsonObject>* ToolObj = nullptr;
			if ((*Tools)[0]->TryGetObject(ToolObj) && ToolObj && (*ToolObj).IsValid())
			{
				FString N;
				if ((*ToolObj)->TryGetStringField(TEXT("name"), N))
				{
					return N;
				}
			}
		}
		return FString();
	}

	static int32 GetTotalMatching(const TSharedPtr<FJsonObject>& Data)
	{
		double V = 0.0;
		if (Data.IsValid() && Data->TryGetNumberField(TEXT("total_matching"), V))
		{
			return static_cast<int32>(V);
		}
		return -1;
	}

	static IClaireonTool::FToolResult RunExecute(ClaireonTool_SearchTools& Tool, const TMap<FString, FString>& Args)
	{
		TSharedPtr<FJsonObject> ArgsObj = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& KV : Args)
		{
			ArgsObj->SetStringField(KV.Key, KV.Value);
		}
		return Tool.Execute(ArgsObj);
	}

	/** Register a list of stub tools, returning their names for teardown. */
	static TArray<FString> RegisterStubs(FClaireonServer& Server, const TArray<TSharedPtr<IClaireonTool>>& Stubs)
	{
		TArray<FString> Names;
		Names.Reserve(Stubs.Num());
		for (const TSharedPtr<IClaireonTool>& Stub : Stubs)
		{
			Names.Add(Stub->GetName());
			Server.RegisterTool(Stub);
		}
		return Names;
	}

	static void UnregisterStubs(FClaireonServer& Server, const TArray<FString>& Names)
	{
		for (const FString& N : Names)
		{
			Server.UnregisterTool(N);
		}
	}
}

// ===========================================================================
// Exact-name pin bypasses category filter; near-exact respects it.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, ExactNamePrecedencePinsWithCategoryMismatch, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	const TArray<FString> Names = RegisterStubs(*Server, {
		MakeTool(TEXT("chooser"), TEXT("create"), TEXT("create a chooser table")),
	});

	ClaireonTool_SearchTools SearchTool;
	IClaireonTool::FToolResult R = RunExecute(SearchTool, {
		{ TEXT("query"),    TEXT("chooser_create") },
		{ TEXT("category"), TEXT("render") },
	});

	UNTEST_EXPECT_FALSE(R.bIsError);
	UNTEST_EXPECT_STREQ(*FirstToolName(R.Data), TEXT("chooser_create"));
	UNTEST_EXPECT_EQ(GetTotalMatching(R.Data), 1);

	UnregisterStubs(*Server, Names);
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

// ===========================================================================
// Hyphen/underscore equivalence for exact-name normalisation.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, ExactNameHyphenUnderscoreEquivalence, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	const TArray<FString> Names = RegisterStubs(*Server, {
		MakeTool(TEXT("chooser"), TEXT("create"), TEXT("create a chooser table")),
	});

	ClaireonTool_SearchTools SearchTool;
	IClaireonTool::FToolResult R = RunExecute(SearchTool, {
		{ TEXT("query"), TEXT("chooser-create") },
	});

	UNTEST_EXPECT_FALSE(R.bIsError);
	UNTEST_EXPECT_STREQ(*FirstToolName(R.Data), TEXT("chooser_create"));

	UnregisterStubs(*Server, Names);
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

// ===========================================================================
// Levenshtein near-match pins typo; distance > 2 does not pin.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, LevenshteinNearMatchPinsTypo, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	const TArray<FString> Names = RegisterStubs(*Server, {
		MakeTool(TEXT("chooser"), TEXT("create"), TEXT("create a chooser table")),
	});

	ClaireonTool_SearchTools SearchTool;

	// Sub-case A: distance 1 (trailing 'e' deleted).
	{
		IClaireonTool::FToolResult R = RunExecute(SearchTool, {
			{ TEXT("query"), TEXT("chooser_creat") },
		});
		UNTEST_EXPECT_FALSE(R.bIsError);
		UNTEST_EXPECT_STREQ(*FirstToolName(R.Data), TEXT("chooser_create"));
	}

	// Sub-case B: distance 1 ('e' deleted from 'chooser').
	{
		IClaireonTool::FToolResult R = RunExecute(SearchTool, {
			{ TEXT("query"), TEXT("choosr_create") },
		});
		UNTEST_EXPECT_FALSE(R.bIsError);
		UNTEST_EXPECT_STREQ(*FirstToolName(R.Data), TEXT("chooser_create"));
	}

	// Sub-case C: distance > 2 -- not pinned. chooser_create must not be at position 0.
	{
		IClaireonTool::FToolResult R = RunExecute(SearchTool, {
			{ TEXT("query"), TEXT("completely_unrelated") },
		});
		UNTEST_EXPECT_FALSE(R.bIsError);
		const FString First = FirstToolName(R.Data);
		UNTEST_EXPECT_FALSE(First == TEXT("chooser_create"));
	}

	UnregisterStubs(*Server, Names);
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

// ===========================================================================
// Near-exact pin respects category filter (zero results when mismatched).
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, NearExactRespectsCategoryFilter, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	const TArray<FString> Names = RegisterStubs(*Server, {
		MakeTool(TEXT("chooser"), TEXT("create"), TEXT("create a chooser table")),
		MakeTool(TEXT("renderfoo"), TEXT("placeholder"), TEXT("unrelated placeholder")),
	});

	ClaireonTool_SearchTools SearchTool;
	IClaireonTool::FToolResult R = RunExecute(SearchTool, {
		{ TEXT("query"),    TEXT("chooser_creat") }, // distance 1 from chooser_create
		{ TEXT("category"), TEXT("render") },        // chooser_create's category is "chooser", not "render"
	});

	UNTEST_EXPECT_FALSE(R.bIsError);
	// chooser_create must NOT be pinned because category mismatches and near-exact respects category.
	UNTEST_EXPECT_FALSE(FirstToolName(R.Data) == TEXT("chooser_create"));

	UnregisterStubs(*Server, Names);
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

// ===========================================================================
// OnToolsChanged auto-invalidation -- rename in place flips dirty bit so
// the next Execute call rebuilds the catalog without an explicit RebuildCatalog.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, OnToolsChangedAutoInvalidates, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	// Step 1: register widget_create and run a search so the catalog gets built.
	TSharedPtr<IClaireonTool> CreateStub = MakeTool(TEXT("widget"), TEXT("create"), TEXT("create a widget asset"));
	Server->RegisterTool(CreateStub);

	ClaireonTool_SearchTools SearchTool;
	{
		IClaireonTool::FToolResult R = RunExecute(SearchTool, {
			{ TEXT("query"), TEXT("widget_create") },
		});
		UNTEST_EXPECT_FALSE(R.bIsError);
		UNTEST_EXPECT_TRUE(FindToolInResult(R.Data, TEXT("widget_create")).IsValid());
	}

	// Step 2: mutate the registry -- unregister widget_create, register widget_make.
	// Net tool count is unchanged, so the count-based heuristic does NOT trip;
	// the dirty bit is what forces the rebuild.
	Server->UnregisterTool(TEXT("widget_create"));
	TSharedPtr<IClaireonTool> MakeStub = MakeTool(TEXT("widget"), TEXT("make"), TEXT("make a widget asset"));
	Server->RegisterTool(MakeStub);

	// Step 3: without calling RebuildCatalog() explicitly, search for the new name.
	{
		IClaireonTool::FToolResult R = RunExecute(SearchTool, {
			{ TEXT("query"), TEXT("widget_make") },
		});
		UNTEST_EXPECT_FALSE(R.bIsError);
		UNTEST_EXPECT_TRUE(FindToolInResult(R.Data, TEXT("widget_make")).IsValid());
		UNTEST_EXPECT_FALSE(FindToolInResult(R.Data, TEXT("widget_create")).IsValid());
	}

	Server->UnregisterTool(TEXT("widget_make"));
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

// ===========================================================================
// "asset create" must return at least one asset_* tool. Uses controlled stub
// fixture so the test does not depend on the live tool set.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, AssetCreateReturnsAssetTool, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	const TArray<FString> Names = RegisterStubs(*Server, {
		MakeTool(TEXT("asset"), TEXT("create"),      TEXT("create a new asset on disk")),
		MakeTool(TEXT("asset"), TEXT("delete"),      TEXT("delete an asset from disk")),
		MakeTool(TEXT("widget"), TEXT("create"),     TEXT("create a widget asset")),
	});

	ClaireonTool_SearchTools SearchTool;
	IClaireonTool::FToolResult R = RunExecute(SearchTool, {
		{ TEXT("query"), TEXT("asset create") },
	});

	UNTEST_EXPECT_FALSE(R.bIsError);
	UNTEST_EXPECT_TRUE(FindToolInResult(R.Data, TEXT("asset_create")).IsValid());

	UnregisterStubs(*Server, Names);
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

// ===========================================================================
// Defensive substring-fallback merge -- substring hits unioned in when
// fuzzy results don't contain any query word as a substring.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, DefensiveSubstringFallbackMerge, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	// Build a fixture where a widget_* tool exists so the substring-fallback
	// path has something to surface for the "widget" query. We don't try to
	// reverse-engineer the abbreviation expansion -- the assertion is that
	// the widget_* tool ends up in the merged result set.
	const TArray<FString> Names = RegisterStubs(*Server, {
		MakeTool(TEXT("widget"), TEXT("create"), TEXT("create a widget asset")),
	});

	ClaireonTool_SearchTools SearchTool;
	IClaireonTool::FToolResult R = RunExecute(SearchTool, {
		{ TEXT("query"), TEXT("widget") },
	});

	UNTEST_EXPECT_FALSE(R.bIsError);
	UNTEST_EXPECT_TRUE(FindToolInResult(R.Data, TEXT("widget_create")).IsValid());

	UnregisterStubs(*Server, Names);
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

// ===========================================================================
// name= is a true alias of tool_name= for the deep-inspect bypass.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, NameParameterAliasesToolName, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	const TArray<FString> Names = RegisterStubs(*Server, {
		MakeTool(TEXT("blueprint"), TEXT("compile"), TEXT("compile a blueprint asset")),
	});

	ClaireonTool_SearchTools SearchTool;

	// Via tool_name= (legacy)
	IClaireonTool::FToolResult RT = RunExecute(SearchTool, {
		{ TEXT("tool_name"), TEXT("blueprint_compile") },
	});
	UNTEST_EXPECT_FALSE(RT.bIsError);
	// Deep-inspect data has deep_inspect=true and a top-level "tool" object.
	UNTEST_ASSERT_TRUE(RT.Data.IsValid());
	bool bDeepInspect = false;
	UNTEST_EXPECT_TRUE(RT.Data->TryGetBoolField(TEXT("deep_inspect"), bDeepInspect) && bDeepInspect);

	// Via name= (new alias)
	IClaireonTool::FToolResult RN = RunExecute(SearchTool, {
		{ TEXT("name"), TEXT("blueprint_compile") },
	});
	UNTEST_EXPECT_FALSE(RN.bIsError);
	UNTEST_ASSERT_TRUE(RN.Data.IsValid());
	bool bDeepInspect2 = false;
	UNTEST_EXPECT_TRUE(RN.Data->TryGetBoolField(TEXT("deep_inspect"), bDeepInspect2) && bDeepInspect2);

	UnregisterStubs(*Server, Names);
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

// ===========================================================================
// name= wins when both name= and tool_name= are set; a Display log
// "Deep-inspect: both `name=` and `tool_name=` set; using `name=` value."
// fires exactly once.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, NameParameterWinsOverToolNameWithLog, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	const TArray<FString> Names = RegisterStubs(*Server, {
		MakeTool(TEXT("blueprint"), TEXT("compile"), TEXT("compile a blueprint asset")),
		MakeTool(TEXT("asset"),     TEXT("search"),  TEXT("search for an asset")),
	});

	ClaireonTool_SearchTools SearchTool;

	// Capture Display-level logs (verbosity numeric value is higher than Warning;
	// FClaireonLogCapture filters `Verbosity > MinVerbosity` so Display passes
	// when min=Display).
	FString CapturedLog;
	IClaireonTool::FToolResult R;
	{
		FClaireonLogCapture Capture(ELogVerbosity::Display);
		R = RunExecute(SearchTool, {
			{ TEXT("name"),      TEXT("blueprint_compile") },
			{ TEXT("tool_name"), TEXT("asset_search") },
		});
		CapturedLog = Capture.GetCapturedOutput();
	}

	UNTEST_EXPECT_FALSE(R.bIsError);
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	// name= won -- response is deep-inspect for blueprint_compile.
	const TSharedPtr<FJsonObject>* InspectedTool = nullptr;
	if (R.Data->TryGetObjectField(TEXT("tool"), InspectedTool) && InspectedTool && (*InspectedTool).IsValid())
	{
		FString InspectedName;
		(*InspectedTool)->TryGetStringField(TEXT("name"), InspectedName);
		UNTEST_EXPECT_STREQ(*InspectedName, TEXT("blueprint_compile"));
	}
	else
	{
		UNTEST_EXPECT_TRUE(false); // missing tool object in response
	}

	// The dual-set Display log fired.
	UNTEST_EXPECT_TRUE(CapturedLog.Contains(TEXT("both `name=` and `tool_name=` set")));

	UnregisterStubs(*Server, Names);
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

#endif // WITH_UNTESTED
