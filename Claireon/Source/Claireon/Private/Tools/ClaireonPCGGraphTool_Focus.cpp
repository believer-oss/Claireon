// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_Focus.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "PCGGraph.h"
#include "PCGNode.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_Focus::GetOperation() const { return TEXT("focus"); }

FString ClaireonPCGGraphTool_Focus::GetDescription() const
{
	return TEXT("Move the session cursor to focus on a specific PCG node. Updates navigation history.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_Focus::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node"), TEXT("Node identifier (index or name) to focus on."), true);
	return Builder.Build();
}

FToolResult ClaireonPCGGraphTool_Focus::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	Data->PushHistory();
	Data->FocusedNodeIndex = NodeIndex;
	Data->LastOperationStatus = FString::Printf(TEXT("Focused on [%d] %s"), NodeIndex, *ClaireonPCGGraphHelpers::GetNodeDisplayName(Node));

	return BuildStateResponse(SessionId, Data);
}
