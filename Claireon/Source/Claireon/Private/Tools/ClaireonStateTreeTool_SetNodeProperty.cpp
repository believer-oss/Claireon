// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_SetNodeProperty.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_SetNodeProperty::GetName() const
{
	return TEXT("claireon.statetree_set_node_property");
}

FString ClaireonStateTreeTool_SetNodeProperty::GetDescription() const
{
	return TEXT("Set a property on a node (task, condition, consideration, evaluator, etc.) in the open State Tree editing session. Requires open session_id from claireon.statetree_open. Transactional. Common pitfall: property_name must match the UPROPERTY name on the node's struct; nested paths use dot notation.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_SetNodeProperty::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node_id"), TEXT("GUID of the node."), true);
	Builder.AddString(TEXT("property_name"), TEXT("Property name to set."), true);
	Builder.AddString(TEXT("property_value"), TEXT("Stringified property value."), true);
	Builder.AddBoolean(TEXT("on_instance_data"), TEXT("True to set on instance data; false (default) to set on the node struct."));
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_SetNodeProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FGuid NodeId;
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("node_id"), NodeId, Error))
		return MakeErrorResult(Error);

	FString PropertyName, PropertyValue;
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName))
		return MakeErrorResult(TEXT("Missing parameter: property_name"));
	if (!Arguments->TryGetStringField(TEXT("property_value"), PropertyValue))
		return MakeErrorResult(TEXT("Missing parameter: property_value"));

	bool bOnInstanceData = false;
	Arguments->TryGetBoolField(TEXT("on_instance_data"), bOnInstanceData);

	FStateTreeEditorNode* Node = ClaireonStateTreeHelpers::FindNodeById(EditorData, NodeId);
	if (!Node)
		return MakeErrorResult(TEXT("Node not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Node Property")));
	Data->StateTree->Modify();

	if (!ClaireonStateTreeHelpers::SetNodeProperty(*Node, PropertyName, PropertyValue, bOnInstanceData, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("set_node_property -> Set %s=%s"), *PropertyName, *PropertyValue);
	return BuildStateResponse(SessionId, Data);
}
