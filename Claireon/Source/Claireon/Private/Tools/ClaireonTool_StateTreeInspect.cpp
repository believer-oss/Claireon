// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_StateTreeInspect.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "ClaireonLog.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_StateTreeInspect::GetName() const
{
	return TEXT("inspect_statetree");
}

FString ClaireonTool_StateTreeInspect::GetCategory() const
{
	return TEXT("statetree");
}

FString ClaireonTool_StateTreeInspect::GetDescription() const
{
	return TEXT("Read the full structure of a State Tree asset as structured text. "
				"Displays states, tasks, conditions, transitions, evaluators, considerations, parameters, and property bindings. "
				"Use detail_level='summary' for a compact overview or 'full' for complete property details. "
				"Optionally inspect a single state by providing state_id.");
}

TSharedPtr<FJsonObject> ClaireonTool_StateTreeInspect::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Unreal asset path to the State Tree (e.g. /Game/AI/ST_MobBehavior)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// detail_level - optional
	TSharedPtr<FJsonObject> DetailProp = MakeShared<FJsonObject>();
	DetailProp->SetStringField(TEXT("type"), TEXT("string"));
	DetailProp->SetStringField(TEXT("description"), TEXT("Level of detail: 'summary' for compact overview, 'full' for complete property details (default: full)"));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("summary")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("full")));
		DetailProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("detail_level"), DetailProp);

	// state_id - optional
	TSharedPtr<FJsonObject> StateIdProp = MakeShared<FJsonObject>();
	StateIdProp->SetStringField(TEXT("type"), TEXT("string"));
	StateIdProp->SetStringField(TEXT("description"), TEXT("Optional GUID of a specific state to inspect (shows only that state and its children)"));
	Properties->SetObjectField(TEXT("state_id"), StateIdProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_StateTreeInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString LoadError;
	UStateTree* StateTree = ClaireonStateTreeHelpers::LoadStateTreeAsset(AssetPath, LoadError);
	if (!StateTree)
	{
		return MakeErrorResult(LoadError);
	}

	FString EditorError;
	UStateTreeEditorData* EditorData = ClaireonStateTreeHelpers::GetEditorData(StateTree, EditorError);
	if (!EditorData)
	{
		return MakeErrorResult(EditorError);
	}

	// Optional focus state
	FGuid FocusStateId;
	FString StateIdStr;
	if (Arguments->TryGetStringField(TEXT("state_id"), StateIdStr) && !StateIdStr.IsEmpty())
	{
		FGuid::Parse(StateIdStr, FocusStateId);
	}

	// Count states and transitions recursively
	TFunction<void(const TArray<TObjectPtr<UStateTreeState>>&, int32&, int32&)> CountStates;
	CountStates = [&CountStates](const TArray<TObjectPtr<UStateTreeState>>& States, int32& OutStateCount, int32& OutTransitionCount)
	{
		for (UStateTreeState* State : States)
		{
			if (!State)
			{
				continue;
			}
			++OutStateCount;
			OutTransitionCount += State->Transitions.Num();
			CountStates(State->Children, OutStateCount, OutTransitionCount);
		}
	};

	int32 StateCount = 0;
	int32 TransitionCount = 0;
	CountStates(EditorData->SubTrees, StateCount, TransitionCount);

	// Build full text structure
	const FGuid* FocusPtr = FocusStateId.IsValid() ? &FocusStateId : nullptr;
	const FString StructureText = ClaireonStateTreeHelpers::FormatStateTreeStructure(EditorData, FocusPtr);

	const FString AssetName = FPaths::GetBaseFilename(AssetPath);
	const int32 EvaluatorCount = EditorData->Evaluators.Num();

	// Build structured data
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetNumberField(TEXT("state_count"), StateCount);
	Data->SetNumberField(TEXT("transition_count"), TransitionCount);
	Data->SetNumberField(TEXT("evaluator_count"), EvaluatorCount);
	Data->SetStringField(TEXT("structure"), StructureText);

	const FString Summary = FString::Printf(TEXT("%s: %d states, %d transitions"),
		*AssetName, StateCount, TransitionCount);

	return MakeSuccessResult(Data, Summary);
}
