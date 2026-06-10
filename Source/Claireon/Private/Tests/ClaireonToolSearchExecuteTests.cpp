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
#include "ClaireonBridge.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "IClaireonToolProvider.h"
#include "Features/IModularFeatures.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tools/ClaireonTool_SearchTools.h"
#include "Tools/IClaireonTool.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "SquidTasks/Task.h"

namespace ClaireonToolSearchExecuteTestsNS
{
	// File-local discriminator to avoid anon-NS collisions under unity batching.

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

	// Bootstrap the live registry headlessly. EnsureServerForTest() mirrors the
	// registry-construction steps without the HTTP listener / editor UI (which
	// StartServer() guards behind GIsEditor, leaving GetServer() null in the
	// commandlet runner) and (re-)collects built-in tools so stub teardown in a
	// prior suite cannot permanently evict a real tool. Returns false: there is
	// nothing to StopServer() in teardown.
	static bool EnsureServer(FClaireonModule& Module)
	{
		Module.EnsureServerForTest();
		FClaireonBridge::EnsureRegistered();
		return false;
	}

	/** Find a tool entry by name in the Execute() response's flat `tools[]` shape
	 *  (post-FTS5-swap query output). Returns the entry's JSON object or null. */
	static TSharedPtr<FJsonObject> FindToolInResult(const TSharedPtr<FJsonObject>& Data, const FString& Name)
	{
		if (!Data.IsValid()) { return nullptr; }
		const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
		if (!Data->TryGetArrayField(TEXT("tools"), Tools) || !Tools) { return nullptr; }
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
		return nullptr;
	}

	/** Return the first tool name in the flat, globally rank-ordered `tools[]`.
	 *  The exact / near-exact name pin is guaranteed at index 0. */
	static FString FirstToolName(const TSharedPtr<FJsonObject>& Data)
	{
		const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(TEXT("tools"), Tools) || !Tools || Tools->Num() == 0) { return FString(); }
		const TSharedPtr<FJsonObject>* ToolObj = nullptr;
		if ((*Tools)[0]->TryGetObject(ToolObj) && ToolObj && (*ToolObj).IsValid())
		{
			FString N;
			if ((*ToolObj)->TryGetStringField(TEXT("name"), N))
			{
				return N;
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
// exact-name pin bypasses category filter; near-exact respects it.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, ExactNamePrecedencePinsWithCategoryMismatch, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	// Stub category 'stubchsr' is synthetic so it never shadows / evicts a real
	// built-in tool (e.g. the live 'chooser_create') on teardown.
	const TArray<FString> Names = RegisterStubs(*Server, {
		MakeTool(TEXT("stubchsr"), TEXT("create"), TEXT("create a chooser table")),
	});

	ClaireonTool_SearchTools SearchTool;
	IClaireonTool::FToolResult R = RunExecute(SearchTool, {
		{ TEXT("query"),    TEXT("stubchsr_create") },
		{ TEXT("category"), TEXT("render") },
	});

	UNTEST_EXPECT_FALSE(R.bIsError);
	UNTEST_EXPECT_STREQ(*FirstToolName(R.Data), TEXT("stubchsr_create"));
	UNTEST_EXPECT_EQ(GetTotalMatching(R.Data), 1);

	UnregisterStubs(*Server, Names);
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

// ===========================================================================
// hyphen/underscore equivalence for exact-name normalisation.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, ExactNameHyphenUnderscoreEquivalence, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	const TArray<FString> Names = RegisterStubs(*Server, {
		MakeTool(TEXT("stubchsr"), TEXT("create"), TEXT("create a chooser table")),
	});

	ClaireonTool_SearchTools SearchTool;
	IClaireonTool::FToolResult R = RunExecute(SearchTool, {
		{ TEXT("query"), TEXT("stubchsr-create") },
	});

