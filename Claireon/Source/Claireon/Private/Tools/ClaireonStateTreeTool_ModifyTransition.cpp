// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_ModifyTransition.h"
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

FString ClaireonStateTreeTool_ModifyTransition::GetOperation() const { return TEXT("modify_transition"); }

FString ClaireonStateTreeTool_ModifyTransition::GetDescription() const
{
	return TEXT("Modify an existing transition's trigger, target, priority, enabled state, or event tag in the open State Tree editing session. Requires open session_id from statetree_open. Transactional. Omitted fields are left unchanged. Common pitfall: target_state_id must reference an existing state on the same tree.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_ModifyTransition::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state."), true);
	Builder.AddString(TEXT("transition_id"), TEXT("GUID of the transition."), true);
	Builder.AddString(TEXT("trigger"), TEXT("Optional new trigger."));
	Builder.AddString(TEXT("event_tag"), TEXT("Optional gameplay tag."));
	Builder.AddString(TEXT("priority"), TEXT("Optional priority."));
	Builder.AddBoolean(TEXT("enabled"), TEXT("Optional enabled flag."));
	Builder.AddString(TEXT("target_type"), TEXT("target_type: enum (GotoState | NextState | NextSelectableState | Succeeded | Failed | None)"));
	Builder.AddString(TEXT("target_state_id"), TEXT("Optional target state GUID (when target_type is GotoState)."));
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_ModifyTransition::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FGuid StateId, TransitionId;
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("transition_id"), TransitionId, Error))
		return MakeErrorResult(Error);

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	FStateTreeTransition* Trans = ClaireonStateTreeHelpers::FindTransitionById(State, TransitionId);
	if (!Trans)
		return MakeErrorResult(TEXT("Transition not found"));

	// Parse target_type up front (strict) and apply D3 check before mutating state.
	FString TargetTypeStr;
	const bool bHasTargetType = Arguments->TryGetStringField(TEXT("target_type"), TargetTypeStr);
	TOptional<EStateTreeTransitionType> ParsedType;
	if (bHasTargetType)
	{
		ParsedType = ClaireonStateTreeEditInternal::TryParseTransitionType(TargetTypeStr);
		if (!ParsedType.IsSet())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Unknown target_type: '%s'. Valid: GotoState, NextState, NextSelectableState, Succeeded, Failed, None"),
				*TargetTypeStr));
		}
	}

	{
		FString TargetStateIdProbe;
		const bool bHasTargetStateId =
			Arguments->TryGetStringField(TEXT("target_state_id"), TargetStateIdProbe) && !TargetStateIdProbe.IsEmpty();
		if (bHasTargetStateId)
		{
			// D3: target_state_id only meaningful when target_type=GotoState was supplied
			// alongside it. There is intentionally no accept-if-existing-is-GotoState path.
			if (!ParsedType.IsSet() || ParsedType.GetValue() != EStateTreeTransitionType::GotoState)
			{
				return MakeErrorResult(TEXT("target_state_id requires target_type=GotoState"));
			}
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Modify Transition")));
	Data->StateTree->Modify();

	FString TriggerStr;
	if (Arguments->TryGetStringField(TEXT("trigger"), TriggerStr))
	{
		Trans->Trigger = ClaireonStateTreeEditInternal::ParseTransitionTrigger(TriggerStr);
	}

	FString EventTag;
	if (Arguments->TryGetStringField(TEXT("event_tag"), EventTag))
	{
		Trans->RequiredEvent.Tag = FGameplayTag::RequestGameplayTag(FName(*EventTag), false);
	}

	FString PriorityStr;
	if (Arguments->TryGetStringField(TEXT("priority"), PriorityStr))
	{
		Trans->Priority = ClaireonStateTreeEditInternal::ParseTransitionPriority(PriorityStr);
	}

	bool bEnabled;
	if (Arguments->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		Trans->bTransitionEnabled = bEnabled;
	}

#if WITH_EDITORONLY_DATA
	if (bHasTargetType)
	{
		Trans->State.LinkType = ParsedType.GetValue();

		if (Trans->State.LinkType == EStateTreeTransitionType::GotoState)
		{
			FGuid TargetStateId;
			if (ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("target_state_id"), TargetStateId, Error))
			{
				UStateTreeState* TargetState = ClaireonStateTreeHelpers::FindStateById(EditorData, TargetStateId);
				if (TargetState)
				{
					Trans->State.ID = TargetState->ID;
					Trans->State.Name = TargetState->Name;
				}
			}
		}
	}
#endif

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = TEXT("modify_transition -> Modified transition");
	return BuildStateResponse(SessionId, Data);
}
