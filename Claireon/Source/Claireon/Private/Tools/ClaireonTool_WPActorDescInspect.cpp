// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_WPActorDescInspect.h"

#include "Tools/FToolSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Crc.h"
#include "UObject/UObjectIterator.h"

#if WITH_EDITOR
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/ActorDescContainerInstance.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionActorDesc.h"
#endif

FString ClaireonTool_WPActorDescInspect::GetDescription() const
{
	return TEXT(
		"Inspects FWorldPartitionActorDesc entries from the in-memory descriptor list "
		"of UWorldPartition. Returns fields not visible via uobject_inspect: "
		"bIsUsingDataLayerAsset, raw DataLayers (FName array as stored on disk), "
		"HasResolvedDataLayerInstanceNames, resolved instance names, and the computed "
		"DataLayersID hash (0 = no-layer cell = always loaded). "
		"Primary use: diagnose actors loaded despite their data layer being Unloaded. "
		"Filter by base_class_filter (substring match on Blueprint class asset name) "
		"and/or actor_guid (exact FGuid string, e.g. 'AABBCCDD-EEFF0011-...').");
}

TArray<FString> ClaireonTool_WPActorDescInspect::GetSearchKeywords() const
{
	return {
		TEXT("world-partition"),
		TEXT("actor-descriptor"),
		TEXT("data-layer"),
		TEXT("streaming"),
		TEXT("bIsUsingDataLayerAsset"),
		TEXT("DataLayersID"),
		TEXT("always-loaded"),
		TEXT("cell"),
		TEXT("descriptor"),
	};
}

TSharedPtr<FJsonObject> ClaireonTool_WPActorDescInspect::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("base_class_filter"),
		TEXT("Substring matched (case-insensitive) against the actor's blueprint "
		     "base class asset name (e.g. 'FlowManager' matches "
		     "'BP_Onboarding_FlowManager_TS_MAL_C'). Leave empty to match all."));
	S.AddString(TEXT("actor_guid"),
		TEXT("Optional: exact FGuid string (e.g. 'CB434576-42126D23-037648AC-5FAF5E7B'). "
		     "When provided, only descriptors with this GUID are returned."));
	S.AddString(TEXT("world_name_filter"),
		TEXT("Optional: substring matched against the world's package name to select "
		     "which UWorldPartition to inspect. Default: first found."));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_WPActorDescInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
#if !WITH_EDITOR
	return MakeErrorResult(TEXT("wp.actor_desc_inspect is editor-only."));
