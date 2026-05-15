// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_SetNodeProperty.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_SetNodeProperty::GetName() const
{
	return TEXT("claireon.pcg_set_node_property");
}

FString ClaireonPCGGraphTool_SetNodeProperty::GetDescription() const
{
	return TEXT("Set a property on a PCG node's settings. The value is parsed as a string and coerced to the target property type.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_SetNodeProperty::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node"), TEXT("Node identifier (index or name)."), true);
	Builder.AddString(TEXT("property_name"), TEXT("Name of the property to set on the node's settings object."), true);
	Builder.AddString(TEXT("value"), TEXT("New value as a string (parsed/coerced to the property type)."), true);
	return Builder.Build();
}

FToolResult ClaireonPCGGraphTool_SetNodeProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FPCGGraphEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString NodeIdentifier, PropertyName, Value;
	if (!Arguments->TryGetStringField(TEXT("node"), NodeIdentifier) || NodeIdentifier.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: node"));
	}
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_name"));
	}
	if (!Arguments->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	int32 NodeIndex;
	UPCGNode* Node = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Data->PCGGraph.Get(), NodeIdentifier, NodeIndex);
	if (!Node)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeIdentifier));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set PCG Node Property")));

	if (!ClaireonPCGGraphHelpers::SetNodeProperty(Node, PropertyName, Value, Error))
	{
		return MakeErrorResult(Error);
	}

	ClaireonPCGGraphHelpers::NotifyGraphChanged(Data->PCGGraph.Get());

	Data->LastOperationStatus = FString::Printf(TEXT("Set %s.%s = %s"),
		*ClaireonPCGGraphHelpers::GetNodeDisplayName(Node), *PropertyName, *Value);

	return BuildStateResponse(SessionId, Data);
}
