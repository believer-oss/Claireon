// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_SearchTools.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "ClaireonBridge.h"
#include "ClaireonToolCatalogMatcher.h"
#include "ClaireonXmlFormatter.h"
#include "ClaireonLog.h"
#include "IPythonScriptPlugin.h"
#include "PythonScriptTypes.h"
#include "HAL/ThreadHeartBeat.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"
#include "Policies/CondensedJsonPrintPolicy.h"

#include <atomic>

namespace ClaireonTool_SearchToolsInternal
{

// ---------------------------------------------------------------------------
// Module-static catalog-invalidation state.
//
// The matcher is rebuilt from the live server's tool registry on demand. The
// existing `LastCatalogToolCount` heuristic only catches tool count changes
// (add/remove); rename-in-place changes leave the count constant and were
// silently mis-ranked. To close that gap we subscribe to
// FClaireonServer::OnToolsChanged and set a dirty bit; Execute() drains
// the bit before each search and rebuilds if set.
// ---------------------------------------------------------------------------
FDelegateHandle ToolCatalogChangedHandle;
FClaireonServer* LastSubscribedServer = nullptr;
std::atomic<bool> bToolCatalogDirty{false};

TArray<FString> SplitWholeWordCaseInsensitive(const FString& In, const TCHAR* Word)
{
	TArray<FString> Out;
	int32 Cursor = 0;
	const FString InLower = In.ToLower();
	const FString WordLower = FString(Word).ToLower();
	const int32 WordLen = WordLower.Len();
	while (Cursor < In.Len())
	{
		const int32 Hit = InLower.Find(WordLower, ESearchCase::CaseSensitive, ESearchDir::FromStart, Cursor);
		if (Hit == INDEX_NONE) { Out.Add(In.Mid(Cursor)); break; }
		const bool LeftBoundary = (Hit == 0) || !FChar::IsAlnum(In[Hit-1]);
		const bool RightBoundary = (Hit + WordLen >= In.Len()) || !FChar::IsAlnum(In[Hit + WordLen]);
		if (LeftBoundary && RightBoundary)
		{
			Out.Add(In.Mid(Cursor, Hit - Cursor));
			Cursor = Hit + WordLen;
		}
		else
		{
			// Not a whole-word match; skip past this hit and keep scanning.
			Cursor = Hit + WordLen;
		}
	}
	return Out;
}

}  // namespace ClaireonTool_SearchToolsInternal

FString ClaireonTool_SearchTools::GetCategory() const { return TEXT("tool"); }
FString ClaireonTool_SearchTools::GetOperation() const { return TEXT("search"); }

TArray<FString> ClaireonTool_SearchTools::GetSearchKeywords() const
{
	return {TEXT("search"), TEXT("tools"), TEXT("find"), TEXT("discover"), TEXT("lookup"), TEXT("query"), TEXT("catalog")};
}

FString ClaireonTool_SearchTools::GetDescription() const
{
	return TEXT("Search and inspect tools. Always call this before invoking a non-trivial tool through python_execute -- "
		"pass `tool_name=\"<name>\"` to get the tool's exact input schema, parameter tooltips, and example usage. "
		"Without `tool_name`, returns a list of tools matching `query` (fuzzy, per-word union ranking; understands "
		"abbreviations like 'bp' for blueprint, 'dt' for data table). Filter by `category` if you already know the area.");
}