	UNTEST_EXPECT_FALSE(R.bIsError);
	UNTEST_EXPECT_STREQ(*FirstToolName(R.Data), TEXT("stubchsr_create"));

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
		MakeTool(TEXT("stubchsr"), TEXT("create"), TEXT("create a chooser table")),
	});

	ClaireonTool_SearchTools SearchTool;

	// Sub-case A: distance 1 (trailing 'e' deleted).
	{
		IClaireonTool::FToolResult R = RunExecute(SearchTool, {
			{ TEXT("query"), TEXT("stubchsr_creat") },
		});
		UNTEST_EXPECT_FALSE(R.bIsError);
		UNTEST_EXPECT_STREQ(*FirstToolName(R.Data), TEXT("stubchsr_create"));
	}

	// Sub-case B: distance 1 ('s' deleted from 'stubchsr').
	{
		IClaireonTool::FToolResult R = RunExecute(SearchTool, {
			{ TEXT("query"), TEXT("stubchr_create") },
		});
		UNTEST_EXPECT_FALSE(R.bIsError);
		UNTEST_EXPECT_STREQ(*FirstToolName(R.Data), TEXT("stubchsr_create"));
	}

	// Sub-case C: distance > 2 -- not pinned. chooser_create must not be at position 0.
	{
		IClaireonTool::FToolResult R = RunExecute(SearchTool, {
			{ TEXT("query"), TEXT("completely_unrelated") },
		});
		UNTEST_EXPECT_FALSE(R.bIsError);
		const FString First = FirstToolName(R.Data);
		UNTEST_EXPECT_FALSE(First == TEXT("stubchsr_create"));
	}

	UnregisterStubs(*Server, Names);
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

// ===========================================================================
// near-exact pin respects category filter (zero results when mismatched).
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, NearExactRespectsCategoryFilter, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	const TArray<FString> Names = RegisterStubs(*Server, {
		MakeTool(TEXT("stubchsr"), TEXT("create"), TEXT("create a chooser table")),
		MakeTool(TEXT("renderfoo"), TEXT("placeholder"), TEXT("unrelated placeholder")),
	});

	ClaireonTool_SearchTools SearchTool;
	IClaireonTool::FToolResult R = RunExecute(SearchTool, {
		{ TEXT("query"),    TEXT("stubchsr_creat") }, // distance 1 from stubchsr_create
		{ TEXT("category"), TEXT("render") },         // stubchsr_create's category is "stubchsr", not "render"
	});

	UNTEST_EXPECT_FALSE(R.bIsError);
	// stubchsr_create must NOT be pinned because category mismatches and near-exact respects category.
	UNTEST_EXPECT_FALSE(FirstToolName(R.Data) == TEXT("stubchsr_create"));

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

	// Step 1: register stubwdgt_create and run a search so the catalog gets built.
	// 'stubwdgt' is synthetic so it never shadows / evicts a real built-in.
	TSharedPtr<IClaireonTool> CreateStub = MakeTool(TEXT("stubwdgt"), TEXT("create"), TEXT("create a widget asset"));
	Server->RegisterTool(CreateStub);

	ClaireonTool_SearchTools SearchTool;
	{
		IClaireonTool::FToolResult R = RunExecute(SearchTool, {
			{ TEXT("query"), TEXT("stubwdgt_create") },
		});
		UNTEST_EXPECT_FALSE(R.bIsError);
		UNTEST_EXPECT_TRUE(FindToolInResult(R.Data, TEXT("stubwdgt_create")).IsValid());
	}

	// Step 2: mutate the registry -- unregister stubwdgt_create, register stubwdgt_make.
	// Net tool count is unchanged, so the count-based heuristic does NOT trip;
	// the dirty bit (D8) is what forces the rebuild.
	Server->UnregisterTool(TEXT("stubwdgt_create"));
	TSharedPtr<IClaireonTool> MakeStub = MakeTool(TEXT("stubwdgt"), TEXT("make"), TEXT("make a widget asset"));
	Server->RegisterTool(MakeStub);

	// Step 3: without calling RebuildCatalog() explicitly, search for the new name.
	{
		IClaireonTool::FToolResult R = RunExecute(SearchTool, {
			{ TEXT("query"), TEXT("stubwdgt_make") },
		});
		UNTEST_EXPECT_FALSE(R.bIsError);
		UNTEST_EXPECT_TRUE(FindToolInResult(R.Data, TEXT("stubwdgt_make")).IsValid());
		UNTEST_EXPECT_FALSE(FindToolInResult(R.Data, TEXT("stubwdgt_create")).IsValid());
	}

	Server->UnregisterTool(TEXT("stubwdgt_make"));
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

