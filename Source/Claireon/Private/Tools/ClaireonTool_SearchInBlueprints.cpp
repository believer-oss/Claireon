// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_SearchInBlueprints.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h" // kBPCategory
#include "ClaireonLog.h"
#include "ClaireonSettings.h"
#include "FindInBlueprintManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include "Templates/Function.h"

#include <atomic>

// ---------------------------------------------------------------------------
// ClaireonSearchWait -- testable wait/guard helpers for bp_search.
//
// External linkage on purpose: ClaireonSearchTimeoutContractTests.cpp
// (Private/Tests/) and ClaireonTool_SearchInBlueprintsIndexStatus.cpp
// re-declare these signatures instead of including a header. Keep the
// declarations in those files in sync with the definitions here.
// ---------------------------------------------------------------------------
namespace ClaireonSearchWait
{

// File-scope state. bp_search executes on the game thread (the MCP server
// marshals Execute there), but the flags are atomic so a guard probe from
// another thread can never race.
static std::atomic<bool> GClaireonSearchInFlight{ false };
static std::atomic<bool> GClaireonAnySearchCompletedThisSession{ false };

// Timed-out FStreamSearch workers that did not drain within the grace period
// are parked here (game thread only) so their destructor does not block on a
// still-running thread. Pruned at the start of the next bp_search call.
static TArray<TSharedPtr<FStreamSearch>> GClaireonOrphanedStreamSearches;

/**
 * Core wait loop for bp_search, extracted so it can be unit-tested with an
 * injected clock and search stub (no engine FiB machinery required).
 *
 * Contract:
 * - The timeout is a HARD bound on simulated/observed time: there is exactly
 *   one wait cycle and never a hidden retry.
 * - On expiry, StopAndDrain is invoked exactly once BEFORE returning false.
 * - PumpIndexing runs once per poll iteration so deferred FiB indexing (which
 *   only advances in FFindInBlueprintSearchManager's game-thread Tick) can
 *   make progress while the game thread is otherwise blocked here.
 *
 * @return true if IsComplete() became true before the timeout expired.
 */
bool WaitForSearchWithHardTimeout(
	double TimeoutSeconds,
	const TFunction<bool()>& IsComplete,
	const TFunction<double()>& GetTimeSeconds,
	const TFunction<void()>& PumpIndexing,
	const TFunction<void()>& Yield,
	const TFunction<void()>& StopAndDrain,
	double& OutElapsedSeconds)
{
	check(IsComplete);
	check(GetTimeSeconds);

	const double StartSeconds = GetTimeSeconds();
	while (!IsComplete())
	{
		const double ElapsedSeconds = GetTimeSeconds() - StartSeconds;
		if (ElapsedSeconds >= TimeoutSeconds)
		{
			if (StopAndDrain)
			{
				StopAndDrain();
			}
			OutElapsedSeconds = ElapsedSeconds;
			return false;
		}

		if (PumpIndexing)
		{
			PumpIndexing();
		}
		if (Yield)
		{
			Yield();
		}
	}

	OutElapsedSeconds = GetTimeSeconds() - StartSeconds;
	return true;
}

/** Attempts to mark a bp_search as in flight. Returns false if one already is. */
bool TryAcquireSearchInFlightGuard()
{
	bool bExpected = false;
	return GClaireonSearchInFlight.compare_exchange_strong(bExpected, true);
}

/** Releases the in-flight guard taken by TryAcquireSearchInFlightGuard(). */
void ReleaseSearchInFlightGuard()
{
	GClaireonSearchInFlight.store(false);
}

/** Returns true while a bp_search is in flight. */
bool IsSearchInFlight()
{
	return GClaireonSearchInFlight.load();
}

/** Records that a stream search ran to completion this editor session. */
void MarkSearchCompletedThisSession()
{
	GClaireonAnySearchCompletedThisSession.store(true);
}

/**
 * True once any bp_search stream search has completed this editor session.
 * Read by bp_search_index_status: until this is true, the first search may
 * still trigger the FiB manager's deferred full-corpus indexing pass, whose
 * backlog is not observable through any public engine API.
 */
bool HasAnySearchCompletedThisSession()
{
	return GClaireonAnySearchCompletedThisSession.load();
}

/** Test seam: clears the session-completion marker. */
void ResetSearchCompletedThisSessionForTests()
{
	GClaireonAnySearchCompletedThisSession.store(false);
}

/** Drops parked timed-out searches whose worker threads have since finished. */
static void ClaireonSearchWait_PruneOrphanedSearches()
{
	GClaireonOrphanedStreamSearches.RemoveAll([](const TSharedPtr<FStreamSearch>& Search)
	{
		return !Search.IsValid() || Search->IsComplete();
	});
}

/** Parks a stopped-but-not-yet-drained search for deferred cleanup. */
static void ClaireonSearchWait_ParkOrphanedSearch(TSharedPtr<FStreamSearch> Search)
{
	GClaireonOrphanedStreamSearches.Add(MoveTemp(Search));
}

} // namespace ClaireonSearchWait

