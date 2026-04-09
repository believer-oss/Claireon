// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AssetReferences.h"
#include "ClaireonLog.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/PackageName.h"

FString ClaireonTool_AssetReferences::GetName() const
{
	return TEXT("claireon.asset_references");
}

FString ClaireonTool_AssetReferences::GetDescription() const
{
	return TEXT("Query asset dependencies (what it uses) and referencers (what uses it) from the Unreal Asset Registry. Supports hard and soft references with optional recursive traversal.");
}

TSharedPtr<FJsonObject> ClaireonTool_AssetReferences::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Full asset path (e.g. /Game/Characters/Player/BP_PlayerCharacter)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// include_soft_references - optional
	TSharedPtr<FJsonObject> SoftRefProp = MakeShared<FJsonObject>();
	SoftRefProp->SetStringField(TEXT("type"), TEXT("boolean"));
	SoftRefProp->SetStringField(TEXT("description"), TEXT("Include soft references in addition to hard references (default: true)"));
	Properties->SetObjectField(TEXT("include_soft_references"), SoftRefProp);

	// recursive - optional
	TSharedPtr<FJsonObject> RecursiveProp = MakeShared<FJsonObject>();
	RecursiveProp->SetStringField(TEXT("type"), TEXT("boolean"));
	RecursiveProp->SetStringField(TEXT("description"), TEXT("Recursively follow dependencies (default: false)"));
	Properties->SetObjectField(TEXT("recursive"), RecursiveProp);

	// depth_limit - optional
	TSharedPtr<FJsonObject> DepthProp = MakeShared<FJsonObject>();
	DepthProp->SetStringField(TEXT("type"), TEXT("integer"));
	DepthProp->SetStringField(TEXT("description"), TEXT("Maximum recursion depth when recursive=true (default: 5)"));
	Properties->SetObjectField(TEXT("depth_limit"), DepthProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_AssetReferences::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required argument: asset_path"));
	}

	bool bIncludeSoft = true;
	Arguments->TryGetBoolField(TEXT("include_soft_references"), bIncludeSoft);

	bool bRecursive = false;
	Arguments->TryGetBoolField(TEXT("recursive"), bRecursive);

	int32 DepthLimit = 5;
	Arguments->TryGetNumberField(TEXT("depth_limit"), DepthLimit);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Convert asset path to package name
	FName PackageName = FName(*AssetPath);
	FString PackageNameStr = AssetPath;
	// Strip asset name suffix if present (e.g. /Game/Foo.Foo -> /Game/Foo)
	int32 DotIndex;
	if (PackageNameStr.FindChar(TEXT('.'), DotIndex))
	{
		PackageNameStr = PackageNameStr.Left(DotIndex);
		PackageName = FName(*PackageNameStr);
	}

	// Validate the asset exists
	TOptional<FAssetPackageData> PackageData = AssetRegistry.GetAssetPackageDataCopy(PackageName);
	if (!PackageData.IsSet())
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset not found in registry: %s"), *AssetPath));
	}

	UE::AssetRegistry::EDependencyQuery QueryFlags = bIncludeSoft
		? UE::AssetRegistry::EDependencyQuery::NoRequirements
		: UE::AssetRegistry::EDependencyQuery::Hard;

	// Gather referencers (things that reference this asset)
	TArray<FName> ReferencerNames;
	AssetRegistry.GetReferencers(PackageName, ReferencerNames, UE::AssetRegistry::EDependencyCategory::Package, QueryFlags);

	// Gather dependencies (things this asset uses)
	TArray<FName> DependencyNames;
	if (bRecursive)
	{
		TSet<FName> Visited;
		Visited.Add(PackageName);
		GatherDependenciesRecursive(AssetRegistry, PackageName, bIncludeSoft, Visited, DependencyNames, 0, DepthLimit);
	}
	else
	{
		AssetRegistry.GetDependencies(PackageName, DependencyNames, UE::AssetRegistry::EDependencyCategory::Package, QueryFlags);
	}

	// Build referencers array
	int32 RedirectorRefsSkipped = 0;
	int32 RedirectorDepsSkipped = 0;
	TArray<TSharedPtr<FJsonValue>> ReferencersArray;
	for (const FName& RefName : ReferencerNames)
	{
		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPackageName(RefName, Assets);

		// Skip redirector assets
		if (Assets.Num() > 0 && Assets[0].IsRedirector())
		{
			++RedirectorRefsSkipped;
			continue;
		}

		TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
		RefObj->SetStringField(TEXT("path"), RefName.ToString());
		if (Assets.Num() > 0)
		{
			RefObj->SetStringField(TEXT("class"), Assets[0].AssetClassPath.GetAssetName().ToString());
		}
		else
		{
			RefObj->SetStringField(TEXT("class"), TEXT("Unknown"));
		}
		ReferencersArray.Add(MakeShared<FJsonValueObject>(RefObj));
	}

	// Build dependencies array
	TArray<TSharedPtr<FJsonValue>> DependenciesArray;
	for (const FName& DepName : DependencyNames)
	{
		TArray<FAssetData> Assets;
		AssetRegistry.GetAssetsByPackageName(DepName, Assets);

		// Skip redirector assets
		if (Assets.Num() > 0 && Assets[0].IsRedirector())
		{
			++RedirectorDepsSkipped;
			continue;
		}

		TSharedPtr<FJsonObject> DepObj = MakeShared<FJsonObject>();
		DepObj->SetStringField(TEXT("path"), DepName.ToString());
		if (Assets.Num() > 0)
		{
			DepObj->SetStringField(TEXT("class"), Assets[0].AssetClassPath.GetAssetName().ToString());
		}
		else
		{
			DepObj->SetStringField(TEXT("class"), TEXT("Unknown"));
		}
		DependenciesArray.Add(MakeShared<FJsonValueObject>(DepObj));
	}

	// Build result object
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetArrayField(TEXT("referencers"), ReferencersArray);
	Data->SetArrayField(TEXT("dependencies"), DependenciesArray);

	// Extract short asset name for summary
	FString AssetShortName = FPackageName::GetShortName(PackageNameStr);
	FString Summary = FString::Printf(TEXT("%s: %d referencers, %d dependencies"),
		*AssetShortName,
		ReferencersArray.Num(),
		DependenciesArray.Num());
	if (RedirectorRefsSkipped > 0 || RedirectorDepsSkipped > 0)
	{
		Summary += FString::Printf(TEXT(" (%d redirector refs hidden)"),
			RedirectorRefsSkipped + RedirectorDepsSkipped);
	}

	return MakeSuccessResult(Data, Summary);
}