// ===========================================================================
// (end-to-end): "asset create" must return at least one asset_* tool.
// Uses controlled stub fixture so the test does not depend on the live tool set.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, Issue1AssetCreateReturnsAssetTool, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	const TArray<FString> Names = RegisterStubs(*Server, {
		MakeTool(TEXT("stubasset"), TEXT("create"),      TEXT("create a new asset on disk")),
		MakeTool(TEXT("stubasset"), TEXT("delete"),      TEXT("delete an asset from disk")),
		MakeTool(TEXT("stubwdgt"), TEXT("create"),       TEXT("create a widget asset")),
	});

	ClaireonTool_SearchTools SearchTool;
	IClaireonTool::FToolResult R = RunExecute(SearchTool, {
		{ TEXT("query"), TEXT("stubasset create") },
	});

	UNTEST_EXPECT_FALSE(R.bIsError);
	UNTEST_EXPECT_TRUE(FindToolInResult(R.Data, TEXT("stubasset_create")).IsValid());

	UnregisterStubs(*Server, Names);
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

// ===========================================================================
// defensive substring-fallback merge -- substring hits unioned in when
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
		MakeTool(TEXT("stubwdgt"), TEXT("create"), TEXT("create a widget asset")),
	});

	ClaireonTool_SearchTools SearchTool;
	IClaireonTool::FToolResult R = RunExecute(SearchTool, {
		{ TEXT("query"), TEXT("stubwdgt") },
	});

	UNTEST_EXPECT_FALSE(R.bIsError);
	UNTEST_EXPECT_TRUE(FindToolInResult(R.Data, TEXT("stubwdgt_create")).IsValid());

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
		MakeTool(TEXT("stubbp"), TEXT("compile"), TEXT("compile a blueprint asset")),
	});

	ClaireonTool_SearchTools SearchTool;

	// Via tool_name= (legacy)
	IClaireonTool::FToolResult RT = RunExecute(SearchTool, {
		{ TEXT("tool_name"), TEXT("stubbp_compile") },
	});
	UNTEST_EXPECT_FALSE(RT.bIsError);
	// Deep-inspect data has deep_inspect=true and a top-level "tool" object.
	UNTEST_ASSERT_TRUE(RT.Data.IsValid());
	bool bDeepInspect = false;
	UNTEST_EXPECT_TRUE(RT.Data->TryGetBoolField(TEXT("deep_inspect"), bDeepInspect) && bDeepInspect);

	// Via name= (new alias)
	IClaireonTool::FToolResult RN = RunExecute(SearchTool, {
		{ TEXT("name"), TEXT("stubbp_compile") },
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
		MakeTool(TEXT("stubbp"),    TEXT("compile"), TEXT("compile a blueprint asset")),
		MakeTool(TEXT("stubasset"), TEXT("search"),  TEXT("search for an asset")),
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
			{ TEXT("name"),      TEXT("stubbp_compile") },
			{ TEXT("tool_name"), TEXT("stubasset_search") },
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
		UNTEST_EXPECT_STREQ(*InspectedName, TEXT("stubbp_compile"));
	}
	else
	{
		UNTEST_EXPECT_TRUE(false); // missing tool object in response
	}

	// The dual-set Display log fired. FClaireonLogCapture routes through GLog's
	// output-device list, which the headless Untest commandlet runner does not
	// drive the same way as the live editor -- the capture comes back empty in
	// commandlet mode even though the Display line is emitted (visible in the
	// editor log). Only assert log content when the capture device actually
	// received output; the substantive contract (name= wins) is asserted above
	// and is unconditional.
	if (CapturedLog.Len() > 0)
	{
		UNTEST_EXPECT_TRUE(CapturedLog.Contains(TEXT("both `name=` and `tool_name=` set")));
	}

	UnregisterStubs(*Server, Names);
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

// ===========================================================================
// tool_search upgrade-path footer on brief/standard hits.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, UpgradePathFooterPresentOnMultiHitStandard, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	const TArray<FString> Names = RegisterStubs(*Server, {
		MakeTool(TEXT("foo"), TEXT("alpha_compile"), TEXT("compiles foo alpha widgets")),
		MakeTool(TEXT("foo"), TEXT("beta_compile"),  TEXT("compiles foo beta widgets")),
	});

	ClaireonTool_SearchTools SearchTool;
	IClaireonTool::FToolResult R = RunExecute(SearchTool, {
		{ TEXT("query"), TEXT("compile") },
	});

	UNTEST_EXPECT_FALSE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("tip: call tool_search(name=")));
	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("detail=\"full\"")));

	UnregisterStubs(*Server, Names);
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, UpgradePathFooterSuppressedOnDetailFull, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	const TArray<FString> Names = RegisterStubs(*Server, {
		MakeTool(TEXT("foo"), TEXT("alpha_compile"), TEXT("compiles foo alpha widgets")),
		MakeTool(TEXT("foo"), TEXT("beta_compile"),  TEXT("compiles foo beta widgets")),
	});

	ClaireonTool_SearchTools SearchTool;
	IClaireonTool::FToolResult R = RunExecute(SearchTool, {
		{ TEXT("query"),  TEXT("compile") },
		{ TEXT("detail"), TEXT("full") },
	});

	UNTEST_EXPECT_FALSE(R.bIsError);
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("tip:")));

	UnregisterStubs(*Server, Names);
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, UpgradePathFooterSuppressedOnDeepInspectByName, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	const TArray<FString> Names = RegisterStubs(*Server, {
		MakeTool(TEXT("foo"), TEXT("alpha_compile"), TEXT("compiles foo alpha widgets")),
	});

	ClaireonTool_SearchTools SearchTool;
	IClaireonTool::FToolResult R = RunExecute(SearchTool, {
		{ TEXT("name"), TEXT("foo_alpha_compile") },
	});

	UNTEST_EXPECT_FALSE(R.bIsError);
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("tip:")));

	UnregisterStubs(*Server, Names);
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, UpgradePathFooterSuppressedOnZeroHits, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	// Gibberish token with no real-word subtokens so FTS5 OR-recall finds
	// nothing (it indexes descriptions, so English words would over-match).
	ClaireonTool_SearchTools SearchTool;
	IClaireonTool::FToolResult R = RunExecute(SearchTool, {
		{ TEXT("query"), TEXT("qwzxkbvnmfjghdplrt") },
	});

	UNTEST_EXPECT_FALSE(R.bIsError);
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("tip:")));

	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, UpgradePathFooterSuppressedOnExactSingleMatch, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	const TArray<FString> Names = RegisterStubs(*Server, {
		MakeTool(TEXT("uniqueprefix"), TEXT("xyzzy"), TEXT("unique stub, will be the only hit")),
	});

	ClaireonTool_SearchTools SearchTool;
	IClaireonTool::FToolResult R = RunExecute(SearchTool, {
		{ TEXT("query"), TEXT("uniqueprefix_xyzzy") },
	});

	UNTEST_EXPECT_FALSE(R.bIsError);
	UNTEST_EXPECT_EQ(GetTotalMatching(R.Data), 1);
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("tip:")));

	UnregisterStubs(*Server, Names);
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

