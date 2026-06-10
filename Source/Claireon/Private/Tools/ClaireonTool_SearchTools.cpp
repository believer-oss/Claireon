// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_SearchTools.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "ClaireonBridge.h"
#include "ClaireonToolSearchIndex.h"
#include "ClaireonToolEmbeddingIndex.h"
#include "ClaireonXmlFormatter.h"
#include "ClaireonLog.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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
// the bit before each search and rebuilds if set. The pattern mirrors
// FClaireonBridge's `bClaireonModuleStale` (see ClaireonBridge.cpp:127-133).
// ---------------------------------------------------------------------------
FDelegateHandle ToolCatalogChangedHandle;
FClaireonServer* LastSubscribedServer = nullptr;
std::atomic<bool> bToolCatalogDirty{false};

// ---------------------------------------------------------------------------
// apply_spec catalog loader.
//
// Loads `ApplySpecCatalog.json` on demand, caches by file mtime so per-search
// IO is avoided in the common case.  Returns the parsed root JSON object, or
// null on any failure (missing plugin, missing file, parse error).  Failures
// are logged at Warning level for ops visibility but never surface to the
// agent -- the deep-inspect branch just omits the `spec_shape` field.
//
// File-local discriminator to avoid anon-NS collisions under unity batching.
// ---------------------------------------------------------------------------
TSharedPtr<FJsonObject> Cl622SearchSpec_LoadCatalog()
{
	static FCriticalSection CacheLock;
	static TSharedPtr<FJsonObject> CachedCatalog;
	static FDateTime CachedMTime = FDateTime::MinValue();
	static FString CachedPath;

	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Claireon"));
	if (!Plugin.IsValid())
	{
		return nullptr;
	}
	const FString Path = FPaths::Combine(Plugin->GetContentDir(), TEXT("ApplySpecCatalog.json"));
	if (!FPaths::FileExists(Path))
	{
		return nullptr;
	}

	const FDateTime MTime = IFileManager::Get().GetTimeStamp(*Path);

	FScopeLock Lock(&CacheLock);
	if (CachedCatalog.IsValid() && CachedPath == Path && CachedMTime == MTime)
	{
		return CachedCatalog;
	}

	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *Path))
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[tool_search] Failed to read ApplySpecCatalog.json at %s"), *Path);
		return nullptr;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[tool_search] Failed to parse ApplySpecCatalog.json: %s"),
			*Reader->GetErrorMessage());
		return nullptr;
	}

	CachedCatalog = Root;
	CachedMTime = MTime;
	CachedPath = Path;
	return CachedCatalog;
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
		"Without `tool_name`, returns a flat, globally rank-ordered `tools[]` list matching `query` (hybrid lexical "
		"bm25 + semantic fused via reciprocal rank fusion, with abbreviation expansion -- understands 'bp' for "
		"blueprint, 'dt' for data table). Results are best-first. Filter by `category` if you "
		"already know the area. TRANSPORT: all claireon.* tools listed here are invoked via mcp__claireon__python_execute: "
		"`import claireon; result = claireon.<tool_name>(arg=value)`.");
}

TSharedPtr<FJsonObject> ClaireonTool_SearchTools::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// query - optional
	TSharedPtr<FJsonObject> QueryProp = MakeShared<FJsonObject>();
	QueryProp->SetStringField(TEXT("type"), TEXT("string"));
	QueryProp->SetStringField(TEXT("description"), TEXT("Search query to match against tool names and descriptions. Supports abbreviations (bp, bt, st, dt, pie, fx, etc.). Empty string returns all tools. Special form: 'select:nameA,nameB' returns full schema records for the named tools (deferred-tool selector)."));
	QueryProp->SetStringField(TEXT("default"), TEXT(""));
	Properties->SetObjectField(TEXT("query"), QueryProp);

	// mode - optional
	TSharedPtr<FJsonObject> ModeProp = MakeShared<FJsonObject>();
	ModeProp->SetStringField(TEXT("type"), TEXT("string"));
	ModeProp->SetStringField(TEXT("description"),
		TEXT("Discovery mode. 'categories' returns just category names + tool counts (cheap discovery, ~200 bytes). Default (omitted) runs the full search/inspect pipeline."));
	TArray<TSharedPtr<FJsonValue>> ModeEnum;
	ModeEnum.Add(MakeShared<FJsonValueString>(TEXT("categories")));
	ModeProp->SetArrayField(TEXT("enum"), ModeEnum);
	Properties->SetObjectField(TEXT("mode"), ModeProp);

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
	MaxResultsProp->SetNumberField(TEXT("default"), 10);
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