void ClaireonTool_AssetReferences::GatherDependenciesRecursive(
	IAssetRegistry& AssetRegistry,
	FName PackageName,
	bool bIncludeSoft,
	TSet<FName>& Visited,
	TArray<FName>& OutDependencies,
	int32 CurrentDepth,
	int32 MaxDepth)
{
	if (CurrentDepth >= MaxDepth)
	{
		return;
	}

	TArray<FName> SubDependencies;
	UE::AssetRegistry::EDependencyQuery QueryFlags = UE::AssetRegistry::EDependencyQuery::Hard;
	if (bIncludeSoft)
	{
		QueryFlags = UE::AssetRegistry::EDependencyQuery::NoRequirements;
	}

	AssetRegistry.GetDependencies(PackageName, SubDependencies, UE::AssetRegistry::EDependencyCategory::Package, QueryFlags);

	for (const FName& SubDep : SubDependencies)
	{
		// Skip if already visited (cycle prevention)
		if (Visited.Contains(SubDep))
		{
			continue;
		}

		// Skip packages without valid asset data
		TOptional<FAssetPackageData> PackageData = AssetRegistry.GetAssetPackageDataCopy(SubDep);
		if (!PackageData.IsSet())
		{
			continue;
		}

		Visited.Add(SubDep);
		OutDependencies.Add(SubDep);

		// Recurse
		GatherDependenciesRecursive(AssetRegistry, SubDep, bIncludeSoft, Visited, OutDependencies, CurrentDepth + 1, MaxDepth);
	}
}

FString ClaireonTool_AssetReferences::FormatDiskSize(int64 SizeBytes)
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
