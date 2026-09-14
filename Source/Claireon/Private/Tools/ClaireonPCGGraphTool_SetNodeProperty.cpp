// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_SetNodeProperty.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_SetNodeProperty::GetOperation() const { return TEXT("set_node_property"); }

FString ClaireonPCGGraphTool_SetNodeProperty::GetDescription() const
{
	return TEXT("Set a property on a PCG node's settings in an open editing session. property_name is a "
				"dotted, optionally subscripted PATH, not a bare name: most of a node's configuration lives "
				"below the settings root, such as MeshSelectorParameters.MeshEntries. Deprecated properties "
				"are refused; nothing reads them. Placed components keep generating from the cached compiled "
				"graph until pcg_refresh evicts it.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_SetNodeProperty::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node"), TEXT("Node identifier (index or name)."), true);
	Builder.AddString(TEXT("property_name"),
		TEXT("Dotted property path on the node's settings object, with optional [N] array "
			 "subscripts. A bare name addresses the settings root; nest to reach sub-objects and "
			 "structs, e.g. 'MeshSelectorParameters.MeshEntries', 'Parameters.PruningType', "
			 "'MeshSelectorParameters.MeshEntries[0].Weight'. pcg_get_node_properties shows the "
			 "effective layout."), true);
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
	if (!IsValid(Node))
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeIdentifier));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set PCG Node Property")));

	EPCGChangeType ChangeType = EPCGChangeType::None;
	if (!ClaireonPCGGraphHelpers::SetNodeProperty(Node, PropertyName, Value, Error, ChangeType))
	{
		return MakeErrorResult(Error);
	}

	// Use engine-derived flags and request editor reconstruction through the shared path.
	ClaireonPCGGraphHelpers::NotifyGraphChanged(Data->PCGGraph.Get(), ChangeType);

	Data->LastOperationStatus = FString::Printf(TEXT("Set %s.%s = %s (%s)"),
		*ClaireonPCGGraphHelpers::GetNodeDisplayName(Node), *PropertyName, *Value,
		*ClaireonPCGGraphHelpers::ChangeTypeToString(ChangeType));

	FToolResult Result = BuildStateResponse(SessionId, Data);
	if (Result.Data.IsValid())
	{
		Result.Data->SetStringField(TEXT("change_type"), ClaireonPCGGraphHelpers::ChangeTypeToString(ChangeType));
	}
	return Result;
}
