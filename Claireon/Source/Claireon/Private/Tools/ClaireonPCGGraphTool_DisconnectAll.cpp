// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_DisconnectAll.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_DisconnectAll::GetName() const
{
	return TEXT("claireon.pcg_disconnect_all");
}

FString ClaireonPCGGraphTool_DisconnectAll::GetDescription() const
{
	return TEXT("Remove all edges connected to a given pin on a PCG node. Direction filter (input/output) is optional.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_DisconnectAll::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node"), TEXT("Node identifier (index or name)."), true);
	Builder.AddString(TEXT("pin"), TEXT("Pin label to disconnect from."), true);
	Builder.AddString(TEXT("direction"), TEXT("Optional pin direction: 'input' or 'output'. Defaults to both directions."));
	return Builder.Build();
}

FToolResult ClaireonPCGGraphTool_DisconnectAll::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FPCGGraphEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString NodeIdentifier, PinLabel;
	if (!Arguments->TryGetStringField(TEXT("node"), NodeIdentifier) || NodeIdentifier.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: node"));
	}
	if (!Arguments->TryGetStringField(TEXT("pin"), PinLabel) || PinLabel.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: pin"));
	}

	int32 NodeIndex;
	UPCGNode* Node = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Data->PCGGraph.Get(), NodeIdentifier, NodeIndex);
	if (!Node)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeIdentifier));
	}

	FString Direction;
	Arguments->TryGetStringField(TEXT("direction"), Direction);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Disconnect All PCG Pins")));

	bool bRemoved = false;
	FName PinName(*PinLabel);

	if (Direction.IsEmpty() || Direction.Equals(TEXT("output"), ESearchCase::IgnoreCase))
	{
		bRemoved |= Data->PCGGraph->RemoveOutboundEdges(Node, PinName);
	}
	if (Direction.IsEmpty() || Direction.Equals(TEXT("input"), ESearchCase::IgnoreCase))
	{
		bRemoved |= Data->PCGGraph->RemoveInboundEdges(Node, PinName);
	}

	if (bRemoved)
	{
		ClaireonPCGGraphHelpers::NotifyGraphChanged(Data->PCGGraph.Get());
	}

	Data->LastOperationStatus = FString::Printf(TEXT("Disconnected all edges on %s.\"%s\"%s"),
		*ClaireonPCGGraphHelpers::GetNodeDisplayName(Node), *PinLabel,
		bRemoved ? TEXT("") : TEXT(" (no edges found)"));

	return BuildStateResponse(SessionId, Data);
}
