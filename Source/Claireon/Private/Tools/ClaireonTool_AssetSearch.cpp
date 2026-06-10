// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AssetSearch.h"
#include "ClaireonLog.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

FString ClaireonTool_AssetSearch::GetCategory() const { return TEXT("asset"); }
FString ClaireonTool_AssetSearch::GetOperation() const { return TEXT("search"); }

FString ClaireonTool_AssetSearch::GetDescription() const
{
    return TEXT("Search for assets in the Unreal Asset Registry by name. Optionally filter by class or path. Results are ranked by match quality. Stateless / read-only / non-session.");
}

TSharedPtr<FJsonObject> ClaireonTool_AssetSearch::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// search_dir - primary directory to search
	TSharedPtr<FJsonObject> SearchDirProp = MakeShared<FJsonObject>();
	SearchDirProp->SetStringField(TEXT("type"), TEXT("string"));
	SearchDirProp->SetStringField(TEXT("description"), TEXT("Content path to search under (e.g. /Game/Characters). Default: /Game"));
	Properties->SetObjectField(TEXT("search_dir"), SearchDirProp);

	// class_filter - optional
	TSharedPtr<FJsonObject> ClassProp = MakeShared<FJsonObject>();
	ClassProp->SetStringField(TEXT("type"), TEXT("string"));
	ClassProp->SetStringField(TEXT("description"), TEXT("Filter results to a specific asset class (e.g. Blueprint, StaticMesh, Material, World)"));
	Properties->SetObjectField(TEXT("class_filter"), ClassProp);

	// query - primary name search parameter (alias for name_filter)
	TSharedPtr<FJsonObject> QueryProp = MakeShared<FJsonObject>();
	QueryProp->SetStringField(TEXT("type"), TEXT("string"));
	QueryProp->SetStringField(TEXT("description"), TEXT("Search query: filter results by asset name (case-insensitive substring match). Alias for name_filter; name_filter takes precedence if both are provided."));
	Properties->SetObjectField(TEXT("query"), QueryProp);

	// name_filter - optional (legacy alias for query)
	TSharedPtr<FJsonObject> NameProp = MakeShared<FJsonObject>();
	NameProp->SetStringField(TEXT("type"), TEXT("string"));
	NameProp->SetStringField(TEXT("description"), TEXT("Filter results by asset name (case-insensitive substring match). Same as query; kept for backward compatibility."));
	Properties->SetObjectField(TEXT("name_filter"), NameProp);

	// recursive - optional
	TSharedPtr<FJsonObject> RecursiveProp = MakeShared<FJsonObject>();
	RecursiveProp->SetStringField(TEXT("type"), TEXT("boolean"));
	RecursiveProp->SetStringField(TEXT("description"), TEXT("Search subdirectories recursively (default: true)"));
	Properties->SetObjectField(TEXT("recursive"), RecursiveProp);

	// max_results - optional (backward compat)
	TSharedPtr<FJsonObject> MaxProp = MakeShared<FJsonObject>();
	MaxProp->SetStringField(TEXT("type"), TEXT("integer"));
	MaxProp->SetStringField(TEXT("description"), TEXT("Maximum number of results to return (default: 50, max: 500)"));
	Properties->SetObjectField(TEXT("max_results"), MaxProp);

	// search_roots - optional (backward compat)
	TSharedPtr<FJsonObject> SearchRootsProp = MakeShared<FJsonObject>();
	SearchRootsProp->SetStringField(TEXT("type"), TEXT("array"));
	SearchRootsProp->SetStringField(TEXT("description"), TEXT("Package path roots to search under (default: [\"/Game\"]). Overrides search_dir if provided."));
	{
		TSharedPtr<FJsonObject> ItemsObj = MakeShared<FJsonObject>();
		ItemsObj->SetStringField(TEXT("type"), TEXT("string"));
		SearchRootsProp->SetObjectField(TEXT("items"), ItemsObj);
	}
	Properties->SetObjectField(TEXT("search_roots"), SearchRootsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_AssetSearch::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Parse arguments
	FString SearchDir = TEXT("/Game");
	if (Arguments->HasField(TEXT("search_dir")))
	{
		SearchDir = Arguments->GetStringField(TEXT("search_dir"));
	}

	FString ClassFilter;
	if (Arguments->HasField(TEXT("class_filter")))
	{
		ClassFilter = Arguments->GetStringField(TEXT("class_filter"));
	}

	FString NameFilter;
	if (Arguments->HasField(TEXT("name_filter")))
	{
		NameFilter = Arguments->GetStringField(TEXT("name_filter"));
	}
	else if (Arguments->HasField(TEXT("query")))
	{
		NameFilter = Arguments->GetStringField(TEXT("query"));
	}

	bool bRecursive = true;
	if (Arguments->HasField(TEXT("recursive")))
	{
		bRecursive = Arguments->GetBoolField(TEXT("recursive"));
	}

	int32 MaxResults = 50;
	if (Arguments->HasField(TEXT("max_results")))
	{
		MaxResults = FMath::Clamp(static_cast<int32>(Arguments->GetNumberField(TEXT("max_results"))), 1, 500);
	}

	// Build search roots: search_roots overrides search_dir if provided
	TArray<FString> SearchRoots;
	if (Arguments->HasField(TEXT("search_roots")))
	{
		const TArray<TSharedPtr<FJsonValue>>& RootsArray = Arguments->GetArrayField(TEXT("search_roots"));
		for (const TSharedPtr<FJsonValue>& Val : RootsArray)
		{
			FString Root;
			if (Val->TryGetString(Root) && !Root.IsEmpty())
			{
				SearchRoots.Add(Root);
			}
		}
	}

	if (SearchRoots.Num() == 0)
	{
		SearchRoots.Add(SearchDir);
	}

	// Query the Asset Registry
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FAssetData> AllAssets;
	for (const FString& Root : SearchRoots)
	{
		TArray<FAssetData> RootAssets;
		AssetRegistry.GetAssetsByPath(FName(*Root), RootAssets, bRecursive);
		AllAssets.Append(RootAssets);
	}

	// Filter and collect results
	TArray<TSharedPtr<FJsonValue>> AssetsArray;
	int32 TotalMatched = 0;
	int32 RedirectorsSkipped = 0;

	for (const FAssetData& AssetData : AllAssets)
	{
		// Skip redirector assets -- they are artifacts of asset moves, not real content
		if (AssetData.IsRedirector())
		{
			++RedirectorsSkipped;
			continue;
		}

		const FString AssetName = AssetData.AssetName.ToString();
		const FString AssetClassName = AssetData.AssetClassPath.GetAssetName().ToString();
		const FString PackagePath = AssetData.PackagePath.ToString();
		const FString ObjectPath = AssetData.GetObjectPathString();

		// Filter by search roots
		if (!IsUnderSearchRoots(PackagePath, SearchRoots))
		{
			continue;
		}

		// Apply class filter (case-insensitive contains)
		if (!ClassFilter.IsEmpty())
		{
			if (!AssetClassName.Contains(ClassFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		// Apply name filter (case-insensitive contains)
		if (!NameFilter.IsEmpty())
		{
			if (!AssetName.Contains(NameFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		TotalMatched++;

		if (AssetsArray.Num() < MaxResults)
		{
			TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
			AssetObj->SetStringField(TEXT("path"), ObjectPath);
			AssetObj->SetStringField(TEXT("class"), AssetClassName);
			AssetObj->SetStringField(TEXT("name"), AssetName);
			AssetsArray.Add(MakeShared<FJsonValueObject>(AssetObj));
		}
	}

	// Sort by path alphabetically
	AssetsArray.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		const FString PathA = A->AsObject()->GetStringField(TEXT("path"));
		const FString PathB = B->AsObject()->GetStringField(TEXT("path"));
		return PathA < PathB;
	});

	// Build result data
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("assets"), AssetsArray);
	Data->SetNumberField(TEXT("total_count"), TotalMatched);
	if (RedirectorsSkipped > 0)
	{
		Data->SetNumberField(TEXT("redirectors_hidden"), RedirectorsSkipped);
	}
	Data->SetStringField(TEXT("search_dir"), SearchRoots.Num() == 1 ? SearchRoots[0] : FString::Join(SearchRoots, TEXT(", ")));

	// Build summary
	FString Summary = FString::Printf(TEXT("Found %d assets in %s"), TotalMatched, *SearchRoots[0]);
	if (!ClassFilter.IsEmpty())
	{
		Summary += FString::Printf(TEXT(" (filtered by %s)"), *ClassFilter);
	}
	if (!NameFilter.IsEmpty())
	{
		Summary += FString::Printf(TEXT(" (name contains \"%s\")"), *NameFilter);
	}
	if (TotalMatched > MaxResults)
	{
		Summary += FString::Printf(TEXT(" — showing first %d"), MaxResults);
	}
	if (RedirectorsSkipped > 0)
	{
		Summary += FString::Printf(TEXT(" (%d redirectors hidden)"), RedirectorsSkipped);
	}

	return MakeSuccessResult(Data, Summary);
}

int32 ClaireonTool_AssetSearch::ScoreAssetByName(const FString& AssetName, const FString& PackagePath, const FString& Query)
{
	// Exact match (case-sensitive)
	if (AssetName.Equals(Query))
	{
		return 100;
	}

	// Prefix match (case-sensitive)
	if (AssetName.StartsWith(Query))
	{
		return 80;
	}

	// Prefix match (case-insensitive)
	if (AssetName.StartsWith(Query, ESearchCase::IgnoreCase))
	{
		return 70;
	}

	// Contains (case-sensitive)
	if (AssetName.Contains(Query))
	{
		return 50;
	}

	// Contains (case-insensitive)
	if (AssetName.Contains(Query, ESearchCase::IgnoreCase))
	{
		return 30;
	}

	// Path contains (case-insensitive)
	if (PackagePath.Contains(Query, ESearchCase::IgnoreCase))
	{
		return 20;
	}

	return 0;
}

int32 ClaireonTool_AssetSearch::ScoreAssetByClass(const FString& ClassName, const FString& Query)
{
	// Exact match (case-sensitive)
	if (ClassName.Equals(Query))
	{
		return 100;
	}

	// Exact match (case-insensitive)
	if (ClassName.Equals(Query, ESearchCase::IgnoreCase))
	{
		return 90;
	}

	// Contains (case-sensitive)
	if (ClassName.Contains(Query))
	{
		return 50;
	}

	// Contains (case-insensitive)
	if (ClassName.Contains(Query, ESearchCase::IgnoreCase))
	{
		return 30;
	}

	return 0;
}

int32 ClaireonTool_AssetSearch::ScoreAssetByPath(const FString& PackagePath, const FString& Query)
{
	// Exact match (case-sensitive) Ã¢Â€Â” full path matches query
	if (PackagePath.Equals(Query))
	{
		return 100;
	}

	// Exact match (case-insensitive)
	if (PackagePath.Equals(Query, ESearchCase::IgnoreCase))
	{
		return 90;
	}

	// Ends with query (case-sensitive) Ã¢Â€Â” e.g., query "/Characters/BP_Player" on full path
	if (PackagePath.EndsWith(Query))
	{
		return 80;
	}

	// Contains (case-sensitive)
	if (PackagePath.Contains(Query))
	{
		return 50;
	}

	// Contains (case-insensitive)
	if (PackagePath.Contains(Query, ESearchCase::IgnoreCase))
	{
		return 30;
	}

	return 0;
}

bool ClaireonTool_AssetSearch::IsUnderSearchRoots(const FString& PackagePath, const TArray<FString>& SearchRoots)
{
	for (const FString& Root : SearchRoots)
	{
		// Check if path starts with root followed by "/" or is exactly root
		if (PackagePath.StartsWith(Root + TEXT("/")) || PackagePath == Root)
		{
			return true;
		}
	}
	return false;
}

FString ClaireonTool_AssetSearch::FormatDiskSize(int64 SizeBytes)
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
