// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_GetNodeProperties.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "PCGGraph.h"
#include "PCGNode.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_GetNodeProperties::GetName() const
{
	return TEXT("claireon.pcg_get_node_properties");
}

FString ClaireonPCGGraphTool_GetNodeProperties::GetDescription() const
{
	return TEXT("Read the editable properties of a PCG node's settings.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_GetNodeProperties::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node"), TEXT("Node identifier (index or name)."), true);
	return Builder.Build();
}

FToolResult ClaireonPCGGraphTool_GetNodeProperties::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FPCGGraphEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString NodeIdentifier;
	if (!Arguments->TryGetStringField(TEXT("node"), NodeIdentifier) || NodeIdentifier.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: node"));
	}

	int32 NodeIndex;
	UPCGNode* Node = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Data->PCGGraph.Get(), NodeIdentifier, NodeIndex);
	if (!Node)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeIdentifier));
	}

	FString PropertiesText = ClaireonPCGGraphHelpers::ReadNodeProperties(Node);
	FString NodeName = ClaireonPCGGraphHelpers::GetNodeDisplayName(Node);

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("node"), NodeName);
	ResultJson->SetStringField(TEXT("properties"), PropertiesText.IsEmpty() ? TEXT("(no editable properties)") : PropertiesText);

	return MakeSuccessResult(ResultJson, FString::Printf(TEXT("Properties for %s"), *NodeName));
}