TSharedPtr<FJsonObject> ClaireonTool_SearchTools::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// query - optional
	TSharedPtr<FJsonObject> QueryProp = MakeShared<FJsonObject>();
	QueryProp->SetStringField(TEXT("type"), TEXT("string"));
	QueryProp->SetStringField(TEXT("description"), TEXT("Search query to match against tool names and descriptions. Supports abbreviations (bp, bt, st, dt, pie, fx, etc.). Empty string returns all tools."));
	QueryProp->SetStringField(TEXT("default"), TEXT(""));
	Properties->SetObjectField(TEXT("query"), QueryProp);

	// category - optional
	TSharedPtr<FJsonObject> CategoryProp = MakeShared<FJsonObject>();
	CategoryProp->SetStringField(TEXT("type"), TEXT("string"));
	CategoryProp->SetStringField(TEXT("description"), TEXT("Filter by tool category. Empty string means no category filter."));
	CategoryProp->SetStringField(TEXT("default"), TEXT(""));
	Properties->SetObjectField(TEXT("category"), CategoryProp);

	// max_results - optional
	TSharedPtr<FJsonObject> MaxResultsProp = MakeShared<FJsonObject>();
	MaxResultsProp->SetStringField(TEXT("type"), TEXT("integer"));
	MaxResultsProp->SetStringField(TEXT("description"), TEXT("Maximum number of tools to return."));
	MaxResultsProp->SetNumberField(TEXT("default"), 100);
	Properties->SetObjectField(TEXT("max_results"), MaxResultsProp);

	// detail - optional
	TSharedPtr<FJsonObject> DetailProp = MakeShared<FJsonObject>();
	DetailProp->SetStringField(TEXT("type"), TEXT("string"));
	DetailProp->SetStringField(TEXT("description"),
		TEXT("Description detail level. 'brief' = one-line summary, "
			"'standard' = concise description (default), "
			"'full' = extended docs with examples and workflows."));
	DetailProp->SetStringField(TEXT("default"), TEXT("standard"));
	TArray<TSharedPtr<FJsonValue>> DetailEnum;
	DetailEnum.Add(MakeShared<FJsonValueString>(TEXT("brief")));
	DetailEnum.Add(MakeShared<FJsonValueString>(TEXT("standard")));
	DetailEnum.Add(MakeShared<FJsonValueString>(TEXT("full")));
	DetailProp->SetArrayField(TEXT("enum"), DetailEnum);
	Properties->SetObjectField(TEXT("detail"), DetailProp);

	// include_schema - optional
	TSharedPtr<FJsonObject> IncludeSchemaProp = MakeShared<FJsonObject>();
	IncludeSchemaProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeSchemaProp->SetStringField(TEXT("description"),
		TEXT("When true, include each tool's full input JSON Schema in the result."));
	IncludeSchemaProp->SetBoolField(TEXT("default"), false);
	Properties->SetObjectField(TEXT("include_schema"), IncludeSchemaProp);

	// include_examples - optional
	TSharedPtr<FJsonObject> IncludeExamplesProp = MakeShared<FJsonObject>();
	IncludeExamplesProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeExamplesProp->SetStringField(TEXT("description"),
		TEXT("When true, include each tool's GetExampleUsage() string in the result."));
	IncludeExamplesProp->SetBoolField(TEXT("default"), false);
	Properties->SetObjectField(TEXT("include_examples"), IncludeExamplesProp);

	// name - optional deep-inspect bypass (preferred alias of tool_name)
	TSharedPtr<FJsonObject> NameProp = MakeShared<FJsonObject>();
	NameProp->SetStringField(TEXT("type"), TEXT("string"));
	NameProp->SetStringField(TEXT("description"),
		TEXT("RECOMMENDED PRE-FLIGHT before any python_execute call. When provided, returns the exact input schema, "
			"parameter tooltips, example usage, and full description for that tool name (bypasses search ranking). "
			"Forces include_schema=true and include_examples=true for the matched tool. "
			"Pass the bare tool name (e.g. 'blueprint_compile'); this is the same name used in `claireon.<name>(...)` "
			"from inside python_execute."));
	NameProp->SetStringField(TEXT("default"), TEXT(""));
	Properties->SetObjectField(TEXT("name"), NameProp);

	// tool_name - optional deep-inspect bypass (deprecated alias of name=)
	TSharedPtr<FJsonObject> ToolNameProp = MakeShared<FJsonObject>();
	ToolNameProp->SetStringField(TEXT("type"), TEXT("string"));
	ToolNameProp->SetStringField(TEXT("description"),
		TEXT("Deprecated alias for `name=`; will be removed in a follow-up. "
			"RECOMMENDED PRE-FLIGHT before any python_execute call. When provided, returns the exact input schema, "
			"parameter tooltips, example usage, and full description for that tool name (bypasses search ranking). "
			"Forces include_schema=true and include_examples=true for the matched tool. "
			"Pass the bare tool name (e.g. 'blueprint_compile'); this is the same name used in `claireon.<name>(...)` "
			"from inside python_execute."));
	ToolNameProp->SetStringField(TEXT("default"), TEXT(""));
	Properties->SetObjectField(TEXT("tool_name"), ToolNameProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

// ---------------------------------------------------------------------------
// Python catalog integration
// ---------------------------------------------------------------------------

bool ClaireonTool_SearchTools::RebuildCatalog()
{
	FClaireonServer* Server = FClaireonModule::Get().GetServer();
	if (!Server)
	{
		return false;
	}

	// (Re-)subscribe to OnToolsChanged on the currently running server so
	// rename-in-place changes flip the dirty bit. Re-subscribes across server
	// restarts when the pointer changes.
	if (Server != ClaireonTool_SearchToolsInternal::LastSubscribedServer)
	{
		if (ClaireonTool_SearchToolsInternal::ToolCatalogChangedHandle.IsValid() && ClaireonTool_SearchToolsInternal::LastSubscribedServer != nullptr)
		{
			ClaireonTool_SearchToolsInternal::LastSubscribedServer->OnToolsChanged.Remove(ClaireonTool_SearchToolsInternal::ToolCatalogChangedHandle);
		}
		ClaireonTool_SearchToolsInternal::ToolCatalogChangedHandle = Server->OnToolsChanged.AddLambda([]()
		{
			ClaireonTool_SearchToolsInternal::bToolCatalogDirty.store(true);
		});
		ClaireonTool_SearchToolsInternal::LastSubscribedServer = Server;
	}

	FClaireonBridge::EnsureRegistered();

	const TMap<FString, TSharedPtr<IClaireonTool>>& ToolsMap = Server->GetTools();

	// Serialize tool metadata to JSON array
	TArray<TSharedPtr<FJsonValue>> ToolsArray;
	for (const auto& Pair : ToolsMap)
	{
		const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
		const FString ToolName = Tool->GetName();

		// Skip meta tools
		if (ToolName == TEXT("python_execute") || ToolName == TEXT("tool_search"))
		{
			continue;
		}

		TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
		ToolObj->SetStringField(TEXT("name"), ToolName);
		ToolObj->SetStringField(TEXT("description"), Tool->GetDescription());
		ToolObj->SetStringField(TEXT("category"), Tool->GetCategory());
		ToolObj->SetStringField(TEXT("operation"), Tool->GetOperation());

		// Include search keywords so the Python catalog can rank them alongside name/description.
		const TArray<FString> Keywords = Tool->GetSearchKeywords();
		if (Keywords.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> KeywordValues;
			KeywordValues.Reserve(Keywords.Num());
			for (const FString& Keyword : Keywords)
			{
				KeywordValues.Add(MakeShared<FJsonValueString>(Keyword));
			}
			ToolObj->SetArrayField(TEXT("keywords"), KeywordValues);
		}

		ToolsArray.Add(MakeShared<FJsonValueObject>(ToolObj));
	}

	FString ToolsJson;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ToolsJson);
	FJsonSerializer::Serialize(ToolsArray, Writer);
	Writer->Close();

	// Use raw string with triple quotes to safely embed JSON containing quotes
	FString PythonCode = FString::Printf(
		TEXT("import json as _json\n")
		TEXT("from mcp_tool_catalog import rebuild as _rebuild\n")
		TEXT("_result = _rebuild(r\"\"\"%s\"\"\")\n")
		TEXT("print(_json.dumps(_result))\n"),
		*ToolsJson
	);

	FPythonCommandEx PythonCommand;
	PythonCommand.Command = PythonCode;
	PythonCommand.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
	PythonCommand.FileExecutionScope = EPythonFileExecutionScope::Private;

	FSlowHeartBeatScope SuspendHeartBeat;
	FDisableHitchDetectorScope SuspendHitchDetector;

	bool bSuccess = IPythonScriptPlugin::Get()->ExecPythonCommandEx(PythonCommand);

	if (bSuccess)
	{
		// Extract the printed JSON result from log output
		FString ResultJson;
		for (const FPythonLogOutputEntry& Entry : PythonCommand.LogOutput)
		{
			if (Entry.Type == EPythonLogOutputType::Info || Entry.Type == EPythonLogOutputType::Warning)
			{
				ResultJson = Entry.Output;
			}
		}
		if (!ResultJson.IsEmpty())
		{
			LastCatalogToolCount = ToolsMap.Num();
			UE_LOG(LogClaireon, Display, TEXT("[MCP] Tool catalog rebuilt (%d tools)"), ToolsArray.Num());
			return true;
		}
	}

	UE_LOG(LogClaireon, Warning, TEXT("[MCP] Failed to rebuild tool catalog via Python"));
	return false;
}