bool ClaireonTool_SearchTools::RebuildSearchIndex()
{
	FClaireonServer* Server = FClaireonModule::Get().GetServer();
	if (!Server)
	{
		return false;
	}

	// (Re-)subscribe to OnToolsChanged on the currently running server so
	// rename-in-place changes flip the dirty bit. Re-subscribes across server
	// restarts when the pointer changes. (Mirrors the legacy RebuildCatalog
	// subscription; the FTS5 index is now the live ranker.)
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

	// Atomic in-place rebuild from the live registry (DELETE+INSERT in one
	// transaction; no DB close/reopen). Indexes every tool's full description,
	// flattened input schema, examples, and patterns.
	FClaireonToolSearchIndex::RebuildFromLiveServer();
	const bool bBuilt = FClaireonToolSearchIndex::EnsureBuilt();

	// Rebuild the SEMANTIC index on the SAME trigger as the lexical index so both
	// stay in sync with the live registry. This re-embeds ~700 tools (ONNX RunSync
	// is several ms each), but the model + session are cached across rebuilds, so
	// only the per-tool embed loop is re-paid. When the ORT runtime / model / vocab
	// are unavailable the rebuild is a graceful no-op (IsReady() stays false;
	// Execute() falls back to lexical).
	// PERF FOLLOW-UP: if OnToolsChanged churns often, make this lazy/async (rebuild
	// on first semantic query after a dirty bit, or off the game thread) rather than
	// synchronously here. At current churn (tool registration is a one-time startup
	// burst) the synchronous batched cost is acceptable and easiest to reason about.
	FClaireonToolEmbeddingIndex::RebuildFromLiveServer();

	if (bBuilt)
	{
		LastCatalogToolCount = Server->GetTools().Num();
		UE_LOG(LogClaireon, Verbose, TEXT("[MCP] FTS5 tool search index rebuilt"));
	}
	else
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP] Failed to rebuild FTS5 tool search index"));
	}
	return bBuilt;
}

// ---------------------------------------------------------------------------
// Test-only ranked seam (zero-cost and entirely absent in non-test builds).
//
// Tests set GSearchToolsExecuteRankedSink to a TArray<FString>* before
// calling Execute().  Execute() fills it with the post-sort, PRE-truncation
// tool names in rank order so the corpus harness can measure top-K accuracy
// against the full ranked list (not just MaxResults entries).
// ---------------------------------------------------------------------------
#if WITH_UNTESTED
static TArray<FString>* GSearchToolsExecuteRankedSink = nullptr;

