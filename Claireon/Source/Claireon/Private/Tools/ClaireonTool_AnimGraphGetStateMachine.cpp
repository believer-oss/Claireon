// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AnimGraphGetStateMachine.h"
#include "Tools/ClaireonAnimGraphHelpers.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "Animation/AnimBlueprint.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateTransitionNode.h"
#include "EdGraph/EdGraph.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_AnimGraphGetStateMachine::GetCategory() const { return TEXT("animbp"); }
FString ClaireonTool_AnimGraphGetStateMachine::GetOperation() const { return TEXT("get_state_machine"); }

FString ClaireonTool_AnimGraphGetStateMachine::GetDescription() const
{
	return TEXT("Inspect a state machine's topology within an Animation Blueprint. Returns the entry state, "
		"all states (with bound graph info), all transitions (with blend mode, duration, priority), "
		"and conduits. Use animbp_inspect to discover state machine names.");
}

TSharedPtr<FJsonObject> ClaireonTool_AnimGraphGetStateMachine::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the Animation Blueprint asset"), true);
	S.AddString(TEXT("state_machine_name"), TEXT("Name of the state machine graph (from animbp_inspect)"), true);
	S.AddBoolean(TEXT("include_transition_details"), TEXT("Include full transition details (blend settings, condition graph). Default: false"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_AnimGraphGetStateMachine::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString SMName;
	if (!Arguments->TryGetStringField(TEXT("state_machine_name"), SMName) || SMName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: state_machine_name"));
	}

	bool bIncludeTransDetails = false;
	Arguments->TryGetBoolField(TEXT("include_transition_details"), bIncludeTransDetails);

	FString Error;
	UAnimBlueprint* AnimBP = ClaireonAnimGraphHelpers::LoadAnimBlueprint(AssetPath, Error);
	if (!AnimBP)
	{
		return MakeErrorResult(Error);
	}

	// Find the state machine graph
	UEdGraph* Graph = ClaireonAnimGraphHelpers::FindAnimGraphByName(AnimBP, SMName, Error);
	if (!Graph)
	{
		return MakeErrorResult(Error);
	}

	UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(Graph);
	if (!SMGraph)
	{
		return MakeErrorResult(FString::Printf(TEXT("Graph '%s' is not a State Machine graph (actual type: %s). "
			"Use animbp_inspect to find state machine graphs."), *SMName, *Graph->GetClass()->GetName()));
	}

	// Serialize state machine topology
	TSharedPtr<FJsonObject> Data = ClaireonAnimGraphHelpers::SerializeStateMachine(SMGraph);

	// Optionally include full transition details
	if (bIncludeTransDetails)
	{
		TArray<TSharedPtr<FJsonValue>> DetailedTransitions;
		for (UEdGraphNode* Node : SMGraph->Nodes)
		{
			if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(Node))
			{
				DetailedTransitions.Add(MakeShared<FJsonValueObject>(
					ClaireonAnimGraphHelpers::SerializeTransition(TransNode)));
			}
		}
		Data->SetArrayField(TEXT("transition_details"), DetailedTransitions);
	}

	double StateCount = 0;
	double TransCount = 0;
	Data->TryGetNumberField(TEXT("state_count"), StateCount);
	Data->TryGetNumberField(TEXT("transition_count"), TransCount);

	FString Summary = FString::Printf(TEXT("State Machine '%s': %d states, %d transitions"),
		*SMName, static_cast<int32>(StateCount), static_cast<int32>(TransCount));

	return MakeSuccessResult(Data, Summary);
}
