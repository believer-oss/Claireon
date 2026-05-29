// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_WPGenerateStreaming.h"

#include "Tools/FToolSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UObjectIterator.h"

#if WITH_EDITOR
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionRuntimeHash.h"
#include "WorldPartition/WorldPartitionRuntimeCell.h"
#include "WorldPartition/WorldPartitionRuntimeLevelStreamingCell.h"
#include "WorldPartition/ErrorHandling/WorldPartitionStreamingGenerationLogErrorHandler.h"
#endif

FString ClaireonTool_WPGenerateStreaming::GetDescription() const
{
	return TEXT(
		"Runs UWorldPartition::GenerateStreaming on the editor world's WorldPartition, "
		"then walks the resulting runtime cells and returns their DataLayers + actor "
		"package lists. In editor (non-PIE) mode, RuntimeStreamingData is normally empty "
		"-- this tool triggers a one-shot build via the same code path cook uses so the "
		"cell layout can be inspected without launching PIE. Pairs with wp_actor_desc_inspect: "
		"descs answer 'is the actor tagged?', this tool answers 'did the cell builder "
		"honor the tag, or did the actor land in an empty-DataLayers always-loaded cell?'. "
		"FlushStreaming() is called automatically at the end so the editor returns to its "
		"normal state. Use actor_filter to find which cell(s) contain a particular actor "
		"by package-name substring (e.g. 'BP_Onboarding_FlowManager' or a uasset GUID).");
}

TArray<FString> ClaireonTool_WPGenerateStreaming::GetSearchKeywords() const
{
	return {
		TEXT("world-partition"),
		TEXT("runtime-cell"),
		TEXT("generate-streaming"),
		TEXT("cell-layout"),
		TEXT("data-layer"),
		TEXT("DataLayers"),
		TEXT("cook"),
		TEXT("always-loaded"),
		TEXT("empty-data-layers"),
		TEXT("ForEachStreamingCells"),
	};
}

TSharedPtr<FJsonObject> ClaireonTool_WPGenerateStreaming::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("actor_filter"),
		TEXT("Substring matched (case-insensitive) against each cell's package paths. "
		     "Cells whose package list contains a match are returned with their full "
		     "DataLayers + a list of matching packages. Leave empty to summarize ALL cells "
		     "(may be large)."));
	S.AddString(TEXT("world_name_filter"),
		TEXT("Optional: substring matched against the world's package name to select "
		     "which UWorldPartition to inspect. Default: first found."));
	S.AddBoolean(TEXT("include_full_package_list"),
		TEXT("When true, each matching cell includes all its package paths (not just the "
		     "ones that matched actor_filter). Default false to keep responses small."));
	S.AddInteger(TEXT("max_packages_per_cell"),
		TEXT("Max number of package entries returned per cell when include_full_package_list "
		     "is true. Default 50."));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_WPGenerateStreaming::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
#if !WITH_EDITOR
	return MakeErrorResult(TEXT("wp.generate_streaming is editor-only."));
