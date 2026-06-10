// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AnimGraphGetTransition.h"
#include "Tools/ClaireonAnimGraphHelpers.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "Animation/AnimBlueprint.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateNodeBase.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_AnimGraphGetTransition::GetCategory() const { return TEXT("animbp"); }
FString ClaireonTool_AnimGraphGetTransition::GetOperation() const { return TEXT("get_transition"); }

FString ClaireonTool_AnimGraphGetTransition::GetDescription() const
{
	return TEXT("Deep inspection of a specific transition in a state machine. Returns crossfade mode, "
		"duration, blend profile, priority, bidirectional flag, logic type, condition graph (with all "
		"nodes), and custom blend graph if present. Identify by from_state+to_state or transition_index.");
}

TSharedPtr<FJsonObject> ClaireonTool_AnimGraphGetTransition::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the Animation Blueprint asset"), true);
	S.AddString(TEXT("state_machine_name"), TEXT("Name of the state machine graph"), true);
	S.AddString(TEXT("from_state"), TEXT("Source state name (use with to_state)"));
	S.AddString(TEXT("to_state"), TEXT("Target state name (use with from_state)"));
	S.AddInteger(TEXT("transition_index"), TEXT("Alternative: index of the transition (0-based, from animbp_get_state_machine)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_AnimGraphGetTransition::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString FromState, ToState;
	Arguments->TryGetStringField(TEXT("from_state"), FromState);
	Arguments->TryGetStringField(TEXT("to_state"), ToState);

	int32 TransitionIndex = -1;
	if (Arguments->HasField(TEXT("transition_index")))
	{
		TransitionIndex = static_cast<int32>(Arguments->GetNumberField(TEXT("transition_index")));
	}

	// Validate: need either from+to or index
	if (FromState.IsEmpty() && ToState.IsEmpty() && TransitionIndex < 0)
	{
		return MakeErrorResult(TEXT("Must provide either from_state+to_state or transition_index"));
	}

	FString Error;
	UAnimBlueprint* AnimBP = ClaireonAnimGraphHelpers::LoadAnimBlueprint(AssetPath, Error);
	if (!AnimBP)
	{
		return MakeErrorResult(Error);
	}

	UEdGraph* Graph = ClaireonAnimGraphHelpers::FindAnimGraphByName(AnimBP, SMName, Error);
	if (!Graph)
	{
		return MakeErrorResult(Error);
	}

	UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(Graph);
	if (!SMGraph)
	{
		return MakeErrorResult(FString::Printf(TEXT("'%s' is not a State Machine graph"), *SMName));
	}

	// Find the transition
	UAnimStateTransitionNode* FoundTransition = nullptr;
	TArray<UAnimStateTransitionNode*> AllTransitions;

	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(Node))
		{
			AllTransitions.Add(TransNode);
		}
	}

	if (TransitionIndex >= 0)
	{
		// Find by index
		if (TransitionIndex >= AllTransitions.Num())
		{
			return MakeErrorResult(FString::Printf(TEXT("Transition index %d out of bounds (total transitions: %d)"),
				TransitionIndex, AllTransitions.Num()));
		}
		FoundTransition = AllTransitions[TransitionIndex];
	}
	else
	{
		// Find by from_state + to_state names
		for (UAnimStateTransitionNode* TransNode : AllTransitions)
		{
			UAnimStateNodeBase* PrevState = TransNode->GetPreviousState();
			UAnimStateNodeBase* NextState = TransNode->GetNextState();

			FString PrevName = PrevState ? PrevState->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("");
			FString NextName = NextState ? NextState->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("");

			if (PrevName.Equals(FromState, ESearchCase::IgnoreCase) &&
				NextName.Equals(ToState, ESearchCase::IgnoreCase))
			{
				FoundTransition = TransNode;
				break;
			}
		}

		if (!FoundTransition)
		{
			// Build error with available transitions
			FString Available;
			for (UAnimStateTransitionNode* TransNode : AllTransitions)
			{
				UAnimStateNodeBase* PrevState = TransNode->GetPreviousState();
				UAnimStateNodeBase* NextState = TransNode->GetNextState();
				FString PrevName = PrevState ? PrevState->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("?");
				FString NextName = NextState ? NextState->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("?");
				if (!Available.IsEmpty())
				{
					Available += TEXT(", ");
				}
				Available += FString::Printf(TEXT("'%s' -> '%s'"), *PrevName, *NextName);
			}
			return MakeErrorResult(FString::Printf(TEXT("Transition from '%s' to '%s' not found. Available: %s"),
				*FromState, *ToState, *Available));
		}
	}

	TSharedPtr<FJsonObject> Data = ClaireonAnimGraphHelpers::SerializeTransition(FoundTransition);

	UAnimStateNodeBase* PrevState = FoundTransition->GetPreviousState();
	UAnimStateNodeBase* NextState = FoundTransition->GetNextState();
	FString PrevName = PrevState ? PrevState->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("?");
	FString NextName = NextState ? NextState->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("?");

	FString Summary = FString::Printf(TEXT("Transition '%s' -> '%s': duration=%.2f"),
		*PrevName, *NextName, FoundTransition->CrossfadeDuration);

	return MakeSuccessResult(Data, Summary);
}
