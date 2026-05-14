// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_SetSchemaProperty.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeSchema.h"
#include "StateTreePropertyBindings.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_SetSchemaProperty::GetName() const
{
	return TEXT("claireon.statetree_set_schema_property");
}

FString ClaireonStateTreeTool_SetSchemaProperty::GetDescription() const
{
	return TEXT("Set a property on the State Tree's Schema (e.g. ActorClass, bReflectActorComponents, bReflectActorProperties, PrefabAsset). Requires open session_id from claireon.statetree_open. Transactional. After writing via ImportText_Direct, dispatches Schema->PostEditChangeChainProperty so subclass overrides (e.g. UFSSampleStateTreeSchema::UpdateContextDataDescs) re-run their reflection updaters and regenerate deterministic context-data GUIDs. Response includes a refreshed context_data_descs array so callers can immediately diff.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_SetSchemaProperty::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("property_name"), TEXT("Property name on the Schema UClass. Supports dot-paths."), true);
	Builder.AddString(TEXT("property_value"), TEXT("Stringified property value (ImportText format)."), true);
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_SetSchemaProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	UStateTreeSchema* Schema = EditorData->Schema;
	if (!Schema)
		return MakeErrorResult(TEXT("State-tree has no schema; cannot set schema property."));

	FString PropertyName, PropertyValue;
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName))
		return MakeErrorResult(TEXT("Missing parameter: property_name"));
	if (!Arguments->TryGetStringField(TEXT("property_value"), PropertyValue))
		return MakeErrorResult(TEXT("Missing parameter: property_value"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Schema Property")));
	Data->StateTree->Modify();
	Schema->Modify();

	// Capture previous value (best-effort).
	FString PreviousValue;
	{
		FProperty* PrevLeafProp = nullptr;
		void* PrevLeafAddr = nullptr;
		FString PrevError;
		if (ClaireonStateTreeHelpers::ResolvePropertyPath(Schema->GetClass(), Schema, PropertyName, PrevLeafProp, PrevLeafAddr, PrevError) && PrevLeafProp && PrevLeafAddr)
		{
			PrevLeafProp->ExportText_Direct(PreviousValue, PrevLeafAddr, PrevLeafAddr, /*OwnerObject=*/Schema, PPF_None);
		}
	}

	if (!ClaireonStateTreeHelpers::SetSchemaProperty(*Schema, PropertyName, PropertyValue, Error))
	{
		return MakeErrorResult(Error);
	}

	// Dispatch PostEditChangeChainProperty so subclass overrides (e.g. UFSSampleStateTreeSchema)
	// regenerate deterministic context-data GUIDs.
	{
		FProperty* LeafProperty = nullptr;
		void* LeafAddress = nullptr;
		FString IgnoreErr;
		if (ClaireonStateTreeHelpers::ResolvePropertyPath(Schema->GetClass(), Schema, PropertyName, LeafProperty, LeafAddress, IgnoreErr) && LeafProperty)
		{
			FEditPropertyChain PropertyChain;
			PropertyChain.AddHead(LeafProperty);
			PropertyChain.SetActivePropertyNode(LeafProperty);
			FPropertyChangedEvent BaseEvent(LeafProperty, EPropertyChangeType::ValueSet);
			FPropertyChangedChainEvent ChainEvent(PropertyChain, BaseEvent);
			Schema->PostEditChangeChainProperty(ChainEvent);
		}
	}

	// Re-walk context-data descs (mirror F3's emission shape).
	TArray<TSharedPtr<FJsonValue>> ContextArray;
	EditorData->VisitGlobalNodes(
		[&ContextArray](const UStateTreeState* /*State*/,
						const FStateTreeBindableStructDesc& Desc,
						const FStateTreeDataView /*Value*/) -> EStateTreeVisitor
		{
			if (Desc.DataSource == EStateTreeBindableStructSource::Context)
			{
				ContextArray.Add(MakeShared<FJsonValueObject>(
					ClaireonStateTreeHelpers::EmitBindingSourceRecord(Desc)));
			}
			return EStateTreeVisitor::Continue;
		});

	Data->LastOperationStatus = FString::Printf(TEXT("set_schema_property -> Set %s=%s"), *PropertyName, *PropertyValue);

	FToolResult Response = BuildStateResponse(SessionId, Data);
	if (!Response.bIsError && Response.Data.IsValid())
	{
		TSharedPtr<FJsonObject> ExtraData = MakeShared<FJsonObject>();
		ExtraData->SetStringField(TEXT("property_name"), PropertyName);
		ExtraData->SetStringField(TEXT("previous_value"), PreviousValue);
		ExtraData->SetArrayField(TEXT("context_data_descs"), ContextArray);
		Response.Data->SetObjectField(TEXT("set_schema_property"), ExtraData);
	}
	return Response;
}
