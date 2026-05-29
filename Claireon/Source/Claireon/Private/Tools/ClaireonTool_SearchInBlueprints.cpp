// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_SearchInBlueprints.h"
#include "ClaireonLog.h"
#include "ClaireonSettings.h"
#include "FindInBlueprintManager.h"
#include "HAL/PlatformProcess.h"

FString ClaireonTool_SearchInBlueprints::GetCategory() const { return TEXT("bp"); }
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
	TimeoutProp->SetStringField(TEXT("description"), TEXT("Search timeout in seconds (default: from settings, typically 30). Increase if FiB index is still building."));
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
	double MaxWaitSeconds = GetDefault<UClaireonSettings>()->BlueprintSearchTimeoutSeconds;
	// Per-call timeout override
	double TimeoutOverride = 0.0;
	if (Arguments->TryGetNumberField(TEXT("timeout"), TimeoutOverride) && TimeoutOverride > 0.0)
	{
		MaxWaitSeconds = TimeoutOverride;
	}

	// Helper lambda: poll a stream search with timeout
	auto WaitForSearch = [](TSharedPtr<FStreamSearch>& Search, double Timeout) -> bool
	{
		const double Start = FPlatformTime::Seconds();
		while (!Search->IsComplete())
		{
			if (FPlatformTime::Seconds() - Start > Timeout)
			{
				return false; // timed out
			}
			FPlatformProcess::Sleep(0.01f); // 10ms yield
		}
		return true; // completed
	};

	if (!WaitForSearch(StreamSearch, MaxWaitSeconds))
	{
		// Retry once — the FiB index may still be building after editor startup
		UE_LOG(LogClaireon, Warning, TEXT("[SearchInBlueprints] Search timed out after %.0fs — retrying once for query: %s"),
			MaxWaitSeconds, *Query);

		FPlatformProcess::Sleep(2.0f);
		StreamSearch = MakeShared<FStreamSearch>(Query, SearchOptions);

		if (!WaitForSearch(StreamSearch, MaxWaitSeconds))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Blueprint search timed out after retry (%.0fs x2). The FiB index may still be building. Try again later."),
				MaxWaitSeconds));
		}
	}

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
