// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_SearchTools.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "ClaireonBridge.h"
#include "ClaireonXmlFormatter.h"
#include "ClaireonLog.h"
#include "IPythonScriptPlugin.h"
#include "PythonScriptTypes.h"
#include "HAL/ThreadHeartBeat.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"
#include "Policies/CondensedJsonPrintPolicy.h"

FString ClaireonTool_SearchTools::GetName() const
{
	return TEXT("claireon.tools_search");
}

FString ClaireonTool_SearchTools::GetDescription() const
{
	return TEXT("Search available tools by query string and/or category. "
		"Uses fuzzy matching with synonym expansion (e.g. 'bp' matches 'blueprint', "
		"'dt' matches 'data table') and semantic search when available. "
		"Returns matching tools grouped by category with descriptions and type signatures.");
}

TSharedPtr<FJsonObject> ClaireonTool_SearchTools::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// query - optional
	TSharedPtr<FJsonObject> QueryProp = MakeShared<FJsonObject>();
	QueryProp->SetStringField(TEXT("type"), TEXT("string"));
	QueryProp->SetStringField(TEXT("description"), TEXT("Search query to match against tool names and descriptions. Supports abbreviations (bp, bt, st, dt, pie, fx, etc.). Empty string returns all tools."));
	QueryProp->SetStringField(TEXT("default"), TEXT(""));
	Properties->SetObjectField(TEXT("query"), QueryProp);

	// category - optional
	TSharedPtr<FJsonObject> CategoryProp = MakeShared<FJsonObject>();
	CategoryProp->SetStringField(TEXT("type"), TEXT("string"));
	CategoryProp->SetStringField(TEXT("description"), TEXT("Filter by tool category. Empty string means no category filter."));
	CategoryProp->SetStringField(TEXT("default"), TEXT(""));
	Properties->SetObjectField(TEXT("category"), CategoryProp);

	// max_results - optional
	TSharedPtr<FJsonObject> MaxResultsProp = MakeShared<FJsonObject>();
	MaxResultsProp->SetStringField(TEXT("type"), TEXT("integer"));
	MaxResultsProp->SetStringField(TEXT("description"), TEXT("Maximum number of tools to return."));
	MaxResultsProp->SetNumberField(TEXT("default"), 100);
	Properties->SetObjectField(TEXT("max_results"), MaxResultsProp);

	// detail - optional
	TSharedPtr<FJsonObject> DetailProp = MakeShared<FJsonObject>();
	DetailProp->SetStringField(TEXT("type"), TEXT("string"));
	DetailProp->SetStringField(TEXT("description"),
		TEXT("Description detail level. 'brief' = one-line summary, "
			"'standard' = concise description (default), "
			"'full' = extended docs with examples and workflows."));
	DetailProp->SetStringField(TEXT("default"), TEXT("standard"));
	TArray<TSharedPtr<FJsonValue>> DetailEnum;
	DetailEnum.Add(MakeShared<FJsonValueString>(TEXT("brief")));
	DetailEnum.Add(MakeShared<FJsonValueString>(TEXT("standard")));
	DetailEnum.Add(MakeShared<FJsonValueString>(TEXT("full")));
	DetailProp->SetArrayField(TEXT("enum"), DetailEnum);
	Properties->SetObjectField(TEXT("detail"), DetailProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

// ---------------------------------------------------------------------------
// Python catalog integration
// ---------------------------------------------------------------------------

bool ClaireonTool_SearchTools::RebuildCatalog()
{
	FClaireonServer* Server = FClaireonModule::Get().GetServer();
	if (!Server)
	{
		return false;
	}

	FClaireonBridge::EnsureRegistered();

	const TMap<FString, TSharedPtr<IClaireonTool>>& ToolsMap = Server->GetTools();

	// Serialize tool metadata to JSON array
	TArray<TSharedPtr<FJsonValue>> ToolsArray;
	for (const auto& Pair : ToolsMap)
	{
		const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
		const FString ToolName = Tool->GetName();

		// Skip meta tools
		if (ToolName == TEXT("claireon.python_execute") || ToolName == TEXT("claireon.tools_search"))
		{
			continue;
		}

		TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
		ToolObj->SetStringField(TEXT("name"), ToolName);
		ToolObj->SetStringField(TEXT("description"), Tool->GetDescription());
		ToolObj->SetStringField(TEXT("category"), Tool->GetCategory());
		ToolsArray.Add(MakeShared<FJsonValueObject>(ToolObj));
	}

	FString ToolsJson;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ToolsJson);
	FJsonSerializer::Serialize(ToolsArray, Writer);
	Writer->Close();

	// Use raw string with triple quotes to safely embed JSON containing quotes
	FString PythonCode = FString::Printf(
		TEXT("import json as _json\n")
		TEXT("from mcp_tool_catalog import rebuild as _rebuild\n")
		TEXT("_result = _rebuild(r\"\"\"%s\"\"\")\n")
		TEXT("print(_json.dumps(_result))\n"),
		*ToolsJson
	);

	FPythonCommandEx PythonCommand;
	PythonCommand.Command = PythonCode;
	PythonCommand.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
	PythonCommand.FileExecutionScope = EPythonFileExecutionScope::Private;

	FSlowHeartBeatScope SuspendHeartBeat;
	FDisableHitchDetectorScope SuspendHitchDetector;

	bool bSuccess = IPythonScriptPlugin::Get()->ExecPythonCommandEx(PythonCommand);

	if (bSuccess)
	{
		// Extract the printed JSON result from log output
		FString ResultJson;
		for (const FPythonLogOutputEntry& Entry : PythonCommand.LogOutput)
		{
			if (Entry.Type == EPythonLogOutputType::Info || Entry.Type == EPythonLogOutputType::Warning)
			{
				ResultJson = Entry.Output;
			}
		}
		if (!ResultJson.IsEmpty())
		{
			LastCatalogToolCount = ToolsMap.Num();
			UE_LOG(LogClaireon, Display, TEXT("[MCP] Tool catalog rebuilt (%d tools)"), ToolsArray.Num());
			return true;
		}
	}

	UE_LOG(LogClaireon, Warning, TEXT("[MCP] Failed to rebuild tool catalog via Python"));
	return false;
}

