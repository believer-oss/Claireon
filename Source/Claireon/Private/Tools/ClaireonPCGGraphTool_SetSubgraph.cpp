// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_SetSubgraph.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonPathResolver.h"

#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSubgraph.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_SetSubgraph::GetOperation() const { return TEXT("set_subgraph"); }

FString ClaireonPCGGraphTool_SetSubgraph::GetDescription() const
{
	return TEXT("Set the graph a Subgraph node references, within an open editing session. Use this "
				"rather than set_node_property: the reference lives inside an instanced "
				"UPCGGraphInstance sub-object, and writing it by reflection skips the cycle check, "
				"editor callbacks and parameter refresh that UPCGGraphInstance::SetGraph performs. "
				"Empty subgraph_path clears it. Requires session_id from pcg_open.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_SetSubgraph::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node"), TEXT("Subgraph node identifier (index or name)."), true);
	Builder.AddString(TEXT("subgraph_path"),
		TEXT("Path to the UPCGGraph or UPCGGraphInstance to reference. Empty string clears it."), true);
	return Builder.Build();
}

FToolResult ClaireonPCGGraphTool_SetSubgraph::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString SubgraphPath;
	if (!Arguments->TryGetStringField(TEXT("subgraph_path"), SubgraphPath))
	{
		return MakeErrorResult(TEXT("Missing required parameter: subgraph_path"));
	}
	SubgraphPath = SubgraphPath.TrimStartAndEnd();

	UPCGGraph* OwningGraph = Data->PCGGraph.Get();
	int32 NodeIndex = INDEX_NONE;
	UPCGNode* Node = ClaireonPCGGraphHelpers::FindNodeByIdentifier(OwningGraph, NodeIdentifier, NodeIndex);
	if (!IsValid(Node))
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeIdentifier));
	}

	UPCGBaseSubgraphSettings* SubgraphSettings = Cast<UPCGBaseSubgraphSettings>(Node->GetSettings());
	if (!IsValid(SubgraphSettings))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Node '%s' is not a Subgraph node (its settings are %s, not a UPCGBaseSubgraphSettings)"),
			*NodeIdentifier,
			IsValid(Node->GetSettings()) ? *Node->GetSettings()->GetClass()->GetName() : TEXT("null")));
	}

	UPCGGraphInterface* TargetGraph = nullptr;
	const bool bClearing = SubgraphPath.IsEmpty() || SubgraphPath.Equals(TEXT("None"), ESearchCase::IgnoreCase);
	if (!bClearing)
	{
		const auto Resolved = ClaireonPathResolver::Resolve(SubgraphPath);
		if (!Resolved.bSuccess)
		{
			return MakeErrorResult(Resolved.Error);
		}
		TargetGraph = LoadObject<UPCGGraphInterface>(nullptr, *Resolved.ResolvedPath.Path);
		if (!IsValid(TargetGraph))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("No UPCGGraph or UPCGGraphInstance found at '%s'"), *Resolved.ResolvedPath.Path));
		}

		// Reject direct self-reference before the engine reports it only through logging.
		if (TargetGraph == OwningGraph)
		{
			return MakeErrorResult(TEXT("A graph cannot reference itself as a subgraph"));
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set PCG Subgraph")));
	SubgraphSettings->Modify();
	SubgraphSettings->SetSubgraph(TargetGraph);

	// Verify void SetSubgraph accepted the assignment. Compare the resolved graph;
	// GetSubgraphInterface returns the node's wrapper even when it references nothing.
	const UPCGGraph* Applied = SubgraphSettings->GetSubgraph();
	const UPCGGraph* Expected = IsValid(TargetGraph) ? TargetGraph->GetGraph() : nullptr;
	if (Applied != Expected)
	{
		Transaction.Cancel();
		return MakeErrorResult(FString::Printf(
			TEXT("Setting the subgraph to '%s' was rejected by the engine, most likely because it "
				 "would create a cycle in the graph-instance hierarchy. The node still references %s."),
			bClearing ? TEXT("None") : *TargetGraph->GetPathName(),
			IsValid(Applied) ? *Applied->GetPathName() : TEXT("nothing")));
	}

	// Changing the subgraph affects both compiled tasks and pins.
	ClaireonPCGGraphHelpers::NotifyGraphChanged(OwningGraph, EPCGChangeType::Structural | EPCGChangeType::Node);

	Data->LastOperationStatus = FString::Printf(TEXT("Set %s subgraph = %s"),
		*ClaireonPCGGraphHelpers::GetNodeDisplayName(Node),
		bClearing ? TEXT("None") : *TargetGraph->GetPathName());

	FToolResult Result = BuildStateResponse(SessionId, Data);
	if (Result.Data.IsValid())
	{
		Result.Data->SetStringField(TEXT("subgraph"), bClearing ? TEXT("") : TargetGraph->GetPathName());
	}
	return Result;
}