#else
	FString ActorFilter;
	Arguments->TryGetStringField(TEXT("actor_filter"), ActorFilter);

	FString WorldNameFilter;
	Arguments->TryGetStringField(TEXT("world_name_filter"), WorldNameFilter);

	bool bIncludeFullPackages = false;
	Arguments->TryGetBoolField(TEXT("include_full_package_list"), bIncludeFullPackages);

	int32 MaxPackagesPerCell = 50;
	Arguments->TryGetNumberField(TEXT("max_packages_per_cell"), MaxPackagesPerCell);
	if (MaxPackagesPerCell <= 0) { MaxPackagesPerCell = 50; }

	// Find a matching UWorldPartition (same iteration as wp_actor_desc_inspect)
	UWorldPartition* TargetWP = nullptr;
	for (TObjectIterator<UWorldPartition> It; It; ++It)
	{
		UWorldPartition* WP = *It;
		if (!IsValid(WP) || WP->IsTemplate())
		{
			continue;
		}
		if (!WorldNameFilter.IsEmpty())
		{
			const FString WPPath = WP->GetPackage() ? WP->GetPackage()->GetName() : WP->GetName();
			if (!WPPath.Contains(WorldNameFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}
		TargetWP = WP;
		break;
	}

	if (!TargetWP)
	{
		return MakeErrorResult(WorldNameFilter.IsEmpty()
			? TEXT("No UWorldPartition found in loaded objects.")
			: FString::Printf(TEXT("No UWorldPartition matching world_name_filter '%s'."), *WorldNameFilter));
	}

	// Refuse to run in PIE -- the streaming policy is already active and re-running
	// GenerateStreaming would corrupt the game world. Caller should exit PIE first.
	if (!TargetWP->CanGenerateStreaming())
	{
		return MakeErrorResult(TEXT(
			"UWorldPartition::CanGenerateStreaming() returned false (streaming policy is "
			"already active -- likely in PIE). Exit PIE before calling this tool."));
	}

	// Run GenerateStreaming. Use log-style error handler so errors land in Saved/Logs/
	// rather than popping the map check dialog.
	FStreamingGenerationLogErrorHandler LogErrorHandler;
	UWorldPartition::FGenerateStreamingParams Params = UWorldPartition::FGenerateStreamingParams()
		.SetErrorHandler(&LogErrorHandler);
	UWorldPartition::FGenerateStreamingContext Context = UWorldPartition::FGenerateStreamingContext();

	const bool bGenerated = TargetWP->GenerateStreaming(Params, Context);
	if (!bGenerated)
	{
		return MakeErrorResult(TEXT(
			"UWorldPartition::GenerateStreaming returned false. Check Saved/Logs/ for "
			"streaming-generation errors."));
	}

	// Walk runtime cells. Use the RuntimeHash's ForEachStreamingCells interface so
	// we work with both WorldPartitionRuntimeHashSet and WorldPartitionRuntimeSpatialHash.
	struct FCellSummary
	{
		FString CellName;
		FString CellPath;
		TArray<FName> DataLayers;
		FName ExternalDataLayer;
		bool bIsClientOnlyVisible = false;
		FGuid ContentBundleID;
		bool bHasContentBundle = false;
		TArray<FString> Packages;
		TArray<FString> MatchedPackages;
	};
	TArray<FCellSummary> Cells;

	if (UWorldPartitionRuntimeHash* RuntimeHash = TargetWP->RuntimeHash)
	{
		RuntimeHash->ForEachStreamingCells([&Cells, &ActorFilter, MaxPackagesPerCell, bIncludeFullPackages]
			(const UWorldPartitionRuntimeCell* Cell) -> bool
		{
			if (!Cell)
			{
				return true;
			}

			FCellSummary Summary;
			Summary.CellName = Cell->GetName();
			Summary.CellPath = Cell->GetPathName();
			Summary.DataLayers = Cell->GetDataLayers();
			Summary.ExternalDataLayer = Cell->GetExternalDataLayer();
			Summary.bIsClientOnlyVisible = Cell->GetClientOnlyVisible();
			Summary.ContentBundleID = Cell->GetContentBundleID();
			Summary.bHasContentBundle = Cell->HasContentBundle();

			// Only level-streaming cells expose their full Packages array
			if (const UWorldPartitionRuntimeLevelStreamingCell* LSCell =
				Cast<UWorldPartitionRuntimeLevelStreamingCell>(Cell))
			{
				const TArray<FWorldPartitionRuntimeCellObjectMapping>& CellPkgs = LSCell->GetPackages();
				bool bAnyMatched = false;
				for (const FWorldPartitionRuntimeCellObjectMapping& Pkg : CellPkgs)
				{
					const FString PkgStr = Pkg.Package.ToString();
					const FString PathStr = Pkg.Path.ToString();
					const bool bMatched = !ActorFilter.IsEmpty() &&
						(PkgStr.Contains(ActorFilter, ESearchCase::IgnoreCase) ||
						 PathStr.Contains(ActorFilter, ESearchCase::IgnoreCase));
					if (bMatched)
					{
						Summary.MatchedPackages.Add(FString::Printf(TEXT("%s -> %s"),
							*PkgStr, *PathStr));
						bAnyMatched = true;
					}
					if (bIncludeFullPackages && Summary.Packages.Num() < MaxPackagesPerCell)
					{
						Summary.Packages.Add(PkgStr);
					}
				}

				// Filtering: when ActorFilter is non-empty, only keep cells that matched
				if (!ActorFilter.IsEmpty() && !bAnyMatched)
				{
					return true; // skip
				}
			}
			else if (!ActorFilter.IsEmpty())
			{
				// Non-level-streaming cell types don't expose Packages -- can't filter
				return true;
			}

			Cells.Add(MoveTemp(Summary));
			return true;
		});
	}

	// Clean up: flush so the editor returns to a streaming-free state
	TargetWP->FlushStreaming();

	// Build JSON
	TArray<TSharedPtr<FJsonValue>> CellsJson;
	for (const FCellSummary& Cell : Cells)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("cell_name"), Cell.CellName);
		Entry->SetStringField(TEXT("cell_path"), Cell.CellPath);
		Entry->SetBoolField(TEXT("is_client_only_visible"), Cell.bIsClientOnlyVisible);
		Entry->SetBoolField(TEXT("has_content_bundle"), Cell.bHasContentBundle);
		if (Cell.bHasContentBundle)
		{
			Entry->SetStringField(TEXT("content_bundle_id"), Cell.ContentBundleID.ToString());
		}
		Entry->SetStringField(TEXT("external_data_layer"),
			Cell.ExternalDataLayer.IsNone() ? FString() : Cell.ExternalDataLayer.ToString());

		TArray<TSharedPtr<FJsonValue>> DLs;
		for (const FName& DL : Cell.DataLayers)
		{
			DLs.Add(MakeShared<FJsonValueString>(DL.ToString()));
		}
		Entry->SetArrayField(TEXT("data_layers"), DLs);
		Entry->SetNumberField(TEXT("data_layer_count"), Cell.DataLayers.Num());

		if (!Cell.MatchedPackages.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> Matched;
			for (const FString& M : Cell.MatchedPackages)
			{
				Matched.Add(MakeShared<FJsonValueString>(M));
			}
			Entry->SetArrayField(TEXT("matched_packages"), Matched);
		}
		if (!Cell.Packages.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> Pkgs;
			for (const FString& P : Cell.Packages)
			{
				Pkgs.Add(MakeShared<FJsonValueString>(P));
			}
			Entry->SetArrayField(TEXT("packages"), Pkgs);
		}

		CellsJson.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("world_partition_path"), TargetWP->GetPathName());
	Data->SetNumberField(TEXT("cell_count"), Cells.Num());
	Data->SetArrayField(TEXT("cells"), CellsJson);

	const FString Summary = ActorFilter.IsEmpty()
		? FString::Printf(TEXT("wp.generate_streaming: %d cells in runtime hash"), Cells.Num())
		: FString::Printf(TEXT("wp.generate_streaming: %d cells matched actor_filter '%s'"),
			Cells.Num(), *ActorFilter);

	return MakeSuccessResult(Data, Summary);
#endif
}
