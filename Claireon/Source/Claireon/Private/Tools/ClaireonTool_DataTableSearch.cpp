// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_DataTableSearch.h"
#include "Tools/ClaireonDataTableHelpers.h"
#include "ClaireonLog.h"
#include "Engine/DataTable.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_DataTableSearch::GetName() const
{
	return TEXT("editor.datatable.search");
}

FString ClaireonTool_DataTableSearch::GetDescription() const
{
	return TEXT("Search for data table assets by name, showing row struct and row count for each match");
}

TSharedPtr<FJsonObject> ClaireonTool_DataTableSearch::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// query - required
	TSharedPtr<FJsonObject> QueryProp = MakeShared<FJsonObject>();
	QueryProp->SetStringField(TEXT("type"), TEXT("string"));
	QueryProp->SetStringField(TEXT("description"), TEXT("Search term to match against asset names"));
	Properties->SetObjectField(TEXT("query"), QueryProp);

	// max_results - optional
	TSharedPtr<FJsonObject> MaxProp = MakeShared<FJsonObject>();
	MaxProp->SetStringField(TEXT("type"), TEXT("integer"));
	MaxProp->SetStringField(TEXT("description"), TEXT("Maximum number of results to return (default: 20, max: 500)"));
	Properties->SetObjectField(TEXT("max_results"), MaxProp);

	// include_composite - optional
	TSharedPtr<FJsonObject> CompositeProp = MakeShared<FJsonObject>();
	CompositeProp->SetStringField(TEXT("type"), TEXT("boolean"));
	CompositeProp->SetStringField(TEXT("description"), TEXT("Include CompositeDataTable assets in results (default: true)"));
	Properties->SetObjectField(TEXT("include_composite"), CompositeProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("query")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_DataTableSearch::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Required parameter
	FString Query;
	if (!Arguments->TryGetStringField(TEXT("query"), Query) || Query.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: query"));
	}

	// Optional parameters
	int32 MaxResults = 20;
	if (Arguments->HasField(TEXT("max_results")))
	{
		MaxResults = FMath::Clamp(static_cast<int32>(Arguments->GetNumberField(TEXT("max_results"))), 1, 500);
	}

	bool bIncludeComposite = true;
	Arguments->TryGetBoolField(TEXT("include_composite"), bIncludeComposite);

	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.datatable.search: query=%s, max=%d, composite=%d"),
		*Query, MaxResults, bIncludeComposite);

	// Get Asset Registry
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Build filter
	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;
	Filter.PackagePaths.Add(FName(TEXT("/Game")));
	Filter.ClassPaths.Add(FTopLevelAssetPath(FString::Printf(TEXT("/Script/Engine.%s"), TEXT("DataTable"))));
	if (bIncludeComposite)
	{
		Filter.ClassPaths.Add(FTopLevelAssetPath(FString::Printf(TEXT("/Script/Engine.%s"), TEXT("CompositeDataTable"))));
	}

	TArray<FAssetData> AllAssets;
	AssetRegistry.GetAssets(Filter, AllAssets);

	// Score and rank results
	struct FScoredAsset
	{
		FAssetData AssetData;
		int32 Score;
	};

	TArray<FScoredAsset> ScoredAssets;
	for (const FAssetData& Asset : AllAssets)
	{
		const FString AssetName = Asset.AssetName.ToString();
		const FString PackagePath = Asset.PackageName.ToString();

		int32 Score = 0;

		// Exact match (case-sensitive)
		if (AssetName.Equals(Query))
		{
			Score = 100;
		}
		// Exact match (case-insensitive)
		else if (AssetName.Equals(Query, ESearchCase::IgnoreCase))
		{
			Score = 90;
		}
		// Prefix match (case-sensitive)
		else if (AssetName.StartsWith(Query))
		{
			Score = 80;
		}
		// Prefix match (case-insensitive)
		else if (AssetName.StartsWith(Query, ESearchCase::IgnoreCase))
		{
			Score = 70;
		}
		// Contains (case-sensitive)
		else if (AssetName.Contains(Query))
		{
			Score = 50;
		}
		// Contains (case-insensitive)
		else if (AssetName.Contains(Query, ESearchCase::IgnoreCase))
		{
			Score = 30;
		}
		// Path contains (case-insensitive)
		else if (PackagePath.Contains(Query, ESearchCase::IgnoreCase))
		{
			Score = 20;
		}

		if (Score > 0)
		{
			ScoredAssets.Add({ Asset, Score });
		}
	}

	// Sort by score descending
	ScoredAssets.Sort([](const FScoredAsset& A, const FScoredAsset& B)
	{
		return A.Score > B.Score;
	});

	// Clamp to max results
	if (ScoredAssets.Num() > MaxResults)
	{
		ScoredAssets.SetNum(MaxResults);
	}

	if (ScoredAssets.Num() == 0)
	{
		return MakeSuccessResult(nullptr, FString::Printf(TEXT("No data tables found matching \"%s\""), *Query));
	}

	FString Output;
	Output += FString::Printf(TEXT("Found %d data table(s) matching \"%s\":\n\n"), ScoredAssets.Num(), *Query);

	for (int32 i = 0; i < ScoredAssets.Num(); i++)
	{
		const FAssetData& Asset = ScoredAssets[i].AssetData;
		const FString PackageName = Asset.PackageName.ToString();

		// Determine if composite
		const bool bIsComposite = Asset.AssetClassPath.GetAssetName().ToString().Equals(TEXT("CompositeDataTable"), ESearchCase::IgnoreCase);

		// Load table to get row struct and row count
		FString LoadError;
		UDataTable* DataTable = ClaireonDataTableHelpers::LoadDataTableAsset(PackageName, LoadError);

		FString RowStructName = TEXT("Unknown");
		int32 RowCount = 0;

		if (DataTable)
		{
			const UScriptStruct* RowStruct = DataTable->GetRowStruct();
			if (RowStruct)
			{
				RowStructName = RowStruct->GetName();
			}
			RowCount = DataTable->GetRowNames().Num();
		}

		if (bIsComposite)
		{
			Output += FString::Printf(TEXT("%d. %s [RowStruct: %s] (%d rows) [Composite]\n"),
				i + 1, *PackageName, *RowStructName, RowCount);
		}
		else
		{
			Output += FString::Printf(TEXT("%d. %s [RowStruct: %s] (%d rows)\n"),
				i + 1, *PackageName, *RowStructName, RowCount);
		}
	}

	return MakeSuccessResult(nullptr, Output);
}
