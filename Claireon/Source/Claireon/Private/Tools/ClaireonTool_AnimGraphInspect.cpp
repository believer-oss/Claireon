// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AnimGraphInspect.h"
#include "Tools/ClaireonAnimGraphHelpers.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "Animation/AnimBlueprint.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_AnimGraphInspect::GetName() const
{
	return TEXT("claireon.animgraph_inspect");
}

TArray<FString> ClaireonTool_AnimGraphInspect::GetSearchKeywords() const
{
	return {TEXT("anim"), TEXT("animation"), TEXT("animgraph"), TEXT("blueprint"), TEXT("inspect"), TEXT("graph")};
}

FString ClaireonTool_AnimGraphInspect::GetDescription() const
{
	return TEXT("Blueprint-level overview of an Animation Blueprint. Returns class settings (parent class, "
		"skeleton, interfaces), variables, functions (with thread safety flags), all graphs enumerated "
		"(AnimGraph, state machines, state graphs, transitions), and a warning summary.");
}

TSharedPtr<FJsonObject> ClaireonTool_AnimGraphInspect::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the Animation Blueprint asset"), true);
	S.AddEnum(TEXT("detail_level"), TEXT("Verbosity: 'summary' (no warnings) or 'full' (includes warnings)"),
		{TEXT("summary"), TEXT("full")});
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_AnimGraphInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString DetailLevel = TEXT("full");
	Arguments->TryGetStringField(TEXT("detail_level"), DetailLevel);

	FString Error;
	UAnimBlueprint* AnimBP = ClaireonAnimGraphHelpers::LoadAnimBlueprint(AssetPath, Error);
	if (!AnimBP)
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AnimBP->GetPathName());
	Data->SetStringField(TEXT("asset_name"), AnimBP->GetName());

	// Class settings
	TSharedPtr<FJsonObject> ClassSettings = ClaireonAnimGraphHelpers::SerializeClassSettings(AnimBP);
	Data->SetObjectField(TEXT("class_settings"), ClassSettings);

	// Variables
	TArray<TSharedPtr<FJsonValue>> Variables = ClaireonAnimGraphHelpers::SerializeVariables(AnimBP);
	Data->SetArrayField(TEXT("variables"), Variables);
	Data->SetNumberField(TEXT("variable_count"), Variables.Num());

	// Functions
	TArray<TSharedPtr<FJsonValue>> Functions = ClaireonAnimGraphHelpers::SerializeFunctions(AnimBP);
	Data->SetArrayField(TEXT("functions"), Functions);
	Data->SetNumberField(TEXT("function_count"), Functions.Num());

	// All graphs
	TArray<ClaireonAnimGraphHelpers::FAnimGraphInfo> AllGraphs = ClaireonAnimGraphHelpers::CollectAllGraphs(AnimBP);
	TArray<TSharedPtr<FJsonValue>> GraphsArray;
	for (const ClaireonAnimGraphHelpers::FAnimGraphInfo& GraphInfo : AllGraphs)
	{
		TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
		GraphObj->SetStringField(TEXT("name"), GraphInfo.Name);
		GraphObj->SetStringField(TEXT("type"), GraphInfo.Type);
		GraphObj->SetNumberField(TEXT("node_count"), GraphInfo.NodeCount);
		if (!GraphInfo.ParentGraphName.IsEmpty())
		{
			GraphObj->SetStringField(TEXT("parent"), GraphInfo.ParentGraphName);
		}
		GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
	}
	Data->SetArrayField(TEXT("graphs"), GraphsArray);
	Data->SetNumberField(TEXT("graph_count"), GraphsArray.Num());

	// Warnings (full detail only)
	if (DetailLevel == TEXT("full"))
	{
		TSharedPtr<FJsonObject> Warnings = ClaireonAnimGraphHelpers::CollectWarnings(AnimBP);
		Data->SetObjectField(TEXT("warnings"), Warnings);

		int32 TotalWarnings = 0;
		double WarningCount = 0;
		if (Warnings->TryGetNumberField(TEXT("total_warning_count"), WarningCount))
		{
			TotalWarnings = static_cast<int32>(WarningCount);
		}
		Data->SetNumberField(TEXT("total_warning_count"), TotalWarnings);
	}

	FString Summary = FString::Printf(TEXT("AnimBP '%s': %d graphs, %d variables, %d functions"),
		*AnimBP->GetName(), AllGraphs.Num(), Variables.Num(), Functions.Num());

	return MakeSuccessResult(Data, Summary);
}