TArray<FString> ClaireonTool_SearchTools::FuzzySearch(const FString& Query, int32 MaxResults)
{
	TArray<FString> Results;

	FClaireonBridge::EnsureRegistered();

	// Use raw string with triple quotes to safely embed query containing quotes
	FString PythonCode = FString::Printf(
		TEXT("import json as _json\n")
		TEXT("from mcp_tool_catalog import search as _search\n")
		TEXT("_hits = _search(r\"\"\"%s\"\"\", max_results=%d)\n")
		TEXT("print(_json.dumps(_hits))\n"),
		*Query, MaxResults
	);

	FPythonCommandEx PythonCommand;
	PythonCommand.Command = PythonCode;
	PythonCommand.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
	PythonCommand.FileExecutionScope = EPythonFileExecutionScope::Private;

	FSlowHeartBeatScope SuspendHeartBeat;
	FDisableHitchDetectorScope SuspendHitchDetector;

	bool bSuccess = IPythonScriptPlugin::Get()->ExecPythonCommandEx(PythonCommand);
	if (!bSuccess)
	{
		return Results;
	}

	// Extract the printed JSON result from log output
	FString ResultJson;
	for (const FPythonLogOutputEntry& Entry : PythonCommand.LogOutput)
	{
		if (Entry.Type == EPythonLogOutputType::Info || Entry.Type == EPythonLogOutputType::Warning)
		{
			ResultJson = Entry.Output;
		}
	}
	if (ResultJson.IsEmpty())
	{
		return Results;
	}

	// Parse the results array
	TArray<TSharedPtr<FJsonValue>> HitsArray;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultJson);
	if (!FJsonSerializer::Deserialize(Reader, HitsArray))
	{
		return Results;
	}

	for (const TSharedPtr<FJsonValue>& HitVal : HitsArray)
	{
		const TSharedPtr<FJsonObject>* HitObj = nullptr;
		if (HitVal->TryGetObject(HitObj) && (*HitObj).IsValid())
		{
			FString ToolName;
			if ((*HitObj)->TryGetStringField(TEXT("tool_name"), ToolName) && !ToolName.IsEmpty())
			{
				Results.Add(ToolName);
			}
		}
	}

	return Results;
}

// ---------------------------------------------------------------------------
// Execute
// ---------------------------------------------------------------------------

