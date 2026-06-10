// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_Disconnect.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_Disconnect::GetOperation() const { return TEXT("disconnect"); }

FString ClaireonPCGGraphTool_Disconnect::GetDescription() const
{
	return TEXT("Disconnect a specific edge between two PCG node pins within an open editing "
				"session. Requires session_id from pcg_graph.open; the edit is transactional and only "
				"persists after save. Other edges on the same pins are unaffected; use disconnect_all "
				"to clear every edge on a pin.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_Disconnect::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("from_node"), TEXT("Source node identifier (index or name)."), true);
	Builder.AddString(TEXT("from_pin"), TEXT("Source output pin label."), true);
	Builder.AddString(TEXT("to_node"), TEXT("Target node identifier (index or name)."), true);
	Builder.AddString(TEXT("to_pin"), TEXT("Target input pin label."), true);
	return Builder.Build();
}

FToolResult ClaireonPCGGraphTool_Disconnect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FPCGGraphEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString FromNodeId, FromPinLabel, ToNodeId, ToPinLabel;
	if (!Arguments->TryGetStringField(TEXT("from_node"), FromNodeId)
		|| !Arguments->TryGetStringField(TEXT("from_pin"), FromPinLabel)
		|| !Arguments->TryGetStringField(TEXT("to_node"), ToNodeId)
		|| !Arguments->TryGetStringField(TEXT("to_pin"), ToPinLabel))
	{
		return MakeErrorResult(TEXT("Missing required parameters: from_node, from_pin, to_node, to_pin"));
	}

	int32 FromIndex, ToIndex;
	UPCGNode* FromNode = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Data->PCGGraph.Get(), FromNodeId, FromIndex);
	UPCGNode* ToNode = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Data->PCGGraph.Get(), ToNodeId, ToIndex);

	if (!FromNode || !ToNode)
	{
		return MakeErrorResult(TEXT("Source or target node not found"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Disconnect PCG Pins")));
	bool bRemoved = Data->PCGGraph->RemoveEdge(FromNode, FName(*FromPinLabel), ToNode, FName(*ToPinLabel));

	if (!bRemoved)
	{
		return MakeErrorResult(TEXT("No matching edge found to remove"));
	}

	ClaireonPCGGraphHelpers::NotifyGraphChanged(Data->PCGGraph.Get());

	Data->LastOperationStatus = FString::Printf(TEXT("Disconnected %s.\"%s\" -> %s.\"%s\""),
		*ClaireonPCGGraphHelpers::GetNodeDisplayName(FromNode), *FromPinLabel,
		*ClaireonPCGGraphHelpers::GetNodeDisplayName(ToNode), *ToPinLabel);

	return BuildStateResponse(SessionId, Data);
}
