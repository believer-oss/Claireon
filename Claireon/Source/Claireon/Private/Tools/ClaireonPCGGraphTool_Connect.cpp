// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_Connect.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_Connect::GetName() const
{
	return TEXT("claireon.pcg_connect");
}

FString ClaireonPCGGraphTool_Connect::GetDescription() const
{
	return TEXT("Connect an output pin of one PCG node to an input pin of another.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_Connect::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("from_node"), TEXT("Source node identifier (index or name)."), true);
	Builder.AddString(TEXT("from_pin"), TEXT("Source output pin label."), true);
	Builder.AddString(TEXT("to_node"), TEXT("Target node identifier (index or name)."), true);
	Builder.AddString(TEXT("to_pin"), TEXT("Target input pin label."), true);
	return Builder.Build();
}

FToolResult ClaireonPCGGraphTool_Connect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FPCGGraphEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString FromNodeId, FromPinLabel, ToNodeId, ToPinLabel;
	if (!Arguments->TryGetStringField(TEXT("from_node"), FromNodeId) || FromNodeId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: from_node"));
	}
	if (!Arguments->TryGetStringField(TEXT("from_pin"), FromPinLabel) || FromPinLabel.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: from_pin"));
	}
	if (!Arguments->TryGetStringField(TEXT("to_node"), ToNodeId) || ToNodeId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: to_node"));
	}
	if (!Arguments->TryGetStringField(TEXT("to_pin"), ToPinLabel) || ToPinLabel.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: to_pin"));
	}

	int32 FromIndex, ToIndex;
	UPCGNode* FromNode = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Data->PCGGraph.Get(), FromNodeId, FromIndex);
	UPCGNode* ToNode = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Data->PCGGraph.Get(), ToNodeId, ToIndex);

	if (!FromNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Source node not found: %s"), *FromNodeId));
	}
	if (!ToNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Target node not found: %s"), *ToNodeId));
	}

	// Verify pins exist
	UPCGPin* FromPin = FromNode->GetOutputPin(FName(*FromPinLabel));
	if (!FromPin)
	{
		// List available output pins
		FString Available;
		for (const TObjectPtr<UPCGPin>& Pin : FromNode->GetOutputPins())
		{
			if (Pin)
			{
				if (!Available.IsEmpty())
					Available += TEXT(", ");
				Available += Pin->Properties.Label.ToString();
			}
		}
		return MakeErrorResult(FString::Printf(TEXT("Output pin '%s' not found on node %s. Available: %s"),
			*FromPinLabel, *ClaireonPCGGraphHelpers::GetNodeDisplayName(FromNode), *Available));
	}

	UPCGPin* ToPin = ToNode->GetInputPin(FName(*ToPinLabel));
	if (!ToPin)
	{
		FString Available;
		for (const TObjectPtr<UPCGPin>& Pin : ToNode->GetInputPins())
		{
			if (Pin)
			{
				if (!Available.IsEmpty())
					Available += TEXT(", ");
				Available += Pin->Properties.Label.ToString();
			}
		}
		return MakeErrorResult(FString::Printf(TEXT("Input pin '%s' not found on node %s. Available: %s"),
			*ToPinLabel, *ClaireonPCGGraphHelpers::GetNodeDisplayName(ToNode), *Available));
	}

	// Check compatibility
	if (!FromPin->CanConnect(ToPin))
	{
		return MakeErrorResult(FString::Printf(TEXT("Pins are not compatible: %s.%s -> %s.%s"),
			*ClaireonPCGGraphHelpers::GetNodeDisplayName(FromNode), *FromPinLabel,
			*ClaireonPCGGraphHelpers::GetNodeDisplayName(ToNode), *ToPinLabel));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Connect PCG Pins")));
	Data->PCGGraph->AddEdge(FromNode, FName(*FromPinLabel), ToNode, FName(*ToPinLabel));
	ClaireonPCGGraphHelpers::NotifyGraphChanged(Data->PCGGraph.Get());

	Data->LastOperationStatus = FString::Printf(TEXT("Connected %s.\"%s\" -> %s.\"%s\""),
		*ClaireonPCGGraphHelpers::GetNodeDisplayName(FromNode), *FromPinLabel,
		*ClaireonPCGGraphHelpers::GetNodeDisplayName(ToNode), *ToPinLabel);

	return BuildStateResponse(SessionId, Data);
}
