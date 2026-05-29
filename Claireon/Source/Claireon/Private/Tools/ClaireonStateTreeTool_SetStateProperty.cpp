// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_SetStateProperty.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_SetStateProperty::GetOperation() const { return TEXT("set_state_property"); }

FString ClaireonStateTreeTool_SetStateProperty::GetDescription() const
{
    return TEXT("Set a top-level field on a UStateTreeState (e.g. bHasRequiredEventToEnter, RequiredEventToEnter.Tag, RequiredEventToEnter.PayloadStruct, bEnabled, Type, SelectionBehavior, Weight). Requires open session_id from statetree_open. Transactional. property_name supports dot-paths into nested structs. Color/Parameters.Parameters are not supported in v1.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_SetStateProperty::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state."), true);
	Builder.AddString(TEXT("property_name"), TEXT("Property name to set. Supports dot-paths (e.g. RequiredEventToEnter.Tag)."), true);
	Builder.AddString(TEXT("property_value"), TEXT("Stringified property value (ImportText format)."), true);
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_SetStateProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set State Property")));
	Data->StateTree->Modify();
	State->Modify();

	// Capture previous value (best-effort -- skip silently if path is unresolvable).
	FString PreviousValue;
	{
		FProperty* PrevLeafProp = nullptr;
		void* PrevLeafAddr = nullptr;
		FString PrevError;
		if (ClaireonStateTreeHelpers::ResolvePropertyPath(UStateTreeState::StaticClass(), State, PropertyName, PrevLeafProp, PrevLeafAddr, PrevError) && PrevLeafProp && PrevLeafAddr)
		{
			PrevLeafProp->ExportText_Direct(PreviousValue, PrevLeafAddr, PrevLeafAddr, /*OwnerObject=*/State, PPF_None);
		}
	}

	if (!ClaireonStateTreeHelpers::SetStateProperty(*State, PropertyName, PropertyValue, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("set_state_property -> Set %s=%s"), *PropertyName, *PropertyValue);

	FToolResult Response = BuildStateResponse(SessionId, Data);
	if (!Response.bIsError && Response.Data.IsValid())
	{
		TSharedPtr<FJsonObject> ExtraData = MakeShared<FJsonObject>();
		ExtraData->SetStringField(TEXT("state_id"), StateId.ToString(EGuidFormats::DigitsWithHyphensLower));
		ExtraData->SetStringField(TEXT("property_name"), PropertyName);
		ExtraData->SetStringField(TEXT("previous_value"), PreviousValue);
		Response.Data->SetObjectField(TEXT("set_state_property"), ExtraData);
	}
	return Response;
}
