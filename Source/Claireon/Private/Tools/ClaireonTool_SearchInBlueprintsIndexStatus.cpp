// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_SearchInBlueprintsIndexStatus.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h" // kBPCategory
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLog.h"
#include "FindInBlueprintManager.h"
#include "Dom/JsonObject.h"

FString ClaireonTool_SearchInBlueprintsIndexStatus::GetCategory() const { return kBPCategory; }
FString ClaireonTool_SearchInBlueprintsIndexStatus::GetOperation() const { return TEXT("search_index_status"); }

TArray<FString> ClaireonTool_SearchInBlueprintsIndexStatus::GetSearchKeywords() const
{
	return {TEXT("bp"), TEXT("blueprint"), TEXT("search"), TEXT("index"), TEXT("fib"), TEXT("status"), TEXT("ready"), TEXT("indexed"), TEXT("cache")};
}

FString ClaireonTool_SearchInBlueprintsIndexStatus::GetDescription() const
{
	return TEXT("Probe Find-in-Blueprints (FiB) index readiness. Use this before bp_search when the index might still be building after editor startup; the bp_search tool blocks on the index and can timeout twice if you do not check first. Stateless / read-only / non-session. Returns {ready, indexed_assets, unindexed_assets, uncached_assets, cache_progress, asset_discovery_in_progress}.");
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
	// ready==true; if false, indexed_assets / cache_progress show partial state.
	const bool bReady = !bCacheInProgress && !bAssetDiscoveryInProgress
		&& UnindexedAssets == 0 && UncachedAssets == 0;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("ready"), bReady);
	Data->SetNumberField(TEXT("unindexed_assets"), UnindexedAssets);
	Data->SetNumberField(TEXT("uncached_assets"), UncachedAssets);
	Data->SetNumberField(TEXT("cache_progress"), CacheProgress);
	Data->SetBoolField(TEXT("cache_in_progress"), bCacheInProgress);
	Data->SetBoolField(TEXT("asset_discovery_in_progress"), bAssetDiscoveryInProgress);

	FString Summary;
	if (bReady)
	{
		Summary = TEXT("FiB index ready (no pending cache or unindexed assets).");
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
