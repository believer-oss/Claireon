// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AssetList.h"
#include "ClaireonLog.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

FString ClaireonTool_AssetList::GetName() const
{
	return TEXT("claireon.asset_list");
}

FString ClaireonTool_AssetList::GetDescription() const
{
	return TEXT("List assets with filtering by path, extension, and pattern. Returns Unreal paths, Windows paths, class names, and sizes.");
}

TSharedPtr<FJsonObject> ClaireonTool_AssetList::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// contentPath - optional
	TSharedPtr<FJsonObject> ContentPathProp = MakeShared<FJsonObject>();
	ContentPathProp->SetStringField(TEXT("type"), TEXT("string"));
	ContentPathProp->SetStringField(TEXT("description"),
		TEXT("Unreal content path to search under (default: /Game). E.g. /Game/Characters"));
	Properties->SetObjectField(TEXT("contentPath"), ContentPathProp);

	// extension - optional
	TSharedPtr<FJsonObject> ExtensionProp = MakeShared<FJsonObject>();
	ExtensionProp->SetStringField(TEXT("type"), TEXT("string"));
	ExtensionProp->SetStringField(TEXT("description"),
		TEXT("Filter by asset class name (e.g. Blueprint, StaticMesh, Material, World, Texture2D)"));
	Properties->SetObjectField(TEXT("extension"), ExtensionProp);

	// pattern - optional
	TSharedPtr<FJsonObject> PatternProp = MakeShared<FJsonObject>();
	PatternProp->SetStringField(TEXT("type"), TEXT("string"));
	PatternProp->SetStringField(TEXT("description"),
		TEXT("Wildcard pattern to match against asset names (e.g. BP_Player*, *Character*, DA_*)"));
	Properties->SetObjectField(TEXT("pattern"), PatternProp);

	// includePlugins - optional
	TSharedPtr<FJsonObject> IncludePluginsProp = MakeShared<FJsonObject>();
	IncludePluginsProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludePluginsProp->SetStringField(TEXT("description"),
		TEXT("Include assets from plugin content directories (default: false). When false, only /Game/ assets are listed."));
	Properties->SetObjectField(TEXT("includePlugins"), IncludePluginsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_AssetList::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Parse optional parameters
	FString ContentPath = TEXT("/Game");
	if (Arguments.IsValid() && Arguments->HasField(TEXT("contentPath")))
	{
		FString ProvidedPath;
		if (Arguments->TryGetStringField(TEXT("contentPath"), ProvidedPath) && !ProvidedPath.IsEmpty())
		{
			ContentPath = ProvidedPath;
		}
	}

	FString Extension;
	if (Arguments.IsValid())
	{
		Arguments->TryGetStringField(TEXT("extension"), Extension);
	}

	FString Pattern;
	if (Arguments.IsValid())
	{
		Arguments->TryGetStringField(TEXT("pattern"), Pattern);
	}

	bool bIncludePlugins = false;
	if (Arguments.IsValid() && Arguments->HasField(TEXT("includePlugins")))
	{
		bIncludePlugins = Arguments->GetBoolField(TEXT("includePlugins"));
	}

	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.asset.list: contentPath=%s, extension=%s, pattern=%s, includePlugins=%d"),
		*ContentPath, *Extension, *Pattern, bIncludePlugins);

	// Get Asset Registry
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Build filter
	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;

	if (!ContentPath.IsEmpty())
	{
		Filter.PackagePaths.Add(FName(*ContentPath));
	}

	// If extension is specified, treat it as a class filter
	if (!Extension.IsEmpty())
	{
		// Try common module paths for the class name
		Filter.ClassPaths.Add(FTopLevelAssetPath(FString::Printf(TEXT("/Script/Engine.%s"), *Extension)));
		Filter.ClassPaths.Add(FTopLevelAssetPath(FString::Printf(TEXT("/Script/CoreUObject.%s"), *Extension)));
		Filter.ClassPaths.Add(FTopLevelAssetPath(FString::Printf(TEXT("/Script/Niagara.%s"), *Extension)));
		Filter.ClassPaths.Add(FTopLevelAssetPath(FString::Printf(TEXT("/Script/Paper2D.%s"), *Extension)));
		Filter.ClassPaths.Add(FTopLevelAssetPath(FString::Printf(TEXT("/Script/UMG.%s"), *Extension)));
	}

	TArray<FAssetData> AllAssets;
	AssetRegistry.GetAssets(Filter, AllAssets);

	// Apply additional filters
	TArray<FAssetData> FilteredAssets;
	static constexpr int32 MaxResults = 500;
	int32 RedirectorsSkipped = 0;

	for (const FAssetData& Asset : AllAssets)
	{
		// Skip redirector assets -- they are artifacts of asset moves, not real content
		if (Asset.IsRedirector())
		{
			++RedirectorsSkipped;
			continue;
		}

		FString PackagePath = Asset.PackageName.ToString();

		// Filter out plugin assets if not requested
		if (!bIncludePlugins && !PackagePath.StartsWith(TEXT("/Game")))
		{
			// If contentPath was explicitly set to a non-/Game path, allow it
			if (ContentPath.StartsWith(TEXT("/Game")))
			{
				continue;
			}
		}

		// Apply wildcard pattern filter against asset name
		if (!Pattern.IsEmpty())
		{
			FString AssetName = Asset.AssetName.ToString();
			if (!AssetName.MatchesWildcard(Pattern, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		FilteredAssets.Add(Asset);

		if (FilteredAssets.Num() >= MaxResults)
		{
			break;
		}
	}

	// Build output
	const bool bTruncated = (FilteredAssets.Num() >= MaxResults) && (AllAssets.Num() > MaxResults);

	FString Output;
	Output += FString::Printf(TEXT("Asset List: %s\n"), *ContentPath);
	if (!Extension.IsEmpty())
	{
		Output += FString::Printf(TEXT("Class Filter: %s\n"), *Extension);
	}
	if (!Pattern.IsEmpty())
	{
		Output += FString::Printf(TEXT("Pattern: %s\n"), *Pattern);
	}
	Output += FString::Printf(TEXT("Assets Found: %d"), FilteredAssets.Num());
	if (bTruncated)
	{
		Output += FString::Printf(TEXT(" (capped at %d)"), MaxResults);
	}
	if (RedirectorsSkipped > 0)
	{
		Output += FString::Printf(TEXT("\nRedirectors Hidden: %d"), RedirectorsSkipped);
	}
	Output += TEXT("\n\n");

	if (FilteredAssets.Num() == 0)
	{
		Output += TEXT("No assets found matching the specified criteria.\n");
	}
	else
	{
		for (int32 i = 0; i < FilteredAssets.Num(); i++)
		{
			const FAssetData& Asset = FilteredAssets[i];
			FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
			FString UnrealPath = Asset.PackageName.ToString();

			// Get disk size
			FString SizeStr = TEXT("?");
			TOptional<FAssetPackageData> PackageData = AssetRegistry.GetAssetPackageDataCopy(Asset.PackageName);
			if (PackageData.IsSet())
			{
				SizeStr = FormatDiskSize(PackageData->DiskSize);
			}

			// Get Windows path
			FString WindowsPath;
			if (FPackageName::TryConvertLongPackageNameToFilename(UnrealPath, WindowsPath, FPackageName::GetAssetPackageExtension()))
			{
				WindowsPath = FPaths::ConvertRelativePathToFull(WindowsPath);
			}
			else
			{
				WindowsPath = TEXT("?");
			}

			Output += FString::Printf(TEXT("%d. %s\n"), i + 1, *UnrealPath);
			Output += FString::Printf(TEXT("   Class: %s | Size: %s\n"), *ClassName, *SizeStr);
			Output += FString::Printf(TEXT("   Windows: %s\n"), *WindowsPath);
		}
	}

	return MakeSuccessResult(nullptr, Output);
}

FString ClaireonTool_AssetList::FormatDiskSize(int64 SizeBytes)
{
	if (SizeBytes < 0)
	{
		return TEXT("?");
	}
	if (SizeBytes < 1024)
	{
		return FString::Printf(TEXT("%lld B"), SizeBytes);
	}
	if (SizeBytes < 1024 * 1024)
	{
		return FString::Printf(TEXT("%.1f KB"), SizeBytes / 1024.0);
	}
	if (SizeBytes < 1024 * 1024 * 1024)
	{
		return FString::Printf(TEXT("%.1f MB"), SizeBytes / (1024.0 * 1024.0));
	}
	return FString::Printf(TEXT("%.2f GB"), SizeBytes / (1024.0 * 1024.0 * 1024.0));
}