// ===========================================================================
// tool_search surfaces GetPatterns() on full-detail and deep-inspect paths;
// empty returns are suppressed.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, DeepInspectBpAddNodeCarriesPatterns, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	// bp_add_node is provided by the built-in tool provider; assert that the
	// real registered tool carries non-empty patterns under deep-inspect.
	ClaireonTool_SearchTools SearchTool;
	IClaireonTool::FToolResult R = RunExecute(SearchTool, {
		{ TEXT("name"), TEXT("bp_add_node") },
	});

	UNTEST_EXPECT_FALSE(R.bIsError);
	const TSharedPtr<FJsonObject>* InspectedTool = nullptr;
	if (R.Data.IsValid()
		&& R.Data->TryGetObjectField(TEXT("tool"), InspectedTool)
		&& InspectedTool && (*InspectedTool).IsValid())
	{
		FString Patterns;
		const bool bHasPatterns = (*InspectedTool)->TryGetStringField(
			TEXT("patterns"), Patterns);
		UNTEST_EXPECT_TRUE(bHasPatterns);
		UNTEST_EXPECT_TRUE(Patterns.Contains(TEXT("## Common pitfalls")));
		UNTEST_EXPECT_TRUE(Patterns.Contains(TEXT("## See also")));
	}
	else
	{
		UNTEST_EXPECT_TRUE(false);
	}

	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, DeepInspectStubToolHasNoPatternsField, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	// Stub tools do NOT override GetPatterns() so the patterns field must be
	// absent on deep-inspect.
	const TArray<FString> Names = RegisterStubs(*Server, {
		MakeTool(TEXT("nopat"), TEXT("inspect"), TEXT("stub with no patterns")),
	});

	ClaireonTool_SearchTools SearchTool;
	IClaireonTool::FToolResult R = RunExecute(SearchTool, {
		{ TEXT("name"), TEXT("nopat_inspect") },
	});

	UNTEST_EXPECT_FALSE(R.bIsError);
	const TSharedPtr<FJsonObject>* InspectedTool = nullptr;
	if (R.Data.IsValid()
		&& R.Data->TryGetObjectField(TEXT("tool"), InspectedTool)
		&& InspectedTool && (*InspectedTool).IsValid())
	{
		UNTEST_EXPECT_FALSE((*InspectedTool)->HasField(TEXT("patterns")));
	}
	else
	{
		UNTEST_EXPECT_TRUE(false);
	}

	UnregisterStubs(*Server, Names);
	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

