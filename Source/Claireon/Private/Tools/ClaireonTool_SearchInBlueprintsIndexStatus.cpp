// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_SearchInBlueprintsIndexStatus.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h" // kBPCategory
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLog.h"
#include "FindInBlueprintManager.h"
#include "Dom/JsonObject.h"

// Defined in ClaireonTool_SearchInBlueprints.cpp (same module). Session-scoped
// marker set once a bp_search stream search has run to completion. Keep this
// declaration in sync with the definition there.
namespace ClaireonSearchWait
{
	bool HasAnySearchCompletedThisSession();
}

FString ClaireonTool_SearchInBlueprintsIndexStatus::GetCategory() const { return kBPCategory; }
FString ClaireonTool_SearchInBlueprintsIndexStatus::GetOperation() const { return TEXT("search_index_status"); }

TArray<FString> ClaireonTool_SearchInBlueprintsIndexStatus::GetSearchKeywords() const
{
	return {TEXT("bp"), TEXT("blueprint"), TEXT("search"), TEXT("index"), TEXT("fib"), TEXT("status"), TEXT("ready"), TEXT("indexed"), TEXT("cache")};
}

FString ClaireonTool_SearchInBlueprintsIndexStatus::GetDescription() const
{
	return TEXT("Check Find-in-Blueprints index readiness before bp_search. Returns {ready, unindexed_assets, "
		"uncached_assets, cache_progress, cache_in_progress, asset_discovery_in_progress, "
		"first_search_may_index_full_corpus}. CAVEAT: ready=true does NOT bound the first search's cost; "
		"first_search_may_index_full_corpus=true means the next bp_search may index the whole corpus. "
		"Stateless / read-only.");
}

TSharedPtr<FJsonObject> ClaireonTool_SearchInBlueprintsIndexStatus::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	return Builder.Build();
}

IClaireonTool::FToolResult ClaireonTool_SearchInBlueprintsIndexStatus::Execute(const TSharedPtr<FJsonObject>& /*Arguments*/)
{
	FFindInBlueprintSearchManager& Mgr = FFindInBlueprintSearchManager::Get();

	const int32 UnindexedAssets = Mgr.GetNumberUnindexedAssets();
	const int32 UncachedAssets = Mgr.GetNumberUncachedAssets();
	const bool bCacheInProgress = Mgr.IsCacheInProgress();
	const bool bAssetDiscoveryInProgress = Mgr.IsAssetDiscoveryInProgress();
	const float CacheProgress = Mgr.GetCacheProgress();

	// "Ready" means: no async cache running, no asset discovery pending, no unindexed
	// or uncached assets queued. Callers that just want a green light should check
	// ready==true; if false, unindexed/uncached counts and cache_progress show
	// partial state.
	//
	// LIMITATION: the counters above do NOT see the manager's deferred-indexing
	// backlog (AssetsToIndexOnFirstSearch / PendingAssets are private with no
	// accessor), which is only flushed once the first search runs. ready==true
	// therefore does not bound the cost of the first bp_search this session; we
	// surface that as first_search_may_index_full_corpus below.
	const bool bReady = !bCacheInProgress && !bAssetDiscoveryInProgress
		&& UnindexedAssets == 0 && UncachedAssets == 0;

	const bool bFirstSearchMayIndexFullCorpus = !ClaireonSearchWait::HasAnySearchCompletedThisSession();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("ready"), bReady);
	Data->SetNumberField(TEXT("unindexed_assets"), UnindexedAssets);
	Data->SetNumberField(TEXT("uncached_assets"), UncachedAssets);
	Data->SetNumberField(TEXT("cache_progress"), CacheProgress);
	Data->SetBoolField(TEXT("cache_in_progress"), bCacheInProgress);
	Data->SetBoolField(TEXT("asset_discovery_in_progress"), bAssetDiscoveryInProgress);
	Data->SetBoolField(TEXT("first_search_may_index_full_corpus"), bFirstSearchMayIndexFullCorpus);

	FString Summary;
	if (bReady)
	{
		Summary = TEXT("FiB index ready (no pending cache or unindexed assets).");
		if (bFirstSearchMayIndexFullCorpus)
		{
			Summary += TEXT(" NOTE: no bp_search has completed this session; the first search may still trigger deferred indexing of the full corpus (ready does not bound its cost).");
		}
	}
	else
	{
		Summary = FString::Printf(
			TEXT("FiB index NOT ready -- unindexed=%d uncached=%d cache_in_progress=%s discovery_in_progress=%s progress=%.0f%%."),
			UnindexedAssets, UncachedAssets,
			bCacheInProgress ? TEXT("yes") : TEXT("no"),
			bAssetDiscoveryInProgress ? TEXT("yes") : TEXT("no"),
			CacheProgress * 100.0f);
	}

	return MakeSuccessResult(Data, Summary);
}