// static
void ClaireonTool_SearchTools::SetExecuteRankedSink(TArray<FString>* Sink)
{
	GSearchToolsExecuteRankedSink = Sink;
}
#endif // WITH_UNTESTED

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
	int32 MaxResults = 10;
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

		double MaxResultsVal = 10;
		if (Arguments->TryGetNumberField(TEXT("max_results"), MaxResultsVal))
		{
			MaxResults = static_cast<int32>(MaxResultsVal);
			if (MaxResults <= 0) MaxResults = 10;
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

	// The documented deferred-tool selector syntax `select:nameA,nameB`
	// short-circuits the SQL/fuzzy pipeline entirely and returns deep-inspect-shape
	// records for each named tool. It must be handled here so the prefix never
	// leaks into the SQLite pipeline (where it would surface as
	// `no such column: select`), keeping the convention documented at the top
	// of every Claude prompt intact.
	const TMap<FString, TSharedPtr<IClaireonTool>>& ToolsMap = Server->GetTools();
	if (Query.StartsWith(TEXT("select:"), ESearchCase::IgnoreCase))
	{
		const FString Selector = Query.Mid(FString(TEXT("select:")).Len()).TrimStartAndEnd();
		TArray<FString> RequestedNames;
		Selector.ParseIntoArray(RequestedNames, TEXT(","), /*InCullEmpty=*/ true);
		for (FString& N : RequestedNames)
		{
			N = N.TrimStartAndEnd();
		}

		TArray<TSharedPtr<FJsonValue>> Results;
		TArray<FString> NotFound;
		Results.Reserve(RequestedNames.Num());
		for (const FString& WantedName : RequestedNames)
		{
			if (WantedName.IsEmpty()) { continue; }
			const TSharedPtr<IClaireonTool>* FoundTool = ToolsMap.Find(WantedName);
			if (!FoundTool || !FoundTool->IsValid())
			{
				NotFound.Add(WantedName);
				continue;
			}
			const TSharedPtr<IClaireonTool>& Tool = *FoundTool;

			TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
			ToolObj->SetStringField(TEXT("name"), Tool->GetName());
			ToolObj->SetStringField(TEXT("category"), Tool->GetCategory());
			ToolObj->SetStringField(TEXT("description"), Tool->GetDescription());
			ToolObj->SetStringField(TEXT("signature"),
				FClaireonXmlFormatter::GenerateTypeSignature(Tool->GetName(), Tool->GetInputSchema()));
			if (TSharedPtr<FJsonObject> InputSchema = Tool->GetInputSchema())
			{
				ToolObj->SetObjectField(TEXT("input_schema"), InputSchema);
			}
			const FString Example = Tool->GetExampleUsage();
			if (!Example.IsEmpty())
			{
				ToolObj->SetStringField(TEXT("example_usage"), Example);
			}
			const FString Patterns = Tool->GetPatterns();
			if (!Patterns.IsEmpty())
			{
				ToolObj->SetStringField(TEXT("patterns"), Patterns);
			}
			if (TSharedPtr<FJsonObject> Tooltips = Tool->GetParameterTooltips())
			{
				ToolObj->SetObjectField(TEXT("parameter_tooltips"), Tooltips);
			}
			Results.Add(MakeShared<FJsonValueObject>(ToolObj));
		}

		TSharedPtr<FJsonObject> SelectData = MakeShared<FJsonObject>();
		SelectData->SetBoolField(TEXT("select"), true);
		SelectData->SetArrayField(TEXT("tools"), Results);
		if (NotFound.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> NotFoundJson;
			for (const FString& N : NotFound)
			{
				NotFoundJson.Add(MakeShared<FJsonValueString>(N));
			}
			SelectData->SetArrayField(TEXT("not_found"), NotFoundJson);
		}

		FString SelectSummary;
		if (NotFound.Num() == 0)
		{
			SelectSummary = FString::Printf(TEXT("Selected %d tool(s)"), Results.Num());
		}
		else
		{
			SelectSummary = FString::Printf(TEXT("Selected %d tool(s); %d not found"),
				Results.Num(), NotFound.Num());
		}
		return MakeSuccessResult(SelectData, SelectSummary);
	}

	// Cheap discovery mode -- return category names + tool counts only.
	// ~200 bytes regardless of catalog size, so agents can list-then-drill without
	// paying for the full per-tool surface.
	FString Mode;
	if (Arguments.IsValid())
	{
		Arguments->TryGetStringField(TEXT("mode"), Mode);
	}
	Mode = Mode.TrimStartAndEnd().ToLower();
	if (Mode == TEXT("categories"))
	{
		TMap<FString, int32> CategoryCounts;
		int32 TotalToolCount = 0;
		for (const auto& Pair : ToolsMap)
		{
			const TSharedPtr<IClaireonTool>& T = Pair.Value;
			if (!T.IsValid()) { continue; }
			const FString Name = T->GetName();
			if (Name == TEXT("python_execute") || Name == TEXT("tool_search")) { continue; }
			++TotalToolCount;
			CategoryCounts.FindOrAdd(T->GetCategory(), 0)++;
		}
		TArray<FString> CategoryKeys;
		CategoryCounts.GetKeys(CategoryKeys);
		CategoryKeys.Sort();

		TArray<TSharedPtr<FJsonValue>> Cats;
		Cats.Reserve(CategoryKeys.Num());
		for (const FString& CatName : CategoryKeys)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("name"), CatName);
			Obj->SetNumberField(TEXT("tool_count"), CategoryCounts[CatName]);
			Cats.Add(MakeShared<FJsonValueObject>(Obj));
		}
		TSharedPtr<FJsonObject> CatData = MakeShared<FJsonObject>();
		CatData->SetArrayField(TEXT("categories"), Cats);
		CatData->SetNumberField(TEXT("total_categories"), CategoryKeys.Num());
		CatData->SetNumberField(TEXT("total_tools"), TotalToolCount);
		const FString CatSummary = FString::Printf(TEXT("Found %d categories spanning %d tools"),
			CategoryKeys.Num(), TotalToolCount);
		return MakeSuccessResult(CatData, CatSummary);
	}

	// When detail='full' and the caller did not explicitly override
	// max_results, clamp the default to 5 so include_schema/include_examples does
	// not return an 85k-character response that overflows assistant context.
	if (Detail == TEXT("full"))
	{
		const bool bExplicitMaxResults = Arguments.IsValid()
			&& Arguments->HasField(TEXT("max_results"));
		if (!bExplicitMaxResults && MaxResults > 5)
		{
			MaxResults = 5;
		}
	}

	// Strip boolean operators and quote/paren punctuation so callers that write
	// pseudo-boolean queries ("blueprint AND chooser") still benefit from per-word
	// union ranking. Delegated to the SHARED canonical normalizer so the lexical AND
	// semantic channels strip identically from one source of truth -- a
	// boolean-decorated query must rank the same as the plain query, which requires
	// the SEMANTIC channel (which embeds the raw string) to see the same normalized
	// text. NormalizeQueryForRetrieval is also applied inside FindNearestSemantic,
	// so this Execute-layer call keeps the visible `query` echo + lexical tokens
	// consistent with what the semantic channel embeds.
	Query = FClaireonToolSearchIndex::NormalizeQueryForRetrieval(Query);

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
		// Deep-inspect always surfaces GetPatterns() when non-empty. Empty
		// returns are dropped so deep-inspect responses for non-overriding
		// tools stay byte-identical.
		const FString InspectPatterns = Tool->GetPatterns();
		if (!InspectPatterns.IsEmpty())
		{
			ToolObj->SetStringField(TEXT("patterns"), InspectPatterns);
		}
		// For apply_spec / instance_apply_spec families, look up the bare
		// GetCategory() in ApplySpecCatalog.json and surface the matching
		// entry under `spec_shape`. No-match path (missing plugin, missing
		// file, parse error, or unknown family) silently omits the field;
		// the tool still succeeds.
		const FString InspectOp = Tool->GetOperation();
		if (InspectOp == TEXT("apply_spec") || InspectOp == TEXT("instance_apply_spec"))
		{
			const FString Family = Tool->GetCategory();
			TSharedPtr<FJsonObject> Catalog =
				ClaireonTool_SearchToolsInternal::Cl622SearchSpec_LoadCatalog();
			if (Catalog.IsValid())
			{
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				if (Catalog->TryGetObjectField(Family, Entry)
					&& Entry && (*Entry).IsValid())
				{
					ToolObj->SetObjectField(TEXT("spec_shape"), *Entry);
				}
			}
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

		TSharedPtr<FJsonObject> InspectData = MakeShared<FJsonObject>();
		InspectData->SetBoolField(TEXT("deep_inspect"), true);
		InspectData->SetObjectField(TEXT("tool"), ToolObj);

		const FString InspectSummary = FString::Printf(TEXT("Deep inspect: %s"), *InspectToolName);
		return MakeSuccessResult(InspectData, InspectSummary);
	}

	// -----------------------------------------------------------------------
	// Hybrid ranking via FClaireonToolSearchIndex::FindNearestHybrid.
	//
	// FindNearestHybrid is the sole ranker for the query path. It fuses the
	// UNPINNED lexical bm25 list and the semantic cosine list via RRF, then
	// applies exact / near-exact name pinning ONCE, internally. It scans the FULL
	// live registry for the exact-name pin, so an agent that types a tool's exact
	// name still gets it at rank 0 even when FTS/semantic recall missed it.
	//
	// Pinning happens exactly once, inside FindNearestHybrid. Each match carries a
	// RankSource (ExactPin / NearExactBoost / HybridRRF / LexicalOnlyFallback);
	// it is kept INTERNAL (not emitted on the wire) and drives footer suppression
	// for genuine exact-name lookups below.
	// -----------------------------------------------------------------------
	struct FRankedEntry
	{
		FString Name;
		FString Category;
		EClaireonRankSource RankSource = EClaireonRankSource::LexicalOnlyFallback;
	};
	TArray<FRankedEntry> RankedEntries;

	if (!Query.IsEmpty())
	{
		// Build / refresh the FTS5 index when the tool count changed OR the
		// OnToolsChanged dirty bit was set (rename-in-place / unregister paths
		// that leave the count constant). exchange(false) atomically drains the
		// bit so a concurrent broadcast queues the next rebuild.
		const bool bCountChanged = (LastCatalogToolCount != ToolsMap.Num());
		const bool bDirtyDrained = ClaireonTool_SearchToolsInternal::bToolCatalogDirty.exchange(false);
		if (bCountChanged || bDirtyDrained)
		{
			RebuildSearchIndex();
		}
		else
		{
			// No staleness signal -- make sure the index exists (cold path after
			// a server restart where the count happens to match).
			FClaireonToolSearchIndex::EnsureBuilt();
		}

		// Fallback diagnostic: when the embedding index is not ready (no ORT runtime
		// / model / vocab), FindNearestHybrid returns the lexical-only fusion tagged
		// LexicalOnlyFallback. Emit ONE diagnostic line for ops visibility; the
		// surface NEVER errors on a missing model.
		if (!FClaireonToolEmbeddingIndex::IsReady())
		{
			UE_LOG(LogClaireon, Display,
				TEXT("[tool_search] Semantic embedding index unavailable; serving lexical-only results (LexicalOnlyFallback)."));
		}

		// Hybrid retrieval. FindNearestHybrid fuses lexical + semantic via RRF and
		// applies exact/near-exact pinning ONCE, internally (scanning the full live
		// registry for the exact pin). No additional Execute-layer pin pass runs.
		const TArray<FClaireonToolCatalogMatch> Matches =
			FClaireonToolSearchIndex::FindNearestHybrid(Query, MaxResults, Category);

		RankedEntries.Reserve(Matches.Num());
		for (const FClaireonToolCatalogMatch& M : Matches)
		{
			FRankedEntry E;
			E.Name       = M.Name;
			E.Category   = M.Category;
			E.RankSource = M.RankSource;
			RankedEntries.Add(MoveTemp(E));
		}
	}
	else
	{
		// Empty query: flat catalog listing (browse). No bm25 rank to apply, so
		// order by (category, name) for a stable deterministic listing. Respects
		// the category filter. mode=categories handles the cheap grouped browse.
		const FString CategoryLower = Category.ToLower();
		for (const auto& Pair : ToolsMap)
		{
			const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
			if (!Tool.IsValid()) { continue; }
			const FString ToolName = Tool->GetName();
			if (ToolName == TEXT("python_execute") || ToolName == TEXT("tool_search")) { continue; }
			const FString ToolCategory = Tool->GetCategory();
			if (!CategoryLower.IsEmpty() && ToolCategory.ToLower() != CategoryLower) { continue; }

			FRankedEntry E;
			E.Name      = ToolName;
			E.Category  = ToolCategory;
			RankedEntries.Add(MoveTemp(E));
		}
		RankedEntries.Sort([](const FRankedEntry& A, const FRankedEntry& B)
		{
			if (A.Category != B.Category) { return A.Category < B.Category; }
			return A.Name < B.Name;
		});
	}

	// Test-only seam: capture the full pre-truncation ranked names in rank order.