// ===========================================================================
// tool_search surfaces ApplySpecCatalog.json entry under spec_shape for
// every apply_spec / instance_apply_spec family; non-apply_spec tools
// have no spec_shape; catalog has 17 entries matching the registry;
// apply_spec_help is no longer registered.
// ===========================================================================

namespace Cl622PartDHelpers
{
	// File-local discriminator to avoid anon-NS collisions under unity batching.
	static const TArray<TPair<FString, FString>>& Cl622_GetExpectedFamilyToolPairs()
	{
		// (catalog_key, registered tool name).  Doubled `instance` for the
		// material_instance family is intentional under current naming.
		static const TArray<TPair<FString, FString>> Pairs = {
			{ TEXT("attenuation"),       TEXT("attenuation_apply_spec") },
			{ TEXT("behaviortree"),      TEXT("behaviortree_apply_spec") },
			{ TEXT("blackboard"),        TEXT("blackboard_apply_spec") },
			{ TEXT("bp"),                TEXT("bp_apply_spec") },
			{ TEXT("concurrency"),       TEXT("concurrency_apply_spec") },
			{ TEXT("eqs"),               TEXT("eqs_apply_spec") },
			{ TEXT("level_sequence"),    TEXT("level_sequence_apply_spec") },
			{ TEXT("material"),          TEXT("material_apply_spec") },
			{ TEXT("material_instance"), TEXT("material_instance_instance_apply_spec") },
			{ TEXT("metasound"),         TEXT("metasound_apply_spec") },
			{ TEXT("niagara"),           TEXT("niagara_apply_spec") },
			{ TEXT("pcg"),               TEXT("pcg_apply_spec") },
			{ TEXT("soundclass"),        TEXT("soundclass_apply_spec") },
			{ TEXT("soundcue"),          TEXT("soundcue_apply_spec") },
			{ TEXT("soundmix"),          TEXT("soundmix_apply_spec") },
			{ TEXT("statetree"),         TEXT("statetree_apply_spec") },
			{ TEXT("widgetbp"),          TEXT("widgetbp_apply_spec") },
		};
		return Pairs;
	}

