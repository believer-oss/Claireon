// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_SetTransitionProperty.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_SetTransitionProperty::GetOperation() const { return TEXT("set_transition_property"); }

FString ClaireonStateTreeTool_SetTransitionProperty::GetDescription() const
{
	return TEXT("Set a struct field on an FStateTreeTransition (e.g. bConsumeEventOnSelect, bDelayTransition, DelayDuration, DelayRandomVariance, Priority, bTransitionEnabled, RequiredEvent.Tag, RequiredEvent.PayloadStruct). Bare bConsumeEventOnSelect is normalized to RequiredEvent.bConsumeEventOnSelect. Refuses Target/State/StateLink (those belong to statetree_modify_transition). Requires open session_id from statetree_open. Transactional.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_SetTransitionProperty::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state owning the transition."), true);
	Builder.AddString(TEXT("transition_id"), TEXT("GUID of the transition."), true);
	Builder.AddString(TEXT("property_name"), TEXT("Property name to set. Supports dot-paths (e.g. RequiredEvent.Tag)."), true);
	Builder.AddString(TEXT("property_value"), TEXT("Stringified property value (ImportText format)."), true);
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_SetTransitionProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString PropertyName, PropertyValue;
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName))
		return MakeErrorResult(TEXT("Missing parameter: property_name"));
	if (!Arguments->TryGetStringField(TEXT("property_value"), PropertyValue))
		return MakeErrorResult(TEXT("Missing parameter: property_value"));

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
	{
		const FString AssetName = Data->StateTree.IsValid() ? Data->StateTree->GetName() : TEXT("(unknown)");
		return MakeErrorResult(FString::Printf(TEXT("State '%s' not found in tree '%s'"),
			*StateId.ToString(EGuidFormats::DigitsWithHyphensLower),
			*AssetName));
	}

	FStateTreeTransition* Transition = ClaireonStateTreeHelpers::FindTransitionById(State, TransitionId);
	if (!Transition)
	{
		return MakeErrorResult(FString::Printf(TEXT("Transition '%s' not found on state '%s'"),
			*TransitionId.ToString(EGuidFormats::DigitsWithHyphensLower),
			*StateId.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Transition Property")));
	Data->StateTree->Modify();
	State->Modify();

	// Capture previous value (best-effort).
	FString PreviousValue;
	{
		FString CanonicalPath = PropertyName;
		if (CanonicalPath == TEXT("bConsumeEventOnSelect"))
		{
			CanonicalPath = TEXT("RequiredEvent.bConsumeEventOnSelect");
		}
		FProperty* PrevLeafProp = nullptr;
		void* PrevLeafAddr = nullptr;
		FString PrevError;
		if (ClaireonStateTreeHelpers::ResolvePropertyPath(FStateTreeTransition::StaticStruct(), Transition, CanonicalPath, PrevLeafProp, PrevLeafAddr, PrevError) && PrevLeafProp && PrevLeafAddr)
		{
			PrevLeafProp->ExportText_Direct(PreviousValue, PrevLeafAddr, PrevLeafAddr, /*OwnerObject=*/nullptr, PPF_None);
		}
	}

	if (!ClaireonStateTreeHelpers::SetTransitionProperty(*Transition, PropertyName, PropertyValue, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("set_transition_property -> Set %s=%s"), *PropertyName, *PropertyValue);

	FToolResult Response = BuildStateResponse(SessionId, Data);
	if (!Response.bIsError && Response.Data.IsValid())
	{
		TSharedPtr<FJsonObject> ExtraData = MakeShared<FJsonObject>();
		ExtraData->SetStringField(TEXT("state_id"), StateId.ToString(EGuidFormats::DigitsWithHyphensLower));
		ExtraData->SetStringField(TEXT("transition_id"), TransitionId.ToString(EGuidFormats::DigitsWithHyphensLower));
		ExtraData->SetStringField(TEXT("property_name"), PropertyName);
		ExtraData->SetStringField(TEXT("previous_value"), PreviousValue);
		Response.Data->SetObjectField(TEXT("set_transition_property"), ExtraData);
	}
	return Response;
}
