// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_AddTransition.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"
#include "GameplayTagContainer.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_AddTransition::GetOperation() const { return TEXT("add_transition"); }

FString ClaireonStateTreeTool_AddTransition::GetDescription() const
{
	return TEXT("Add a transition to a state in the open State Tree editing session. Requires open session_id from statetree_open. Transactional. Transitions fire on a trigger (OnTaskCompleted, OnEvent, OnStateCompleted, etc.) and route execution to a target state. Returns the new transition GUID.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_AddTransition::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state."), true);
	Builder.AddString(TEXT("trigger"), TEXT("Trigger: OnStateCompleted, OnStateSucceeded, OnStateFailed, OnTick, OnEvent, None."), true);
	Builder.AddString(TEXT("target_type"), TEXT("target_type: enum (GotoState | NextState | NextSelectableState | Succeeded | Failed | None)"), true);
	Builder.AddString(TEXT("target_state_id"), TEXT("Target state GUID (required when target_type is GotoState)."));
	Builder.AddString(TEXT("event_tag"), TEXT("Gameplay tag (used when trigger is OnEvent)."));
	Builder.AddString(TEXT("priority"), TEXT("Priority: None, Low, Normal, Medium, High, Critical."));
	Builder.AddBoolean(TEXT("enabled"), TEXT("Whether the transition is enabled. Defaults to true."));
	Builder.AddNumber(TEXT("delay"), TEXT("Optional delay in seconds."));
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_AddTransition::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FStateTreeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	UStateTreeEditorData* EditorData = ClaireonStateTreeEditInternal::GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid StateId;
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);

	FString TriggerStr, TargetTypeStr;
	if (!Arguments->TryGetStringField(TEXT("trigger"), TriggerStr))
		return MakeErrorResult(TEXT("Missing parameter: trigger"));
	if (!Arguments->TryGetStringField(TEXT("target_type"), TargetTypeStr))
		return MakeErrorResult(TEXT("Missing parameter: target_type"));

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	EStateTreeTransitionTrigger Trigger = ClaireonStateTreeEditInternal::ParseTransitionTrigger(TriggerStr);
	TOptional<EStateTreeTransitionType> ParsedType = ClaireonStateTreeEditInternal::TryParseTransitionType(TargetTypeStr);
	if (!ParsedType.IsSet())
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Unknown target_type: '%s'. Valid: GotoState, NextState, NextSelectableState, Succeeded, Failed, None"),
			*TargetTypeStr));
	}
	const EStateTreeTransitionType TransType = ParsedType.GetValue();

	// target_state_id only meaningful when target_type=GotoState. Reject silent drop.
	{
		FString TargetStateIdProbe;
		const bool bHasTargetStateId =
			Arguments->TryGetStringField(TEXT("target_state_id"), TargetStateIdProbe) && !TargetStateIdProbe.IsEmpty();
		if (bHasTargetStateId && TransType != EStateTreeTransitionType::GotoState)
		{
			return MakeErrorResult(TEXT("target_state_id requires target_type=GotoState"));
		}
	}

	// Resolve target state if GotoState
	UStateTreeState* TargetState = nullptr;
	if (TransType == EStateTreeTransitionType::GotoState)
	{
		FGuid TargetStateId;
		if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("target_state_id"), TargetStateId, Error))
		{
			return MakeErrorResult(TEXT("target_state_id required when target_type is GotoState"));
		}
		TargetState = ClaireonStateTreeHelpers::FindStateById(EditorData, TargetStateId);
		if (!TargetState)
			return MakeErrorResult(TEXT("Target state not found"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Transition")));
	Data->StateTree->Modify();

	FStateTreeTransition& NewTransition = State->AddTransition(Trigger, TransType, TargetState);

	// Set event tag if OnEvent trigger
	FString EventTag;
	if (Arguments->TryGetStringField(TEXT("event_tag"), EventTag) && !EventTag.IsEmpty())
	{
		NewTransition.RequiredEvent.Tag = FGameplayTag::RequestGameplayTag(FName(*EventTag), false);
	}

	// Set priority
	FString PriorityStr;
	if (Arguments->TryGetStringField(TEXT("priority"), PriorityStr))
	{
		NewTransition.Priority = ClaireonStateTreeEditInternal::ParseTransitionPriority(PriorityStr);
	}

	// Set enabled
	bool bEnabled = true;
	if (Arguments->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		NewTransition.bTransitionEnabled = bEnabled;
	}

	// Set delay
	double Delay = 0.0;
	if (Arguments->TryGetNumberField(TEXT("delay"), Delay) && Delay > 0.0)
	{
		NewTransition.bDelayTransition = true;
		NewTransition.DelayDuration = static_cast<float>(Delay);
	}

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("add_transition -> Added %s transition to '%s'"), *TriggerStr, *State->Name.ToString());

	const FString NewIdStr = NewTransition.ID.ToString(EGuidFormats::DigitsWithHyphens);
	return BuildStateResponse(SessionId, Data, FStringView(TEXT("transition_id")), FStringView(NewIdStr));
}
