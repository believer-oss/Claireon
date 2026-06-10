// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AnimGraphAnalyze.h"
#include "Tools/ClaireonAnimGraphHelpers.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_Base.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_AnimGraphAnalyze::GetCategory() const { return TEXT("animbp"); }
FString ClaireonTool_AnimGraphAnalyze::GetOperation() const { return TEXT("analyze"); }

FString ClaireonTool_AnimGraphAnalyze::GetDescription() const
{
	return TEXT("Analyze an Animation Blueprint for fast path compliance, thread safety issues, "
		"and compiler warnings. Returns per-node fast path status, per-function thread safety "
		"flags, and categorized warning counts.");
}

TSharedPtr<FJsonObject> ClaireonTool_AnimGraphAnalyze::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the Animation Blueprint asset"), true);
	S.AddEnum(TEXT("analysis_type"), TEXT("What to analyze: 'fast_path', 'thread_safety', 'warnings', or 'all'"),
		{TEXT("all"), TEXT("fast_path"), TEXT("thread_safety"), TEXT("warnings")});
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_AnimGraphAnalyze::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString AnalysisType = TEXT("all");
	Arguments->TryGetStringField(TEXT("analysis_type"), AnalysisType);

	FString Error;
	UAnimBlueprint* AnimBP = ClaireonAnimGraphHelpers::LoadAnimBlueprint(AssetPath, Error);
	if (!AnimBP)
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AnimBP->GetPathName());
	Data->SetStringField(TEXT("asset_name"), AnimBP->GetName());
	Data->SetStringField(TEXT("analysis_type"), AnalysisType);

	int32 TotalIssues = 0;

	// Fast path analysis
	if (AnalysisType == TEXT("fast_path") || AnalysisType == TEXT("all"))
	{
		TArray<TSharedPtr<FJsonValue>> FastPathResults;
		int32 FastPathNodes = 0;
		int32 NonFastPathNodes = 0;

		TArray<ClaireonAnimGraphHelpers::FAnimGraphInfo> AllGraphs = ClaireonAnimGraphHelpers::CollectAllGraphs(AnimBP);
		for (const ClaireonAnimGraphHelpers::FAnimGraphInfo& GraphInfo : AllGraphs)
		{
			if (!GraphInfo.Graph)
			{
				continue;
			}

			for (UEdGraphNode* Node : GraphInfo.Graph->Nodes)
			{
				UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
				if (!AnimNode)
				{
					continue;
				}

				TArray<FString> Warnings;
				bool bFastPath = ClaireonAnimGraphHelpers::AnalyzeFastPath(AnimNode, Warnings);

				TSharedPtr<FJsonObject> NodeResult = MakeShared<FJsonObject>();
				NodeResult->SetStringField(TEXT("graph"), GraphInfo.Name);
				NodeResult->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
				NodeResult->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
				NodeResult->SetStringField(TEXT("category"), ClaireonAnimGraphHelpers::GetAnimNodeCategory(Node));
				NodeResult->SetBoolField(TEXT("is_fast_path"), bFastPath);

				if (!bFastPath)
				{
					NonFastPathNodes++;
					TArray<TSharedPtr<FJsonValue>> WarningsArray;
					for (const FString& Warning : Warnings)
					{
						WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
					}
					NodeResult->SetArrayField(TEXT("warnings"), WarningsArray);
				}
				else
				{
					FastPathNodes++;
				}

				FastPathResults.Add(MakeShared<FJsonValueObject>(NodeResult));
			}
		}

		TSharedPtr<FJsonObject> FastPathObj = MakeShared<FJsonObject>();
		FastPathObj->SetArrayField(TEXT("nodes"), FastPathResults);
		FastPathObj->SetNumberField(TEXT("fast_path_node_count"), FastPathNodes);
		FastPathObj->SetNumberField(TEXT("non_fast_path_node_count"), NonFastPathNodes);
		FastPathObj->SetNumberField(TEXT("total_analyzed"), FastPathResults.Num());
		Data->SetObjectField(TEXT("fast_path_analysis"), FastPathObj);

		TotalIssues += NonFastPathNodes;
	}

	// Thread safety analysis
	if (AnalysisType == TEXT("thread_safety") || AnalysisType == TEXT("all"))
	{
		TSharedPtr<FJsonObject> TSAnalysis = ClaireonAnimGraphHelpers::AnalyzeThreadSafety(AnimBP);
		Data->SetObjectField(TEXT("thread_safety_analysis"), TSAnalysis);

		double UnsafeCount = 0;
		if (TSAnalysis->TryGetNumberField(TEXT("non_thread_safe_count"), UnsafeCount))
		{
			TotalIssues += static_cast<int32>(UnsafeCount);
		}
	}

	// Warning collection
	if (AnalysisType == TEXT("warnings") || AnalysisType == TEXT("all"))
	{
		TSharedPtr<FJsonObject> Warnings = ClaireonAnimGraphHelpers::CollectWarnings(AnimBP);
		Data->SetObjectField(TEXT("warnings"), Warnings);

		double WarnCount = 0;
		if (Warnings->TryGetNumberField(TEXT("total_warning_count"), WarnCount))
		{
			// Don't double-count — warnings includes fast path and thread safety
			if (AnalysisType == TEXT("warnings"))
			{
				TotalIssues += static_cast<int32>(WarnCount);
			}
		}
	}

	Data->SetNumberField(TEXT("total_issues"), TotalIssues);

	FString Summary = FString::Printf(TEXT("Analysis of '%s' (%s): %d issues found"),
		*AnimBP->GetName(), *AnalysisType, TotalIssues);

	return MakeSuccessResult(Data, Summary);
}
