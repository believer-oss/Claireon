// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_Connect.h"
#include "Tools/ClaireonPCGEditorSync.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGEdge.h"
#include "Elements/PCGUserParameterGet.h"
#include "StructUtils/PropertyBag.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_Connect::GetOperation() const { return TEXT("connect"); }

FString ClaireonPCGGraphTool_Connect::GetDescription() const
{
	return TEXT("Connect an output pin of one PCG node to an input pin of another within an open "
				"editing session. Requires session_id from pcg_graph.open; the edit is transactional "
				"and only persists after save. Pin compatibility is enforced by PCG's type system at "
				"connection time.");
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

	if (!IsValid(FromNode))
	{
		return MakeErrorResult(FString::Printf(TEXT("Source node not found: %s"), *FromNodeId));
	}
	if (!IsValid(ToNode))
	{
		return MakeErrorResult(FString::Printf(TEXT("Target node not found: %s"), *ToNodeId));
	}

	// Verify pins exist
	UPCGPin* FromPin = FromNode->GetOutputPin(FName(*FromPinLabel));
	if (!IsValid(FromPin))
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
	if (!IsValid(ToPin))
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

	// An unbound parameter getter still advertises an output pin, but the editor
	// cannot create its links. Validate the property GUID before adding an edge.
	if (const UPCGUserParameterGetSettings* GetSettings =
			Cast<UPCGUserParameterGetSettings>(FromNode->GetSettings()))
	{
		const FInstancedPropertyBag* Bag = Data->PCGGraph->GetUserParametersStruct();
		const bool bBound =
			Bag && GetSettings->PropertyGuid.IsValid() && Bag->FindPropertyDescByID(GetSettings->PropertyGuid);
		if (!bBound)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Node %s is a Get Graph Parameter bound to no live graph parameter (PropertyGuid "
					 "%s), so the link would be accepted by the graph and rejected by the editor. Set "
					 "its PropertyGuid first -- pcg_set_node_property accepts the parameter NAME and "
					 "resolves it, and pcg_add_user_parameter returns property_guid."),
				*ClaireonPCGGraphHelpers::GetNodeDisplayName(FromNode),
				GetSettings->PropertyGuid.IsValid() ? *GetSettings->PropertyGuid.ToString(EGuidFormats::Digits)
													: TEXT("unset")));
		}
	}

	// Settle pending reconstruction so newly added peers have editor nodes before
	// AddEdge triggers native Input/Output link rebuilding.
	ClaireonPCGEditorSync::SettlePendingReconstruct(Data->PCGGraph.Get());

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Connect PCG Pins")));
	Data->PCGGraph->AddEdge(FromNode, FName(*FromPinLabel), ToNode, FName(*ToPinLabel));

	// AddEdge returns void and may reject a pair silently; verify the edge exists.
	bool bEdgeLanded = false;
	for (const TObjectPtr<UPCGEdge>& Edge : FromPin->Edges)
	{
		if (Edge && Edge->GetOtherPin(FromPin) == ToPin)
		{
			bEdgeLanded = true;
			break;
		}
	}
	if (!bEdgeLanded)
	{
		Transaction.Cancel();
		return MakeErrorResult(FString::Printf(
			TEXT("AddEdge did not produce a link %s.\"%s\" -> %s.\"%s\""),
			*ClaireonPCGGraphHelpers::GetNodeDisplayName(FromNode), *FromPinLabel,
			*ClaireonPCGGraphHelpers::GetNodeDisplayName(ToNode), *ToPinLabel));
	}

	ClaireonPCGGraphHelpers::NotifyGraphChanged(Data->PCGGraph.Get(), ClaireonPCGGraphHelpers::EPCGGraphEditOp::Connect);

	Data->LastOperationStatus = FString::Printf(TEXT("Connected %s.\"%s\" -> %s.\"%s\""),
		*ClaireonPCGGraphHelpers::GetNodeDisplayName(FromNode), *FromPinLabel,
		*ClaireonPCGGraphHelpers::GetNodeDisplayName(ToNode), *ToPinLabel);

	return BuildStateResponse(SessionId, Data);
}