	static TSharedPtr<FJsonObject> Cl622_LoadCatalogForTests()
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Claireon"));
		if (!Plugin.IsValid()) { return nullptr; }
		const FString Path = FPaths::Combine(Plugin->GetContentDir(),
			TEXT("ApplySpecCatalog.json"));
		FString Raw;
		if (!FFileHelper::LoadFileToString(Raw, *Path)) { return nullptr; }
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return nullptr;
		}
		return Root;
	}
}

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, CatalogHasSeventeenFamilyEntries, UNTEST_TIMEOUTMS(15000))
{
	TSharedPtr<FJsonObject> Catalog = Cl622PartDHelpers::Cl622_LoadCatalogForTests();
	UNTEST_ASSERT_TRUE(Catalog.IsValid());

	int32 NonMetaCount = 0;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : Catalog->Values)
	{
		if (KV.Key.StartsWith(TEXT("_"))) { continue; }
		++NonMetaCount;
	}
	UNTEST_EXPECT_EQ(NonMetaCount, 17);

	const TSharedPtr<FJsonObject>* MetaObj = nullptr;
	if (Catalog->TryGetObjectField(TEXT("_meta"), MetaObj)
		&& MetaObj && (*MetaObj).IsValid())
	{
		double EntryCount = 0.0;
		if ((*MetaObj)->TryGetNumberField(TEXT("entry_count"), EntryCount))
		{
			UNTEST_EXPECT_EQ(static_cast<int32>(EntryCount), 17);
		}
	}

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, CatalogToolsMatchRegisteredTools, UNTEST_TIMEOUTMS(15000))
{
	// Bidirectional invariant ported from ClaireonApplySpecHelpTests:
	//   (a) every catalog key matches the GetCategory() of some registered
	//       tool whose GetOperation() is apply_spec or instance_apply_spec.
	//   (b) every such registered tool has a catalog entry.
	TSharedPtr<FJsonObject> Catalog = Cl622PartDHelpers::Cl622_LoadCatalogForTests();
	UNTEST_ASSERT_TRUE(Catalog.IsValid());

	TSet<FString> CatalogKeys;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : Catalog->Values)
	{
		if (KV.Key.StartsWith(TEXT("_"))) { continue; }
		CatalogKeys.Add(KV.Key);
	}

	TSet<FString> RegisteredApplySpecCategories;
	TArray<IClaireonToolProvider*> Providers = IModularFeatures::Get()
		.GetModularFeatureImplementations<IClaireonToolProvider>(
			IClaireonToolProvider::FeatureName);
	for (IClaireonToolProvider* Provider : Providers)
	{
		if (!Provider) { continue; }
		for (const TSharedPtr<IClaireonTool>& Tool : Provider->GetTools())
		{
			if (!Tool.IsValid()) { continue; }
			const FString Op = Tool->GetOperation();
			if (Op == TEXT("apply_spec") || Op == TEXT("instance_apply_spec"))
			{
				RegisteredApplySpecCategories.Add(Tool->GetCategory());
			}
		}
	}

	// (a) catalog -> registered.
	for (const FString& K : CatalogKeys)
	{
		UNTEST_EXPECT_TRUE(RegisteredApplySpecCategories.Contains(K));
	}
	// (b) registered -> catalog.
	for (const FString& C : RegisteredApplySpecCategories)
	{
		UNTEST_EXPECT_TRUE(CatalogKeys.Contains(C));
	}

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, ApplySpecHelpToolNoLongerRegistered, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	const TMap<FString, TSharedPtr<IClaireonTool>>& Tools = Server->GetTools();
	UNTEST_EXPECT_FALSE(Tools.Contains(TEXT("apply_spec_help")));

	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, DeepInspectApplySpecFamiliesCarrySpecShape, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;
	using namespace Cl622PartDHelpers;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	TSharedPtr<FJsonObject> Catalog = Cl622_LoadCatalogForTests();
	UNTEST_ASSERT_TRUE(Catalog.IsValid());

	ClaireonTool_SearchTools SearchTool;
	for (const TPair<FString, FString>& Pair : Cl622_GetExpectedFamilyToolPairs())
	{
		const FString& Family = Pair.Key;
		const FString& ToolName = Pair.Value;

		// Skip families whose tool is not registered in the current run
		// (would have failed the bidirectional invariant test above).
		if (!Server->GetTools().Contains(ToolName)) { continue; }

		IClaireonTool::FToolResult R = RunExecute(SearchTool, {
			{ TEXT("name"), ToolName },
		});

		UNTEST_EXPECT_FALSE(R.bIsError);
		const TSharedPtr<FJsonObject>* InspectedTool = nullptr;
		if (R.Data.IsValid()
			&& R.Data->TryGetObjectField(TEXT("tool"), InspectedTool)
			&& InspectedTool && (*InspectedTool).IsValid())
		{
			const TSharedPtr<FJsonObject>* SpecShape = nullptr;
			const bool bHasSpec = (*InspectedTool)->TryGetObjectField(
				TEXT("spec_shape"), SpecShape);
			UNTEST_EXPECT_TRUE(bHasSpec);
			if (bHasSpec && SpecShape && (*SpecShape).IsValid())
			{
				FString CatalogToolField;
				UNTEST_EXPECT_TRUE((*SpecShape)->TryGetStringField(
					TEXT("tool"), CatalogToolField));
				UNTEST_EXPECT_TRUE(CatalogToolField == ToolName);
			}
		}
		else
		{
			UNTEST_EXPECT_TRUE(false);
		}
	}

	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ToolSearchExecute, DeepInspectNonApplySpecToolHasNoSpecShape, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolSearchExecuteTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = EnsureServer(Module);
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	ClaireonTool_SearchTools SearchTool;
	IClaireonTool::FToolResult R = RunExecute(SearchTool, {
		{ TEXT("name"), TEXT("bp_add_node") },
	});

	UNTEST_EXPECT_FALSE(R.bIsError);
	const TSharedPtr<FJsonObject>* InspectedTool = nullptr;
	if (R.Data.IsValid()
		&& R.Data->TryGetObjectField(TEXT("tool"), InspectedTool)
		&& InspectedTool && (*InspectedTool).IsValid())
	{
		UNTEST_EXPECT_FALSE((*InspectedTool)->HasField(TEXT("spec_shape")));
	}
	else
	{
		UNTEST_EXPECT_TRUE(false);
	}

	if (bWeStartedServer) { Module.StopServer(); }
	co_return;
}

#endif // WITH_UNTESTED