TArray<FString> ClaireonTool_SearchTools::FuzzySearch(const FString& Query, int32 MaxResults, TMap<FString, int32>& OutTokensMatchedByName)
{
	TArray<FString> Results;
	OutTokensMatchedByName.Reset();

	FClaireonBridge::EnsureRegistered();

	// Use raw string with triple quotes to safely embed query containing quotes
	FString PythonCode = FString::Printf(
		TEXT("import json as _json\n")
		TEXT("from mcp_tool_catalog import search as _search\n")
		TEXT("_hits = _search(r\"\"\"%s\"\"\", max_results=%d)\n")
		TEXT("print(_json.dumps(_hits))\n"),
		*Query, MaxResults
	);

	FPythonCommandEx PythonCommand;
	PythonCommand.Command = PythonCode;
	PythonCommand.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
	PythonCommand.FileExecutionScope = EPythonFileExecutionScope::Private;

	FSlowHeartBeatScope SuspendHeartBeat;
	FDisableHitchDetectorScope SuspendHitchDetector;

	bool bSuccess = IPythonScriptPlugin::Get()->ExecPythonCommandEx(PythonCommand);
	if (!bSuccess)
	{
		return Results;
	}

	// Extract the printed JSON result from log output
	FString ResultJson;
	for (const FPythonLogOutputEntry& Entry : PythonCommand.LogOutput)
	{
		if (Entry.Type == EPythonLogOutputType::Info || Entry.Type == EPythonLogOutputType::Warning)
		{
			ResultJson = Entry.Output;
		}
	}
	if (ResultJson.IsEmpty())
	{
		return Results;
	}

	// Parse the results array
	TArray<TSharedPtr<FJsonValue>> HitsArray;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
	if (!FJsonSerializer::Deserialize(Reader, HitsArray))
	{
		return Results;
	}

	for (const TSharedPtr<FJsonValue>& HitVal : HitsArray)
	{
		const TSharedPtr<FJsonObject>* HitObj = nullptr;
		if (HitVal->TryGetObject(HitObj) && (*HitObj).IsValid())
		{
			FString ToolName;
			if ((*HitObj)->TryGetStringField(TEXT("tool_name"), ToolName) && !ToolName.IsEmpty())
			{
				Results.Add(ToolName);
				// Capture the per-entry distinct-query-tokens-matched count so
				// Execute() can surface `query_tokens_matched` per result and
				// decide whether to merge in substring-fallback hits.
				int32 TokensMatched = 0;
				double TokensMatchedVal = 0.0;
				if ((*HitObj)->TryGetNumberField(TEXT("tokens_matched"), TokensMatchedVal))
				{
					TokensMatched = static_cast<int32>(TokensMatchedVal);
				}
				OutTokensMatchedByName.Add(ToolName, TokensMatched);
			}
		}
	}

	return Results;
}

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------