#if WITH_UNTESTED
	if (GSearchToolsExecuteRankedSink != nullptr)
	{
		GSearchToolsExecuteRankedSink->Reset();
		GSearchToolsExecuteRankedSink->Reserve(RankedEntries.Num());
		for (const FRankedEntry& E : RankedEntries)
		{
			GSearchToolsExecuteRankedSink->Add(E.Name);
		}
	}
#endif // WITH_UNTESTED

	// Truncate to MaxResults after ranking. (FindNearestHybrid already caps to
	// MaxResults and pins internally; the empty-query browse branch does not, so
	// re-clamp here defensively.)
	const int32 TotalMatching = RankedEntries.Num();
	if (RankedEntries.Num() > MaxResults)
	{
		RankedEntries.SetNum(MaxResults);
	}

	// Build structured Data: a FLAT, globally rank-ordered `tools[]` array.
	// Query results no longer group by category (the agent's question is "what
	// do I call next?", which a global rank answers directly). `mode=categories`
	// keeps the grouped browse surface; deep-inspect / select: record shapes are
	// unchanged. Ranking internals (score, rank_source, provider source) are NOT
	// emitted -- order alone carries the signal.
	TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
	if (!Query.IsEmpty())
	{
		DataObj->SetStringField(TEXT("query"), Query);
	}
	DataObj->SetNumberField(TEXT("total_matching"), TotalMatching);
	DataObj->SetNumberField(TEXT("returned"), RankedEntries.Num());

	TArray<TSharedPtr<FJsonValue>> ToolsArr;
	ToolsArr.Reserve(RankedEntries.Num());
	for (const FRankedEntry& Entry : RankedEntries)
	{
		const TSharedPtr<IClaireonTool>* FoundTool = ToolsMap.Find(Entry.Name);
		if (!FoundTool || !FoundTool->IsValid()) { continue; }
		const TSharedPtr<IClaireonTool>& Tool = *FoundTool;

		// Select description tier.
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

		TSharedPtr<FJsonObject> ToolSchema = Tool->GetInputSchema();

		TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
		ToolObj->SetStringField(TEXT("name"), Entry.Name);
		ToolObj->SetStringField(TEXT("category"), Entry.Category);
		ToolObj->SetStringField(TEXT("description"), ToolDescription);
		ToolObj->SetStringField(TEXT("signature"),
			FClaireonXmlFormatter::GenerateTypeSignature(Entry.Name, ToolSchema));
		if (bIncludeSchema && ToolSchema.IsValid())
		{
			ToolObj->SetObjectField(TEXT("input_schema"), ToolSchema);
		}
		if (bIncludeExamples)
		{
			const FString Example = Tool->GetExampleUsage();
			if (!Example.IsEmpty())
			{
				ToolObj->SetStringField(TEXT("example_usage"), Example);
			}
		}
		// Patterns surface only on the full-detail tier; empty returns are dropped.
		if (Detail == TEXT("full"))
		{
			const FString Patterns = Tool->GetPatterns();
			if (!Patterns.IsEmpty())
			{
				ToolObj->SetStringField(TEXT("patterns"), Patterns);
			}
		}

		ToolsArr.Add(MakeShared<FJsonValueObject>(ToolObj));
	}
	DataObj->SetArrayField(TEXT("tools"), ToolsArr);

	const int32 ReturnedCount = ToolsArr.Num();

	// Build plain text summary.
	FString SummaryText;
	if (Query.IsEmpty())
	{
		SummaryText = FString::Printf(TEXT("Found %d tool(s)"), TotalMatching);
	}
	else
	{
		SummaryText = FString::Printf(TEXT("Found %d tool(s) matching '%s'"), TotalMatching, *Query);
	}
	if (TotalMatching > ReturnedCount)
	{
		SummaryText += FString::Printf(TEXT(" (showing %d of %d)"), ReturnedCount, TotalMatching);
	}

	// Upgrade-path footer. Tells the agent how to escalate to full detail
	// without a follow-up search. Suppressed when:
	//   - already at full detail (nothing to escalate to);
	//   - the deep-inspect bypass already short-circuited earlier (defensive --
	//     deep-inspect returns at MakeSuccessResult above);
	//   - no hits to recommend (genuine zero retrieval);
	//   - the query is a GENUINE exact tool-name lookup -- i.e. the rank-0 result is
	//     an exact_pin whose name equals the query (the agent already has the
	//     canonical name, so there is nothing to "upgrade" to).
	//
	// Footer suppression for the single-exact case must key off a GENUINE exact
	// match (internal RankSource == ExactPin at position 0), NOT merely "exactly one
	// result exists". Keying off ReturnedCount==1 would either (a) FALSELY suppress
	// the footer for a one-hit ordinary search, or (b) FALSELY show it when semantic
	// fusion happens to leave one result. The honest signal is "the query was a
	// tool-name lookup", which the ExactPin RankSource at position 0 encodes
	// directly. The genuine zero-retrieval footer (ReturnedCount==0) is preserved.
	const FString FirstResultName = (RankedEntries.Num() > 0) ? RankedEntries[0].Name : FString();
	// RankSource == ExactPin at position 0 IS the genuine-exact-name-lookup signal:
	// FindNearestHybrid sets ExactPin only when the name-normalized query (lowercased,
	// '-'/'_' stripped) equals a tool's name-normalized form. So this correctly covers
	// hyphen/underscore-equivalent lookups too, without re-deriving the normalization
	// here.
	const bool bGenuineExactNameLookup = (RankedEntries.Num() > 0)
		&& RankedEntries[0].RankSource == EClaireonRankSource::ExactPin;
	const bool bSuppressFooter =
		Detail == TEXT("full") ||
		!InspectToolName.IsEmpty() ||
		ReturnedCount == 0 ||
		bGenuineExactNameLookup;

	if (!bSuppressFooter)
	{
		SummaryText += FString::Printf(
			TEXT("\ntip: call tool_search(name=\"%s\", detail=\"full\") "
				 "for schema, examples, and patterns"),
			*FirstResultName);
	}

	// Always append the invocation reminder so callers know the transport.
	if (ReturnedCount > 0)
	{
		SummaryText += TEXT("\ninvoke via: mcp__claireon__python_execute with `import claireon; result = claireon.<tool_name>(...)`");
	}

	return MakeSuccessResult(DataObj, SummaryText);
}