FString ClaireonTool_SearchInBlueprints::GetCategory() const { return kBPCategory; }
FString ClaireonTool_SearchInBlueprints::GetOperation() const { return TEXT("search"); }

TArray<FString> ClaireonTool_SearchInBlueprints::GetSearchKeywords() const
{
	return {TEXT("bp"), TEXT("blueprint"), TEXT("search"), TEXT("find"), TEXT("find_in_blueprints"), TEXT("query"), TEXT("nodes"), TEXT("pins")};
}

FString ClaireonTool_SearchInBlueprints::GetDescription() const
{
    return TEXT("Search within Blueprint content (nodes, pins, values, comments). Equivalent to Edit > Find in Blueprints. Supports expression syntax: plain text, Nodes(\"Name\"), Pins(\"Name\"), AND/OR/NOT. Stateless / read-only / non-session.");
}

TSharedPtr<FJsonObject> ClaireonTool_SearchInBlueprints::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// query - required
	TSharedPtr<FJsonObject> QueryProp = MakeShared<FJsonObject>();
	QueryProp->SetStringField(TEXT("type"), TEXT("string"));
	QueryProp->SetStringField(TEXT("description"), TEXT("Search term. Supports expression syntax: plain text, Nodes(\"Name\"), Pins(\"Name\"), AND/OR/NOT."));
	Properties->SetObjectField(TEXT("query"), QueryProp);

	// path_filter - optional
	TSharedPtr<FJsonObject> PathProp = MakeShared<FJsonObject>();
	PathProp->SetStringField(TEXT("type"), TEXT("string"));
	PathProp->SetStringField(TEXT("description"), TEXT("Filter results to blueprints whose path contains this substring (case-insensitive). E.g. /Game/Characters"));
	Properties->SetObjectField(TEXT("path_filter"), PathProp);

	// filter - optional enum
	TSharedPtr<FJsonObject> FilterProp = MakeShared<FJsonObject>();
	FilterProp->SetStringField(TEXT("type"), TEXT("string"));
	FilterProp->SetStringField(TEXT("description"), TEXT("Limit result types: all (default), nodes, pins, graphs, functions, macros, properties, variables, components"));
	TArray<TSharedPtr<FJsonValue>> FilterEnum;
	FilterEnum.Add(MakeShared<FJsonValueString>(TEXT("all")));
	FilterEnum.Add(MakeShared<FJsonValueString>(TEXT("nodes")));
	FilterEnum.Add(MakeShared<FJsonValueString>(TEXT("pins")));
	FilterEnum.Add(MakeShared<FJsonValueString>(TEXT("graphs")));
	FilterEnum.Add(MakeShared<FJsonValueString>(TEXT("functions")));
	FilterEnum.Add(MakeShared<FJsonValueString>(TEXT("macros")));
	FilterEnum.Add(MakeShared<FJsonValueString>(TEXT("properties")));
	FilterEnum.Add(MakeShared<FJsonValueString>(TEXT("variables")));
	FilterEnum.Add(MakeShared<FJsonValueString>(TEXT("components")));
	FilterProp->SetArrayField(TEXT("enum"), FilterEnum);
	Properties->SetObjectField(TEXT("filter"), FilterProp);

	// max_results - optional
	TSharedPtr<FJsonObject> MaxProp = MakeShared<FJsonObject>();
	MaxProp->SetStringField(TEXT("type"), TEXT("integer"));
	MaxProp->SetStringField(TEXT("description"), TEXT("Maximum number of matching blueprints to return (default: 50, max: 200)"));
	Properties->SetObjectField(TEXT("max_results"), MaxProp);

	// timeout - optional per-call override
	TSharedPtr<FJsonObject> TimeoutProp = MakeShared<FJsonObject>();
	TimeoutProp->SetStringField(TEXT("type"), TEXT("number"));
	TimeoutProp->SetStringField(TEXT("description"), TEXT("Search timeout in seconds (default: from settings, typically 30). HARD bound: on expiry the search is stopped and a timeout error is returned immediately -- there is no hidden retry. Increase if the FiB index is still building (see bp_search_index_status)."));
	Properties->SetObjectField(TEXT("timeout"), TimeoutProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("query")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_SearchInBlueprints::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Validate query
	if (!Arguments->HasField(TEXT("query")))
	{
		return MakeErrorResult(TEXT("Missing required parameter: query"));
	}

	FString Query = Arguments->GetStringField(TEXT("query"));
	if (Query.IsEmpty())
	{
		return MakeErrorResult(TEXT("Query must not be empty"));
	}

	// Optional parameters
	FString PathFilter;
	if (Arguments->HasField(TEXT("path_filter")))
	{
		PathFilter = Arguments->GetStringField(TEXT("path_filter"));
	}

	ESearchQueryFilter SearchFilter = ESearchQueryFilter::AllFilter;
	if (Arguments->HasField(TEXT("filter")))
	{
		FString FilterStr = Arguments->GetStringField(TEXT("filter"));
		if (!FilterStr.IsEmpty() && !ParseFilterString(FilterStr, SearchFilter))
		{
			return MakeErrorResult(FString::Printf(TEXT("Invalid filter value: %s"), *FilterStr));
		}
	}

	int32 MaxResults = 50;
	if (Arguments->HasField(TEXT("max_results")))
	{
		MaxResults = FMath::Clamp(static_cast<int32>(Arguments->GetNumberField(TEXT("max_results"))), 1, 200);
	}

	// Reentrancy guard: a stream search monopolizes the FiB manager and (via
	// the pumped wait below) the game thread. Client retries of a slow search
	// used to stack behind the server's game-thread lock and re-run the same
	// expensive scan back-to-back; reject overlap up front instead.
	if (!ClaireonSearchWait::TryAcquireSearchInFlightGuard())
	{
		return MakeErrorResult(TEXT("bp_search is busy: another Blueprint search is already in flight. Concurrent searches are not supported; wait for the in-flight search to complete or time out, then retry."));
	}
	ON_SCOPE_EXIT { ClaireonSearchWait::ReleaseSearchInFlightGuard(); };

	// Drop any previously timed-out searches whose workers have since finished.
	ClaireonSearchWait::ClaireonSearchWait_PruneOrphanedSearches();

	// Run search via FFindInBlueprintSearchManager.
	FFindInBlueprintSearchManager& SearchManager = FFindInBlueprintSearchManager::Get();

	TArray<TSharedPtr<FFindInBlueprintsResult>> RawResults;
	FStreamSearchOptions SearchOptions;
	SearchOptions.MinimiumVersionRequirement = EFiBVersion::FIB_VER_LATEST;
	SearchOptions.ImaginaryDataFilter = SearchFilter;

	TSharedPtr<FStreamSearch> StreamSearch = MakeShared<FStreamSearch>(Query, SearchOptions);

	// Poll with yield -- FStreamSearch runs on background threads managed by
	// FFindInBlueprintSearchManager. We must NOT call ProcessThreadUntilIdle here
	// because Execute() may already be running inside a game-thread task graph
	// task (e.g. when invoked via the REPL client's AsyncTask dispatch), and
	// re-entrant task processing triggers a fatal recursion guard assertion.
	//
	// FiB DEFERRED INDEXING only advances inside FFindInBlueprintSearchManager's
	// game-thread Tick. While Execute() blocks the game thread here, the engine
	// tick loop never runs, so a plain sleep-poll would starve the very indexing
	// it is waiting for. We therefore pump the manager's Tick manually, one
	// bounded slice per poll iteration. Calling the manager's Tick directly is
	// NOT task-graph processing, so the recursion guard above does not apply.
	double MaxWaitSeconds = GetDefault<UClaireonSettings>()->BlueprintSearchTimeoutSeconds;
	// Per-call timeout override
	double TimeoutOverride = 0.0;
	if (Arguments->TryGetNumberField(TEXT("timeout"), TimeoutOverride) && TimeoutOverride > 0.0)
	{
		MaxWaitSeconds = TimeoutOverride;
	}

	const bool bOnGameThread = IsInGameThread();
	double ElapsedSeconds = 0.0;
	const bool bCompleted = ClaireonSearchWait::WaitForSearchWithHardTimeout(
		MaxWaitSeconds,
		/*IsComplete*/ [&StreamSearch]()
		{
			return StreamSearch->IsComplete();
		},
		/*GetTimeSeconds*/ []()
		{
			return FPlatformTime::Seconds();
		},
		/*PumpIndexing*/ [&SearchManager, bOnGameThread]()
		{
			if (bOnGameThread)
			{
				SearchManager.Tick(0.01f);
			}
		},
		/*Yield*/ []()
		{
			FPlatformProcess::Sleep(0.01f); // 10ms yield
		},
		/*StopAndDrain*/ [&StreamSearch, &SearchManager]()
		{
			// Unregister the query from the manager first so the worker's next
			// ContinueSearchQuery() returns false and its Run() loop exits.
			SearchManager.EnsureSearchQueryEnds(StreamSearch.Get());
			StreamSearch->Stop();

			// Bounded drain WITHOUT task-graph processing (FStreamSearch::
			// EnsureCompletion calls ProcessThreadUntilIdle, which would hit
			// the recursion guard described above). If the worker is blocked
			// on a game-thread task it can only finish after we return, so
			// park the search rather than blocking in its destructor.
			const double DrainDeadline = FPlatformTime::Seconds() + 2.0;
			while (!StreamSearch->IsComplete() && FPlatformTime::Seconds() < DrainDeadline)
			{
				FPlatformProcess::Sleep(0.01f);
			}
			if (!StreamSearch->IsComplete())
			{
				UE_LOG(LogClaireon, Warning,
					TEXT("[SearchInBlueprints] Stream search worker did not drain within 2s of Stop(); parking it for deferred cleanup."));
				ClaireonSearchWait::ClaireonSearchWait_ParkOrphanedSearch(StreamSearch);
			}
		},
		ElapsedSeconds);

	if (!bCompleted)
	{
		UE_LOG(LogClaireon, Warning, TEXT("[SearchInBlueprints] Search timed out after %.1fs (hard bound, no retry) for query: %s"),
			ElapsedSeconds, *Query);

		return MakeErrorResult(FString::Printf(
			TEXT("Blueprint search timed out after %.0fs (hard bound, no retry). The search was stopped and unregistered. The FiB index may still be building -- check bp_search_index_status, or retry with a larger 'timeout'. Query: %s"),
			MaxWaitSeconds, *Query));
	}

	// A search ran to completion: from here on the FiB deferred-indexing
	// backlog has been flushed, so index_status can stop warning about
	// first-search cost.
	ClaireonSearchWait::MarkSearchCompletedThisSession();

	StreamSearch->GetFilteredItems(RawResults);

	// Apply path filter and build hits array
	TArray<TSharedPtr<FJsonValue>> HitsArray;
	TSet<FString> BlueprintPathsSeen;
	int32 TotalHits = 0;

	for (const TSharedPtr<FFindInBlueprintsResult>& TopResult : RawResults)
	{
		if (!TopResult.IsValid())
		{
			continue;
		}

		// Top-level result is the Blueprint itself
		FString BlueprintPath = TopResult->GetDisplayString().ToString();

		// Apply path filter
		if (!PathFilter.IsEmpty() && !BlueprintPath.Contains(PathFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		BlueprintPathsSeen.Add(BlueprintPath);

		// Collect all child hits (nodes, pins, etc.)
		// NOTE: Result accessors (GetDisplayString, GetCategory, GetCommentText) are
		// virtual calls on FFindInBlueprintsResult subclasses. We wrap them in a
		// try-style guard (null checks) to avoid crashes if the FiB index contained
		// stale or partially-constructed entries.
		TFunction<void(const TSharedPtr<FFindInBlueprintsResult>&, int32)> CollectHits;
		CollectHits = [&](const TSharedPtr<FFindInBlueprintsResult>& Result, int32 Depth)
		{
			if (!Result.IsValid())
			{
				return;
			}

			// Skip the top-level blueprint entry (Depth == 0), it is the container
			if (Depth > 0)
			{
				TotalHits++;

				if (HitsArray.Num() < MaxResults)
				{
					TSharedPtr<FJsonObject> HitObj = MakeShared<FJsonObject>();
					HitObj->SetStringField(TEXT("blueprint_path"), BlueprintPath);

					FText DisplayText = Result->GetDisplayString();
					FString DisplayStr = DisplayText.IsEmpty() ? TEXT("(unnamed)") : DisplayText.ToString();

					FText CategoryText = Result->GetCategory();
					FString CategoryStr = CategoryText.IsEmpty() ? TEXT("unknown") : CategoryText.ToString();

					HitObj->SetStringField(TEXT("node_title"), DisplayStr);
					HitObj->SetStringField(TEXT("match_type"), CategoryStr);

					// Comment as context if available
					FString Comment = Result->GetCommentText();
					if (!Comment.IsEmpty())
					{
						HitObj->SetStringField(TEXT("context"), Comment);
					}
					else
					{
						HitObj->SetStringField(TEXT("context"), TEXT(""));
					}

					HitsArray.Add(MakeShared<FJsonValueObject>(HitObj));
				}
			}

			// Copy children array before iterating -- the underlying TArray may be
			// modified if the search manager is still processing in the background.
			TArray<TSharedPtr<FFindInBlueprintsResult>> ChildrenCopy = Result->Children;
			for (const TSharedPtr<FFindInBlueprintsResult>& Child : ChildrenCopy)
			{
				CollectHits(Child, Depth + 1);
			}
		};

		CollectHits(TopResult, 0);
	}

	// Build result data
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("query"), Query);
	Data->SetNumberField(TEXT("total_hits"), TotalHits);
	Data->SetNumberField(TEXT("blueprints_matched"), BlueprintPathsSeen.Num());
	Data->SetArrayField(TEXT("hits"), HitsArray);

	if (!PathFilter.IsEmpty())
	{
		Data->SetStringField(TEXT("path_filter"), PathFilter);
	}

	// Summary
	FString Summary = FString::Printf(
		TEXT("Found %d matches for '%s' across %d blueprints"),
		TotalHits,
		*Query,
		BlueprintPathsSeen.Num());

	if (TotalHits > MaxResults)
	{
		Summary += FString::Printf(TEXT(" (showing first %d)"), MaxResults);
	}

	return MakeSuccessResult(Data, Summary);
}

bool ClaireonTool_SearchInBlueprints::ParseFilterString(const FString& FilterStr, ESearchQueryFilter& OutFilter)
{
	if (FilterStr.Equals(TEXT("all"), ESearchCase::IgnoreCase))
	{
		OutFilter = ESearchQueryFilter::AllFilter;
		return true;
	}
	if (FilterStr.Equals(TEXT("nodes"), ESearchCase::IgnoreCase))
	{
		OutFilter = ESearchQueryFilter::NodesFilter;
		return true;
	}
	if (FilterStr.Equals(TEXT("pins"), ESearchCase::IgnoreCase))
	{
		OutFilter = ESearchQueryFilter::PinsFilter;
		return true;
	}
	if (FilterStr.Equals(TEXT("graphs"), ESearchCase::IgnoreCase))
	{
		OutFilter = ESearchQueryFilter::GraphsFilter;
		return true;
	}
	if (FilterStr.Equals(TEXT("functions"), ESearchCase::IgnoreCase))
	{
		OutFilter = ESearchQueryFilter::FunctionsFilter;
		return true;
	}
	if (FilterStr.Equals(TEXT("macros"), ESearchCase::IgnoreCase))
	{
		OutFilter = ESearchQueryFilter::MacrosFilter;
		return true;
	}
	if (FilterStr.Equals(TEXT("properties"), ESearchCase::IgnoreCase))
	{
		OutFilter = ESearchQueryFilter::PropertiesFilter;
		return true;
	}
	if (FilterStr.Equals(TEXT("variables"), ESearchCase::IgnoreCase))
	{
		OutFilter = ESearchQueryFilter::VariablesFilter;
		return true;
	}
	if (FilterStr.Equals(TEXT("components"), ESearchCase::IgnoreCase))
	{
		OutFilter = ESearchQueryFilter::ComponentsFilter;
		return true;
	}

	return false;
}

void ClaireonTool_SearchInBlueprints::FormatResultTree(const TSharedPtr<FFindInBlueprintsResult>& Result, int32 Depth, FString& OutText)
{
	if (!Result.IsValid())
	{
		return;
	}

	// Build indentation
	FString Indent;
	if (Depth == 0)
	{
		Indent = TEXT("=== ");
	}
	else
	{
		for (int32 i = 0; i < Depth; ++i)
		{
			Indent += TEXT("  ");
		}
	}

	// Format this node
	FString NodeText = FormatResultNode(Result);
	if (!NodeText.IsEmpty())
	{
		if (Depth == 0)
		{
			// Top-level blueprint entry: === DisplayText ===
			OutText += FString::Printf(TEXT("=== %s ===\n"), *NodeText);
		}
		else
		{
			OutText += FString::Printf(TEXT("%s%s\n"), *Indent, *NodeText);
		}
	}

	// Recurse into children
	for (const TSharedPtr<FFindInBlueprintsResult>& Child : Result->Children)
	{
		FormatResultTree(Child, Depth + 1, OutText);
	}
}

FString ClaireonTool_SearchInBlueprints::FormatResultNode(const TSharedPtr<FFindInBlueprintsResult>& Result)
{
	if (!Result.IsValid())
	{
		return FString();
	}

	FString DisplayStr = Result->GetDisplayString().ToString();
	if (DisplayStr.IsEmpty())
	{
		return FString();
	}

	// Append category if available
	FText Category = Result->GetCategory();
	if (!Category.IsEmpty())
	{
		DisplayStr += FString::Printf(TEXT(" [%s]"), *Category.ToString());
	}

	// Append comment if available
	FString Comment = Result->GetCommentText();
	if (!Comment.IsEmpty())
	{
		DisplayStr += FString::Printf(TEXT(" // %s"), *Comment);
	}

	return DisplayStr;
}