#else
	FString BaseClassFilter;
	Arguments->TryGetStringField(TEXT("base_class_filter"), BaseClassFilter);

	FString ActorGuidStr;
	Arguments->TryGetStringField(TEXT("actor_guid"), ActorGuidStr);

	FString WorldNameFilter;
	Arguments->TryGetStringField(TEXT("world_name_filter"), WorldNameFilter);

	// Parse GUID filter if provided
	FGuid GuidFilter;
	bool bHasGuidFilter = false;
	if (!ActorGuidStr.IsEmpty())
	{
		if (!FGuid::Parse(ActorGuidStr, GuidFilter))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Could not parse actor_guid '%s'. Expected FGuid format e.g. 'AABBCCDD-11223344-AABB1122-CCDD3344'."),
				*ActorGuidStr));
		}
		bHasGuidFilter = true;
	}

	// Find a matching UWorldPartition
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

	// Collect matching descriptors from all container instances
	TArray<TSharedPtr<FJsonValue>> Results;
	int32 TotalScanned = 0;

	auto ProcessContainer = [&](UActorDescContainerInstance* ContainerInstance)
	{
		if (!ContainerInstance)
		{
			return;
		}

		for (UActorDescContainerInstance::TConstIterator<> It(ContainerInstance); It; ++It)
		{
			++TotalScanned;
			const FWorldPartitionActorDescInstance* DescInstance = *It;
			if (!DescInstance)
			{
				continue;
			}

			// GUID filter
			if (bHasGuidFilter && DescInstance->GetGuid() != GuidFilter)
			{
				continue;
			}

			// Base class filter: match against the asset name of the Blueprint base class
			if (!BaseClassFilter.IsEmpty())
			{
				const FString BaseClassName = DescInstance->GetBaseClass().GetAssetName().ToString();
				const FString NativeClassName = DescInstance->GetNativeClass().GetAssetName().ToString();
				const FString LabelStr = DescInstance->GetActorLabel().ToString();
				const FString PackageStr = DescInstance->GetActorPackage().ToString();
				const bool bMatches = BaseClassName.Contains(BaseClassFilter, ESearchCase::IgnoreCase)
					|| NativeClassName.Contains(BaseClassFilter, ESearchCase::IgnoreCase)
					|| LabelStr.Contains(BaseClassFilter, ESearchCase::IgnoreCase)
					|| PackageStr.Contains(BaseClassFilter, ESearchCase::IgnoreCase);
				if (!bMatches)
				{
					continue;
				}
			}

			// Build result entry
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("guid"), DescInstance->GetGuid().ToString());
			Entry->SetStringField(TEXT("actor_package"), DescInstance->GetActorPackage().ToString());
			Entry->SetStringField(TEXT("actor_label"), DescInstance->GetActorLabel().ToString());
			Entry->SetStringField(TEXT("base_class"), DescInstance->GetBaseClass().ToString());
			Entry->SetStringField(TEXT("native_class"), DescInstance->GetNativeClass().ToString());
			Entry->SetBoolField(TEXT("is_spatially_loaded"), DescInstance->GetIsSpatiallyLoaded());

			// The critical diagnostic fields (via the raw desc)
			const FWorldPartitionActorDesc* Desc = DescInstance->GetActorDesc();
			Entry->SetBoolField(TEXT("is_using_data_layer_asset"), Desc ? Desc->IsUsingDataLayerAsset() : false);

			// Raw data layers stored in the descriptor (asset paths or legacy instance names
			// depending on bIsUsingDataLayerAsset)
			{
				TArray<TSharedPtr<FJsonValue>> RawLayers;
				if (Desc)
				{
					for (const FName& LayerName : Desc->GetDataLayers())
					{
						RawLayers.Add(MakeShared<FJsonValueString>(LayerName.ToString()));
					}
				}
				Entry->SetArrayField(TEXT("raw_data_layers"), RawLayers);
			}

			// Resolved instance names (set by ResolveDataLayerInstanceNames during WP
			// streaming generation; empty = no-layer cell = always loaded)
			const bool bResolved = DescInstance->HasResolvedDataLayerInstanceNames();
			Entry->SetBoolField(TEXT("has_resolved_data_layer_names"), bResolved);

			TArray<TSharedPtr<FJsonValue>> ResolvedLayers;
			uint32 ComputedHash = 0;

			if (bResolved)
			{
				const FDataLayerInstanceNames& ResolvedNames = DescInstance->GetDataLayerInstanceNames();
				TArray<FName> NamesCopy = ResolvedNames.ToArray();
				for (const FName& Name : NamesCopy)
				{
					ResolvedLayers.Add(MakeShared<FJsonValueString>(Name.ToString()));
				}

				// Replicate FDataLayersID: CRC32 of sorted names
				// (resolved names are already runtime-only; see SetRuntimeDataLayerInstanceNames)
				NamesCopy.Sort([](const FName& A, const FName& B)
				{
					return A.ToString() < B.ToString();
				});
				for (const FName& Name : NamesCopy)
				{
					ComputedHash = FCrc::StrCrc32(*Name.ToString(), ComputedHash);
				}
			}

			Entry->SetArrayField(TEXT("resolved_data_layer_names"), ResolvedLayers);
			// 0 = no data layers = always-loaded cell (the bug signature)
			Entry->SetNumberField(TEXT("computed_data_layers_id_hash"), (double)ComputedHash);

			Results.Add(MakeShared<FJsonValueObject>(Entry));
		}
	};

	TargetWP->ForEachActorDescContainerInstance([&ProcessContainer](UActorDescContainerInstance* ContainerInstance)
	{
		ProcessContainer(ContainerInstance);
	}, /*bRecursive=*/true);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("world_partition_path"), TargetWP->GetPathName());
	Data->SetNumberField(TEXT("total_scanned"), TotalScanned);
	Data->SetArrayField(TEXT("descriptors"), Results);

	const FString Summary = FString::Printf(
		TEXT("wp.actor_desc_inspect: %d match%s out of %d scanned descriptor%s"),
		Results.Num(),
		Results.Num() == 1 ? TEXT("") : TEXT("es"),
		TotalScanned,
		TotalScanned == 1 ? TEXT("") : TEXT("s"));

	return MakeSuccessResult(Data, Summary);
#endif
}
