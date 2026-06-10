// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_StateTreeInspect.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "ClaireonLog.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_StateTreeInspect::GetCategory() const { return TEXT("statetree"); }
FString ClaireonTool_StateTreeInspect::GetOperation() const { return TEXT("inspect"); }

FString ClaireonTool_StateTreeInspect::GetDescription() const
{
	return TEXT("Read the full structure of a State Tree asset by path. Stateless / read-only / non-session: never mutates and requires no open session. Returns states, tasks, conditions, transitions, evaluators, considerations, parameters, and bindings. Use detail_level='summary' or 'full'; optionally pass state_id to inspect a single state.");
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

	// sections - optional array filter
	TSharedPtr<FJsonObject> SectionsProp = MakeShared<FJsonObject>();
	SectionsProp->SetStringField(TEXT("type"), TEXT("array"));
	SectionsProp->SetStringField(TEXT("description"), TEXT("Optional array of section names to include in the formatted structure. Empty/omitted emits all four. Recognized: global_evaluators, global_tasks, states, bindings."));
	{
		TSharedPtr<FJsonObject> ItemsObj = MakeShared<FJsonObject>();
		ItemsObj->SetStringField(TEXT("type"), TEXT("string"));
		TArray<TSharedPtr<FJsonValue>> SectionEnum;
		SectionEnum.Add(MakeShared<FJsonValueString>(TEXT("global_evaluators")));
		SectionEnum.Add(MakeShared<FJsonValueString>(TEXT("global_tasks")));
		SectionEnum.Add(MakeShared<FJsonValueString>(TEXT("states")));
		SectionEnum.Add(MakeShared<FJsonValueString>(TEXT("bindings")));
		ItemsObj->SetArrayField(TEXT("enum"), SectionEnum);
		SectionsProp->SetObjectField(TEXT("items"), ItemsObj);
	}
	Properties->SetObjectField(TEXT("sections"), SectionsProp);

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

	// Optional sections filter (default: all four). Unknown names ignored silently.
	TArray<FString> Sections;
	bool bHasSections = false;
	const TArray<TSharedPtr<FJsonValue>>* SectionsArray = nullptr;
	if (Arguments->TryGetArrayField(TEXT("sections"), SectionsArray) && SectionsArray)
	{
		bHasSections = true;
		for (const TSharedPtr<FJsonValue>& Value : *SectionsArray)
		{
			FString Entry;
			if (Value.IsValid() && Value->TryGetString(Entry) && !Entry.IsEmpty())
			{
				Sections.Add(Entry);
			}
		}
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
	const TArray<FString>* SectionsPtr = bHasSections ? &Sections : nullptr;
	const FString StructureText = ClaireonStateTreeHelpers::FormatStateTreeStructure(EditorData, FocusPtr, SectionsPtr);

	const FString AssetName = FPaths::GetBaseFilename(AssetPath);
	const int32 EvaluatorCount = EditorData->Evaluators.Num();

	// Build structured data
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetNumberField(TEXT("state_count"), StateCount);
	Data->SetNumberField(TEXT("transition_count"), TransitionCount);
	Data->SetNumberField(TEXT("evaluator_count"), EvaluatorCount);
	ClaireonStateTreeEditInternal::ApplyStructuredSpill(
		*Data, TEXT("structure"), TEXT("structure_full"), StructureText);

	const FString Summary = FString::Printf(TEXT("%s: %d states, %d transitions"),
		*AssetName, StateCount, TransitionCount);

	return MakeSuccessResult(Data, Summary);
}