IClaireonTool::FToolResult ClaireonTool_SearchTools::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FClaireonServer* Server = FClaireonModule::Get().GetServer();
	if (!Server)
	{
		return MakeErrorResult(TEXT("MCP server is not running"));
	}

	// Read parameters
	FString Query;
	FString Category;
	FString Detail = TEXT("standard");
	FString InspectToolName;
	int32 MaxResults = 100;
	bool bIncludeSchema = false;
	bool bIncludeExamples = false;

	if (Arguments.IsValid())
	{
		Arguments->TryGetStringField(TEXT("query"), Query);
		Arguments->TryGetStringField(TEXT("category"), Category);
		Arguments->TryGetStringField(TEXT("detail"), Detail);

		// `name=` is the preferred alias; `tool_name=` is a deprecated alias
		// kept for backwards compatibility. Read `name=` first; fall back to
		// `tool_name=` only when `name=` is empty. When both are set to
		// different values, log a one-line Display warning and use `name=`.
		Arguments->TryGetStringField(TEXT("name"), InspectToolName);
		FString ToolNameField;
		Arguments->TryGetStringField(TEXT("tool_name"), ToolNameField);
		if (InspectToolName.IsEmpty())
		{
			InspectToolName = ToolNameField;
		}
		else if (!ToolNameField.IsEmpty()
			&& !InspectToolName.Equals(ToolNameField, ESearchCase::CaseSensitive))
		{
			UE_LOG(LogClaireon, Display,
				TEXT("Deep-inspect: both `name=` and `tool_name=` set; using `name=` value."));
		}

		Arguments->TryGetBoolField(TEXT("include_schema"), bIncludeSchema);
		Arguments->TryGetBoolField(TEXT("include_examples"), bIncludeExamples);

		double MaxResultsVal = 100;
		if (Arguments->TryGetNumberField(TEXT("max_results"), MaxResultsVal))
		{
			MaxResults = static_cast<int32>(MaxResultsVal);
			if (MaxResults <= 0) MaxResults = 100;
		}
	}

	Detail = Detail.TrimStartAndEnd().ToLower();
	if (Detail != TEXT("brief") && Detail != TEXT("full"))
	{
		Detail = TEXT("standard");
	}

	Query = Query.TrimStartAndEnd();
	Category = Category.TrimStartAndEnd();
	InspectToolName = InspectToolName.TrimStartAndEnd();

	// Strip boolean operators and quote/paren punctuation so callers that write
	// pseudo-boolean queries ("blueprint AND chooser") still benefit from per-word
	// union ranking. The C++ matcher's tokeniser drops <2-char tokens, so any
	// residual single chars are harmless.
	{
		static const TCHAR* const BoolOps[] = { TEXT("AND"), TEXT("OR"), TEXT("NOT") };
		for (const TCHAR* Op : BoolOps)
		{
			Query = FString::Join(ClaireonTool_SearchToolsInternal::SplitWholeWordCaseInsensitive(Query, Op), TEXT(" "));
		}
		Query = Query.Replace(TEXT("("), TEXT(" "), ESearchCase::CaseSensitive);
		Query = Query.Replace(TEXT(")"), TEXT(" "), ESearchCase::CaseSensitive);
		Query = Query.Replace(TEXT("\""), TEXT(" "), ESearchCase::CaseSensitive);
	}

	const TMap<FString, TSharedPtr<IClaireonTool>>& ToolsMap = Server->GetTools();

	// --- Deep-inspect bypass: when tool_name is provided, return that tool's full metadata. ---
	if (!InspectToolName.IsEmpty())
	{
		const TSharedPtr<IClaireonTool>* FoundTool = ToolsMap.Find(InspectToolName);
		if (!FoundTool || !FoundTool->IsValid())
		{
			return MakeErrorResult(FString::Printf(TEXT("Tool not found: %s"), *InspectToolName));
		}
		const TSharedPtr<IClaireonTool>& Tool = *FoundTool;

		TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
		ToolObj->SetStringField(TEXT("name"), Tool->GetName());
		ToolObj->SetStringField(TEXT("category"), Tool->GetCategory());
		ToolObj->SetStringField(TEXT("description"), Tool->GetDescription());
		ToolObj->SetStringField(TEXT("brief_description"), Tool->GetBriefDescription());
		ToolObj->SetStringField(TEXT("full_description"), Tool->GetFullDescription());
		ToolObj->SetStringField(TEXT("signature"),
			FClaireonXmlFormatter::GenerateTypeSignature(Tool->GetName(), Tool->GetInputSchema()));

		// Deep inspect always includes schema + example regardless of caller flags.
		if (TSharedPtr<FJsonObject> InputSchema = Tool->GetInputSchema())
		{
			ToolObj->SetObjectField(TEXT("input_schema"), InputSchema);
		}
		const FString Example = Tool->GetExampleUsage();
		if (!Example.IsEmpty())
		{
			ToolObj->SetStringField(TEXT("example_usage"), Example);
		}
		if (TSharedPtr<FJsonObject> Tooltips = Tool->GetParameterTooltips())
		{
			ToolObj->SetObjectField(TEXT("parameter_tooltips"), Tooltips);
		}
		const TArray<FString> Keywords = Tool->GetSearchKeywords();
		if (Keywords.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> KeywordValues;
			KeywordValues.Reserve(Keywords.Num());
			for (const FString& Keyword : Keywords)
			{
				KeywordValues.Add(MakeShared<FJsonValueString>(Keyword));
			}
			ToolObj->SetArrayField(TEXT("keywords"), KeywordValues);
		}

		const TMap<FString, FName>& SourceMap = Server->GetToolSourceMap();
		if (const FName* SourceName = SourceMap.Find(InspectToolName))
		{
			ToolObj->SetStringField(TEXT("source"), SourceName->ToString());
		}

		TSharedPtr<FJsonObject> InspectData = MakeShared<FJsonObject>();
		InspectData->SetBoolField(TEXT("deep_inspect"), true);
		InspectData->SetObjectField(TEXT("tool"), ToolObj);

		const FString InspectSummary = FString::Printf(TEXT("Deep inspect: %s"), *InspectToolName);
		return MakeSuccessResult(InspectData, InspectSummary);
	}

	// ---------------------------------------------------------------------
	// Exact-name + near-exact-name precedence pass.
	//
	// Runs ALWAYS before fuzzy/category filtering. Hyphen and underscore are
	// equivalent, comparison is case-insensitive, and whitespace is trimmed.
	// Exact (distance 0) bypasses the category filter unconditionally;
	// near-exact (distance 1-2) RESPECTS category.
	// ---------------------------------------------------------------------
	auto NormaliseForExactName = [](const FString& In) -> FString
	{
		FString S = In.TrimStartAndEnd().ToLower();
		S.ReplaceInline(TEXT("-"), TEXT("_"));
		return S;
	};

	const FString QueryExactKey = NormaliseForExactName(Query);
	const bool bHasCategoryFilter = !Category.IsEmpty();
	TSharedPtr<IClaireonTool> ExactNameTool;
	FString                 ExactNameToolKey;     // lex-first key when collisions exist.
	TSharedPtr<IClaireonTool> NearExactNameTool;
	int32                   NearExactDistance = INT32_MAX;
	FString                 NearExactToolKey;

	if (!QueryExactKey.IsEmpty())
	{
		for (const auto& Pair : ToolsMap)
		{
			const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
			if (!Tool.IsValid())
			{
				continue;
			}
			const FString ToolName = Tool->GetName();
			// Skip meta tools (consistent with the candidate-collection loop below).
			if (ToolName == TEXT("python_execute") || ToolName == TEXT("tool_search"))
			{
				continue;
			}

			const FString ToolKey = NormaliseForExactName(ToolName);
			if (ToolKey == QueryExactKey)
			{
				if (!ExactNameTool.IsValid())
				{
					ExactNameTool = Tool;
					ExactNameToolKey = ToolName;
				}
				else
				{
					// Multiple tools normalise to the same exact-name key:
					// keep the lexicographically-first name and warn.
					const FString& IncumbentKey = ExactNameToolKey;
					const FString& ChallengerKey = ToolName;
					const FString& Winner = (ChallengerKey < IncumbentKey) ? ChallengerKey : IncumbentKey;
					UE_LOG(LogClaireon, Warning,
						TEXT("Tool registry name collision under hyphen-underscore normalisation: '%s' vs '%s' both normalise to '%s'; selecting '%s' for exact-name precedence."),
						*IncumbentKey, *ChallengerKey, *ToolKey, *Winner);
					if (&Winner == &ChallengerKey)
					{
						ExactNameTool = Tool;
						ExactNameToolKey = ToolName;
					}
				}
				continue;
			}

			// Near-exact RESPECTS the category filter.
			if (bHasCategoryFilter && !Tool->GetCategory().Equals(Category, ESearchCase::IgnoreCase))
			{
				continue;
			}
			const int32 Dist = FClaireonToolCatalogMatcher::DistanceBounded(ToolKey, QueryExactKey, 2);
			if (Dist <= 2)
			{
				if (Dist < NearExactDistance
					|| (Dist == NearExactDistance && ToolName < NearExactToolKey))
				{
					NearExactDistance = Dist;
					NearExactNameTool = Tool;
					NearExactToolKey = ToolName;
				}
			}
		}
	}

	// Determine the pinned tool: exact wins over near-exact.
	TSharedPtr<IClaireonTool> PinnedTool;
	if (ExactNameTool.IsValid())
	{
		PinnedTool = ExactNameTool;
	}
	else if (NearExactNameTool.IsValid() && NearExactDistance <= 2)
	{
		PinnedTool = NearExactNameTool;
	}
	const FString PinnedName = PinnedTool.IsValid() ? PinnedTool->GetName() : FString();

	// Determine which tools to include via fuzzy search or full catalog
	TArray<FString> FuzzyRankedNames;  // ordered by relevance
	TSet<FString> FuzzyMatchedSet;
	TMap<FString, int32> FuzzyTokensMatchedByName;
	bool bUsedFuzzySearch = false;

	if (!Query.IsEmpty())
	{
		// Ensure catalog is built (or rebuilt if tool count changed OR the
		// OnToolsChanged dirty bit was set by a rename-in-place / unregister
		// path that did not change the count). exchange(false) atomically drains
		// the bit so a concurrent broadcast queues the next rebuild instead of
		// racing the current one.
		const bool bCountChanged = (LastCatalogToolCount != ToolsMap.Num());
		const bool bDirtyDrained = ClaireonTool_SearchToolsInternal::bToolCatalogDirty.exchange(false);
		if (bCountChanged || bDirtyDrained)
		{
			RebuildCatalog();
		}

		// Try fuzzy search via the C++ nearest-string catalog matcher (bridged through Python)
		FuzzyRankedNames = FuzzySearch(Query, MaxResults, FuzzyTokensMatchedByName);
		if (FuzzyRankedNames.Num() > 0)
		{
			bUsedFuzzySearch = true;
			for (const FString& Name : FuzzyRankedNames)
			{
				FuzzyMatchedSet.Add(Name);
			}
		}
		// If fuzzy search returned nothing (Python unavailable or no matches),
		// fall back to substring matching below
	}

	// -----------------------------------------------------------------------
	// Defensive substring-fallback merge.
	//
	// When the fuzzy ranker returned results but none of those names contain
	// any query word as a substring, the ranking is "pathological" -- typically
	// abbreviation expansion has wandered the result set into tools whose
	// names have no visible relationship to the operator's terms. In that
	// case, ALSO run the substring-fallback collection path below and union
	// its hits into the candidate set (the fuzzy hits stay; substring hits
	// are added). Merged hits respect the category filter; only the
	// exact-name pin bypasses category.
	// -----------------------------------------------------------------------
	bool bFuzzyResultsLookPathological = false;
	if (bUsedFuzzySearch && !Query.IsEmpty())
	{
		TArray<FString> QueryWordsForCheck;
		Query.ToLower().ParseIntoArray(QueryWordsForCheck, TEXT(" "), true);
		bool bAnyNameMatch = false;
		for (const FString& MatchedName : FuzzyMatchedSet)
		{
			const FString LowerName = MatchedName.ToLower();
			for (const FString& Word : QueryWordsForCheck)
			{
				if (LowerName.Contains(Word)) { bAnyNameMatch = true; break; }
			}
			if (bAnyNameMatch) break;
		}
		bFuzzyResultsLookPathological = !bAnyNameMatch;
	}

	// Collect matching tools
	struct FMatchEntry
	{
		FString Name;
		FString Description;
		FString Category;
		FString TypeSignature;
		FString Source;
		TSharedPtr<FJsonObject> InputSchema;
		FString ExampleUsage;
		/** Distinct query tokens that matched this entry (surfaced as `query_tokens_matched`). */
		int32 QueryTokensMatched = 0;
	};
	TArray<FMatchEntry> MatchingTools;
	TMap<FString, int32> MatchCountByName;

	// Pre-compute the count of query tokens that survive the >2-char cutoff;
	// used as the `query_tokens_matched` value for the exact-name / near-exact-name
	// pinned entry.
	int32 PinnedQueryTokenCount = 0;
	{
		TArray<FString> AllQueryWords;
		Query.ToLower().ParseIntoArray(AllQueryWords, TEXT(" "), true);
		int32 LongCount = 0;
		for (const FString& W : AllQueryWords)
		{
			if (W.Len() > 2) { ++LongCount; }
		}
		PinnedQueryTokenCount = (LongCount > 0) ? LongCount : AllQueryWords.Num();
	}

	const FString QueryLower = Query.ToLower();
	const FString CategoryLower = Category.ToLower();

	for (const auto& Pair : ToolsMap)
	{
		const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
		const FString ToolName = Tool->GetName();

		// Skip meta tools
		if (ToolName == TEXT("python_execute") || ToolName == TEXT("tool_search"))
		{
			continue;
		}

		const FString ToolCategory = Tool->GetCategory();
		const bool bIsPinned = (!PinnedName.IsEmpty() && ToolName == PinnedName);
		const bool bIsExactPin = bIsPinned && ExactNameTool.IsValid();

		// Apply category filter -- exact-name pin (distance 0) bypasses it.
		// Near-exact pins still respect category (already checked above), so
		// they don't need a bypass here; we let the already-validated
		// near-exact pin through too for symmetry.
		if (!CategoryLower.IsEmpty() && ToolCategory.ToLower() != CategoryLower && !bIsExactPin)
		{
			continue;
		}

		// Apply query filter -- the pinned tool ALWAYS surfaces irrespective of
		// the fuzzy/substring outcomes (the pin is the whole point).
		//
		// Substring-fallback merge: when fuzzy results look pathological, we
		// ALSO accept tools that satisfy the substring-fallback predicate, in
		// addition to fuzzy-set members. The merge expands the candidate set
		// without dropping any fuzzy hits.
		int32 SubstringMatchCount = 0;
		bool bIncludedByQueryFilter = bIsPinned || QueryLower.IsEmpty();
		if (!QueryLower.IsEmpty() && !bIsPinned)
		{
			const bool bRunFuzzyMembership = bUsedFuzzySearch;
			const bool bRunSubstringFallback = !bUsedFuzzySearch || bFuzzyResultsLookPathological;

			bool bFuzzyHit = false;
			if (bRunFuzzyMembership && FuzzyMatchedSet.Contains(ToolName))
			{
				bFuzzyHit = true;
			}

			bool bSubstringHit = false;
			if (bRunSubstringFallback)
			{
				// Substring fallback (UNION semantics; rank by per-tool match count).
				// A tool is included if at least one query word substring-matches name OR description.
				TArray<FString> QueryWords;
				QueryLower.ParseIntoArray(QueryWords, TEXT(" "), true);

				const FString NameLower = ToolName.ToLower();
				const FString DescLower = Tool->GetDescription().ToLower();

				for (const FString& Word : QueryWords)
				{
					if (NameLower.Contains(Word) || DescLower.Contains(Word))
					{
						++SubstringMatchCount;
					}
				}
				if (SubstringMatchCount > 0)
				{
					bSubstringHit = true;
				}
			}

			if (!bFuzzyHit && !bSubstringHit)
			{
				continue;
			}

			if (bSubstringHit && SubstringMatchCount > 0)
			{
				// Stash MatchCount on the entry so the fallback-sort branch
				// (and the merged-branch ranking) can rank by it.
				MatchCountByName.Add(ToolName, SubstringMatchCount);
			}

			bIncludedByQueryFilter = true;
		}
		(void)bIncludedByQueryFilter;

		// Select description tier
		FString ToolDescription;
		if (Detail == TEXT("brief"))
		{
			ToolDescription = Tool->GetBriefDescription();
		}
		else if (Detail == TEXT("full"))
		{
			ToolDescription = Tool->GetFullDescription();
		}
		else
		{
			ToolDescription = Tool->GetDescription();
		}

		FMatchEntry Entry;
		Entry.Name = ToolName;
		Entry.Description = ToolDescription;
		Entry.Category = ToolCategory;
		TSharedPtr<FJsonObject> ToolSchema = Tool->GetInputSchema();
		Entry.TypeSignature = FClaireonXmlFormatter::GenerateTypeSignature(ToolName, ToolSchema);

		if (bIncludeSchema)
		{
			Entry.InputSchema = ToolSchema;
		}
		if (bIncludeExamples)
		{
			Entry.ExampleUsage = Tool->GetExampleUsage();
		}

		// Look up tool source from the server's source map
		const TMap<FString, FName>& SourceMap = Server->GetToolSourceMap();
		if (const FName* SourceName = SourceMap.Find(ToolName))
		{
			Entry.Source = SourceName->ToString();
		}

		// query_tokens_matched: prefer the matcher-reported count when this
		// entry came from the fuzzy ranker; fall back to the substring-fallback
		// per-tool count when fuzzy did not contribute. Pinned entries use
		// the count of query tokens that survived the >2-char cutoff.
		if (bIsPinned)
		{
			Entry.QueryTokensMatched = PinnedQueryTokenCount;
		}
		else if (const int32* FuzzyTokens = FuzzyTokensMatchedByName.Find(ToolName))
		{
			Entry.QueryTokensMatched = *FuzzyTokens;
		}
		else
		{
			Entry.QueryTokensMatched = SubstringMatchCount;
		}

		MatchingTools.Add(MoveTemp(Entry));
	}

	// Sort: fuzzy-ranked results preserve relevance order; fallback sorts by category/name.
	// In both branches, a pinned exact/near-exact name short-circuits to index 0.
	if (bUsedFuzzySearch)
	{
		// Build rank map from the cached fuzzy results order
		TMap<FString, int32> RankMap;
		for (int32 i = 0; i < FuzzyRankedNames.Num(); ++i)
		{
			RankMap.Add(FuzzyRankedNames[i], i);
		}
		MatchingTools.Sort([&RankMap, &PinnedName](const FMatchEntry& A, const FMatchEntry& B)
		{
			if (!PinnedName.IsEmpty())
			{
				if (A.Name == PinnedName) return true;
				if (B.Name == PinnedName) return false;
			}
			const int32* RankA = RankMap.Find(A.Name);
			const int32* RankB = RankMap.Find(B.Name);
			int32 RA = RankA ? *RankA : INT32_MAX;
			int32 RB = RankB ? *RankB : INT32_MAX;
			return RA < RB;
		});
	}
	else
	{
		MatchingTools.Sort([&MatchCountByName, &PinnedName](const FMatchEntry& A, const FMatchEntry& B)
		{
			if (!PinnedName.IsEmpty())
			{
				if (A.Name == PinnedName) return true;
				if (B.Name == PinnedName) return false;
			}
			const int32* CountAPtr = MatchCountByName.Find(A.Name);
			const int32* CountBPtr = MatchCountByName.Find(B.Name);
			const int32 CountA = CountAPtr ? *CountAPtr : 0;
			const int32 CountB = CountBPtr ? *CountBPtr : 0;
			if (CountA != CountB)
			{
				return CountA > CountB;
			}
			if (A.Category != B.Category)
			{
				return A.Category < B.Category;
			}
			return A.Name < B.Name;
		});
	}

	// Truncate after sorting
	const int32 TotalMatching = MatchingTools.Num();
	if (MatchingTools.Num() > MaxResults)
	{
		MatchingTools.SetNum(MaxResults);
	}

	// Build structured Data as JSON
	TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
	if (!Query.IsEmpty())
	{
		DataObj->SetStringField(TEXT("query"), Query);
	}
	DataObj->SetNumberField(TEXT("total_matching"), TotalMatching);
	DataObj->SetNumberField(TEXT("returned"), MatchingTools.Num());

	// Group by category for the JSON output
	TMap<FString, TArray<const FMatchEntry*>> Grouped;
	for (const FMatchEntry& Entry : MatchingTools)
	{
		Grouped.FindOrAdd(Entry.Category).Add(&Entry);
	}

	TArray<TSharedPtr<FJsonValue>> CategoriesArr;
	TArray<FString> CategoryNames;
	Grouped.GetKeys(CategoryNames);
	CategoryNames.Sort();

	for (const FString& CatName : CategoryNames)
	{
		TSharedPtr<FJsonObject> CatObj = MakeShared<FJsonObject>();
		CatObj->SetStringField(TEXT("name"), CatName);

		TArray<TSharedPtr<FJsonValue>> ToolsArr;
		for (const FMatchEntry* Entry : Grouped[CatName])
		{
			TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
			ToolObj->SetStringField(TEXT("name"), Entry->Name);
			ToolObj->SetStringField(TEXT("description"), Entry->Description);
			ToolObj->SetStringField(TEXT("signature"), Entry->TypeSignature);
			// Always emit `query_tokens_matched` so future zero-result regressions
			// are diagnosable.
			ToolObj->SetNumberField(TEXT("query_tokens_matched"), Entry->QueryTokensMatched);
			if (!Entry->Source.IsEmpty())
			{
				ToolObj->SetStringField(TEXT("source"), Entry->Source);
			}
			if (Entry->InputSchema.IsValid())
			{
				ToolObj->SetObjectField(TEXT("input_schema"), Entry->InputSchema);
			}
			if (!Entry->ExampleUsage.IsEmpty())
			{
				ToolObj->SetStringField(TEXT("example_usage"), Entry->ExampleUsage);
			}
			ToolsArr.Add(MakeShared<FJsonValueObject>(ToolObj));
		}
		CatObj->SetArrayField(TEXT("tools"), ToolsArr);
		CategoriesArr.Add(MakeShared<FJsonValueObject>(CatObj));
	}
	DataObj->SetArrayField(TEXT("categories"), CategoriesArr);

	// Build plain text summary
	FString SummaryText;
	if (Query.IsEmpty())
	{
		SummaryText = FString::Printf(TEXT("Found %d tool(s) across %d categories"), TotalMatching, CategoryNames.Num());
	}
	else
	{
		SummaryText = FString::Printf(TEXT("Found %d tool(s) matching '%s'"), TotalMatching, *Query);
		if (bUsedFuzzySearch)
		{
			SummaryText += TEXT(" (fuzzy)");
		}
	}
	if (TotalMatching > MatchingTools.Num())
	{
		SummaryText += FString::Printf(TEXT(" (showing %d of %d)"), MatchingTools.Num(), TotalMatching);
	}

	return MakeSuccessResult(DataObj, SummaryText);
}
