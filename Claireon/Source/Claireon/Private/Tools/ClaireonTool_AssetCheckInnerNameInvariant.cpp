// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AssetCheckInnerNameInvariant.h"
#include "Tools/FToolSchemaBuilder.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"

namespace
{
	// AssetCheckInnerNameInvariant_*: discriminator-prefixed file-local
	// helpers to avoid unity-batch collisions with similarly-named
	// helpers across cohort files (see MEMORY.md
	// feedback_anon_namespace_unity_collision.md).

	static constexpr int32 AssetCheckInnerNameInvariant_MaxResults = 500;
}

FString ClaireonTool_AssetCheckInnerNameInvariant::GetCategory() const
{
	return TEXT("asset");
}

FString ClaireonTool_AssetCheckInnerNameInvariant::GetOperation() const
{
	return TEXT("check_inner_name_invariant");
}

FString ClaireonTool_AssetCheckInnerNameInvariant::GetDescription() const
{
	return TEXT("Stateless read-only audit: walks the asset registry and reports packages whose on-disk short-name disagrees with the inner top-level UObject's name. Optional contentPath (default /Game) and includePlugins (default false). Returns {scanned_count, mismatch_count, elapsed_ms, mismatches[], truncated}.");
}

TSharedPtr<FJsonObject> ClaireonTool_AssetCheckInnerNameInvariant::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(
		TEXT("contentPath"),
		TEXT("Unreal content path to scan under (default: /Game). E.g. /Game/Characters."),
		/*bRequired=*/ false);
	Builder.AddBoolean(
		TEXT("includePlugins"),
		TEXT("Include assets from plugin content directories (default: false). When false and contentPath starts with /Game, plugin-prefixed package paths are filtered out."),
		/*bRequired=*/ false);
	return Builder.Build();
}

IClaireonTool::FToolResult ClaireonTool_AssetCheckInnerNameInvariant::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Parse optional parameters (mirrors ClaireonTool_AssetList::Execute)
	FString ContentPath = TEXT("/Game");
	if (Arguments.IsValid() && Arguments->HasField(TEXT("contentPath")))
	{
		FString ProvidedPath;
		if (Arguments->TryGetStringField(TEXT("contentPath"), ProvidedPath) && !ProvidedPath.IsEmpty())
		{
			ContentPath = ProvidedPath;
		}
	}

	bool bIncludePlugins = false;
	if (Arguments.IsValid() && Arguments->HasField(TEXT("includePlugins")))
	{
		bIncludePlugins = Arguments->GetBoolField(TEXT("includePlugins"));
	}

	// Acquire the asset registry
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Build the filter. bIncludeOnlyOnDiskAssets=true because the bug
	// class only manifests on disk; in-memory-only registry entries are
	// irrelevant to the invariant.
	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;
	Filter.bIncludeOnlyOnDiskAssets = true;
	if (!ContentPath.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*ContentPath));
	}

	const double StartSeconds = FPlatformTime::Seconds();

	TArray<FAssetData> AllAssets;
	AssetRegistry.GetAssets(Filter, AllAssets);

	TArray<TSharedPtr<FJsonValue>> Mismatches;
	int32 ScannedCount = 0;
	bool bTruncated = false;

	for (const FAssetData& Asset : AllAssets)
	{
		// Skip redirector entries (artifacts of asset moves)
		if (Asset.IsRedirector())
		{
			continue;
		}

		const FString PackagePath = Asset.PackageName.ToString();

		// Mirror ClaireonTool_AssetList's includePlugins semantics:
		// when !bIncludePlugins and ContentPath was /Game-rooted, drop
		// entries that resolve outside /Game.
		if (!bIncludePlugins && !PackagePath.StartsWith(TEXT("/Game")))
		{
			if (ContentPath.StartsWith(TEXT("/Game")))
			{
				continue;
			}
		}

		// Skip entries with empty / None inner names (still count under
		// ScannedCount for transparency per PROPOSAL "Error / edge cases").
		const FString InnerName = Asset.AssetName.ToString();
		++ScannedCount;
		if (Asset.AssetName.IsNone() || InnerName.IsEmpty())
		{
			continue;
		}

		const FString PackageShortName = FPackageName::GetShortName(PackagePath);
		if (InnerName == PackageShortName)
		{
			continue;
		}

		TSharedPtr<FJsonObject> MismatchObj = MakeShared<FJsonObject>();
		MismatchObj->SetStringField(TEXT("package_path"), PackagePath);
		MismatchObj->SetStringField(TEXT("package_short_name"), PackageShortName);
		MismatchObj->SetStringField(TEXT("inner_name"), InnerName);
		MismatchObj->SetStringField(TEXT("asset_class"), Asset.AssetClassPath.GetAssetName().ToString());

		TOptional<FAssetPackageData> PackageData = AssetRegistry.GetAssetPackageDataCopy(Asset.PackageName);
		if (PackageData.IsSet())
		{
			MismatchObj->SetNumberField(TEXT("disk_size_bytes"), static_cast<double>(PackageData->DiskSize));
		}

		Mismatches.Add(MakeShared<FJsonValueObject>(MismatchObj));

		if (Mismatches.Num() >= AssetCheckInnerNameInvariant_MaxResults)
		{
			bTruncated = true;
			break;
		}
	}

	const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;
	const int32 ElapsedMs = static_cast<int32>(ElapsedSeconds * 1000.0);
	const int32 MismatchCount = Mismatches.Num();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("mismatch_count"), MismatchCount);
	Data->SetNumberField(TEXT("scanned_count"), ScannedCount);
	Data->SetNumberField(TEXT("elapsed_ms"), ElapsedMs);
	Data->SetArrayField(TEXT("mismatches"), Mismatches);
	Data->SetBoolField(TEXT("truncated"), bTruncated);

	const FString Summary = FString::Printf(
		TEXT("Scanned %d assets in %d ms; found %d mismatch%s%s"),
		ScannedCount,
		ElapsedMs,
		MismatchCount,
		MismatchCount == 1 ? TEXT("") : TEXT("es"),
		bTruncated ? TEXT(" (capped at 500)") : TEXT(""));

	return IClaireonTool::MakeSuccessResult(Data, Summary);
}