IClaireonTool::FToolResult ClaireonTool_SearchTools::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FClaireonServer* Server = FClaireonModule::Get().GetServer();
	if (!Server)
	{
		return MakeErrorResult(TEXT("MCP server is not running"));
	}

	// Read parameters
	FString Query;
	FString Category;
	FString Detail = TEXT("standard");
	int32 MaxResults = 100;

	if (Arguments.IsValid())
	{
		Arguments->TryGetStringField(TEXT("query"), Query);
		Arguments->TryGetStringField(TEXT("category"), Category);
		Arguments->TryGetStringField(TEXT("detail"), Detail);

		double MaxResultsVal = 100;
		if (Arguments->TryGetNumberField(TEXT("max_results"), MaxResultsVal))
		{
			MaxResults = static_cast<int32>(MaxResultsVal);
			if (MaxResults <= 0) MaxResults = 100;
		}
	}

	Detail = Detail.TrimStartAndEnd().ToLower();
	if (Detail != TEXT("brief") && Detail != TEXT("full"))
	{
		Detail = TEXT("standard");
	}

	Query = Query.TrimStartAndEnd();
	Category = Category.TrimStartAndEnd();

	const TMap<FString, TSharedPtr<IClaireonTool>>& ToolsMap = Server->GetTools();

	// Determine which tools to include via fuzzy search or full catalog
	TArray<FString> FuzzyRankedNames;  // ordered by relevance
	TSet<FString> FuzzyMatchedSet;
	bool bUsedFuzzySearch = false;

	if (!Query.IsEmpty())
	{
		// Ensure catalog is built (or rebuilt if tool count changed)
		if (LastCatalogToolCount != ToolsMap.Num())
		{
			RebuildCatalog();
		}

		// Try fuzzy search via Python IndexEngine
		FuzzyRankedNames = FuzzySearch(Query, MaxResults);
		if (FuzzyRankedNames.Num() > 0)
		{
			bUsedFuzzySearch = true;
			for (const FString& Name : FuzzyRankedNames)
			{
				FuzzyMatchedSet.Add(Name);
			}
		}
		// If fuzzy search returned nothing (Python unavailable or no matches),
		// fall back to substring matching below
	}

	// Collect matching tools
	struct FMatchEntry
	{
		FString Name;
		FString Description;
		FString Category;
		FString TypeSignature;
		FString Source;
	};
	TArray<FMatchEntry> MatchingTools;

	const FString QueryLower = Query.ToLower();
	const FString CategoryLower = Category.ToLower();

	for (const auto& Pair : ToolsMap)
	{
		const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
		const FString ToolName = Tool->GetName();

		// Skip meta tools
		if (ToolName == TEXT("claireon.python_execute") || ToolName == TEXT("claireon.tools_search"))
		{
			continue;
		}

		const FString ToolCategory = Tool->GetCategory();

		// Apply category filter
		if (!CategoryLower.IsEmpty() && ToolCategory.ToLower() != CategoryLower)
		{
			continue;
		}

		// Apply query filter
		if (!QueryLower.IsEmpty())
		{
			if (bUsedFuzzySearch)
			{
				// Fuzzy search mode: only include tools that matched
				if (!FuzzyMatchedSet.Contains(ToolName))
				{
					continue;
				}
			}
			else
			{
				// Fallback: substring matching (all query words must match somewhere)
				TArray<FString> QueryWords;
				QueryLower.ParseIntoArray(QueryWords, TEXT(" "), true);

				const FString NameLower = ToolName.ToLower();
				const FString DescLower = Tool->GetDescription().ToLower();

				bool bAllWordsMatch = true;
				for (const FString& Word : QueryWords)
				{
					if (!NameLower.Contains(Word) && !DescLower.Contains(Word))
					{
						bAllWordsMatch = false;
						break;
					}
				}
				if (!bAllWordsMatch)
				{
					continue;
				}
			}
		}

		// Select description tier
		FString ToolDescription;
		if (Detail == TEXT("brief"))
		{
			ToolDescription = Tool->GetBriefDescription();
		}
		else if (Detail == TEXT("full"))
		{
			ToolDescription = Tool->GetFullDescription();
		}
		else
		{
			ToolDescription = Tool->GetDescription();
		}

		FMatchEntry Entry;
		Entry.Name = ToolName;
		Entry.Description = ToolDescription;
		Entry.Category = ToolCategory;
		Entry.TypeSignature = FClaireonXmlFormatter::GenerateTypeSignature(ToolName, Tool->GetInputSchema());

		// Look up tool source from the server's source map
		const TMap<FString, FName>& SourceMap = Server->GetToolSourceMap();
		if (const FName* SourceName = SourceMap.Find(ToolName))
		{
			Entry.Source = SourceName->ToString();
		}

		MatchingTools.Add(MoveTemp(Entry));
	}

	// Sort: fuzzy-ranked results preserve relevance order; fallback sorts by category/name
	if (bUsedFuzzySearch)
	{
		// Build rank map from the cached fuzzy results order
		TMap<FString, int32> RankMap;
		for (int32 i = 0; i < FuzzyRankedNames.Num(); ++i)
		{
			RankMap.Add(FuzzyRankedNames[i], i);
		}
		MatchingTools.Sort([&RankMap](const FMatchEntry& A, const FMatchEntry& B)
		{
			const int32* RankA = RankMap.Find(A.Name);
			const int32* RankB = RankMap.Find(B.Name);
			int32 RA = RankA ? *RankA : INT32_MAX;
			int32 RB = RankB ? *RankB : INT32_MAX;
			return RA < RB;
		});
	}
	else
	{
		MatchingTools.Sort([](const FMatchEntry& A, const FMatchEntry& B)
		{
			if (A.Category != B.Category)
			{
				return A.Category < B.Category;
			}
			return A.Name < B.Name;
		});
	}

	// Truncate after sorting
	const int32 TotalMatching = MatchingTools.Num();
	if (MatchingTools.Num() > MaxResults)
	{
		MatchingTools.SetNum(MaxResults);
	}

	// Build structured Data as JSON
	TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
	if (!Query.IsEmpty())
	{
		DataObj->SetStringField(TEXT("query"), Query);
	}
	DataObj->SetNumberField(TEXT("total_matching"), TotalMatching);
	DataObj->SetNumberField(TEXT("returned"), MatchingTools.Num());

	// Group by category for the JSON output
	TMap<FString, TArray<const FMatchEntry*>> Grouped;
	for (const FMatchEntry& Entry : MatchingTools)
	{
		Grouped.FindOrAdd(Entry.Category).Add(&Entry);
	}

	TArray<TSharedPtr<FJsonValue>> CategoriesArr;
	TArray<FString> CategoryNames;
	Grouped.GetKeys(CategoryNames);
	CategoryNames.Sort();

	for (const FString& CatName : CategoryNames)
	{
		TSharedPtr<FJsonObject> CatObj = MakeShared<FJsonObject>();
		CatObj->SetStringField(TEXT("name"), CatName);

		TArray<TSharedPtr<FJsonValue>> ToolsArr;
		for (const FMatchEntry* Entry : Grouped[CatName])
		{
			TSharedPtr<FJsonObject> ToolObj = MakeShared<FJsonObject>();
			ToolObj->SetStringField(TEXT("name"), Entry->Name);
			ToolObj->SetStringField(TEXT("description"), Entry->Description);
			ToolObj->SetStringField(TEXT("signature"), Entry->TypeSignature);
			if (!Entry->Source.IsEmpty())
			{
				ToolObj->SetStringField(TEXT("source"), Entry->Source);
			}
			ToolsArr.Add(MakeShared<FJsonValueObject>(ToolObj));
		}
		CatObj->SetArrayField(TEXT("tools"), ToolsArr);
		CategoriesArr.Add(MakeShared<FJsonValueObject>(CatObj));
	}
	DataObj->SetArrayField(TEXT("categories"), CategoriesArr);

	// Build plain text summary
	FString SummaryText;
	if (Query.IsEmpty())
	{
		SummaryText = FString::Printf(TEXT("Found %d tool(s) across %d categories"), TotalMatching, CategoryNames.Num());
	}
	else
	{
		SummaryText = FString::Printf(TEXT("Found %d tool(s) matching '%s'"), TotalMatching, *Query);
		if (bUsedFuzzySearch)
		{
			SummaryText += TEXT(" (fuzzy)");
		}
	}
	if (TotalMatching > MatchingTools.Num())
	{
		SummaryText += FString::Printf(TEXT(" (showing %d of %d)"), MatchingTools.Num(), TotalMatching);
	}

	return MakeSuccessResult(DataObj, SummaryText);
}
