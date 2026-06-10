// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintGraphTool_PruneReroutes.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonBlueprintHelpers.h"
#include "Dom/JsonObject.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase_Internal.h"
#include "ClaireonLog.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_Knot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ScopedTransaction.h"
#include "Containers/Queue.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;

// ---------------------------------------------------------------------------
// Shared logic: find and delete orphaned reroute chains in a graph.
// A reroute (K2Node_Knot) chain is orphaned when no node in its connected
// component has a pin linked to a non-knot node.
// Returns the number of knots deleted.
// ---------------------------------------------------------------------------
static int32 PruneOrphanReroutesInGraph(UBlueprint* Blueprint, UEdGraph* Graph)
{
	// Collect all knots
	TArray<UK2Node_Knot*> AllKnots;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node))
		{
			AllKnots.Add(Knot);
		}
	}

	if (AllKnots.Num() == 0)
	{
		return 0;
	}

	// For each knot: does it have at least one pin connected to a non-knot node?
	TMap<UK2Node_Knot*, bool> TouchesReal;
	// knot -> neighbouring knots (undirected)
	TMap<UK2Node_Knot*, TArray<UK2Node_Knot*>> KnotNeighbors;

	for (UK2Node_Knot* Knot : AllKnots)
	{
		TouchesReal.Add(Knot, false);
		KnotNeighbors.Add(Knot, {});

		for (UEdGraphPin* Pin : Knot->Pins)
		{
			if (!Pin) continue;
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;
				if (UK2Node_Knot* LinkedKnot = Cast<UK2Node_Knot>(LinkedPin->GetOwningNode()))
				{
					KnotNeighbors[Knot].AddUnique(LinkedKnot);
				}
				else
				{
					TouchesReal[Knot] = true;
				}
			}
		}
	}

	// BFS over connected components; mark whole component orphaned if nothing touches real
	TSet<UK2Node_Knot*> Visited;
	TArray<UK2Node_Knot*> ToDelete;

	for (UK2Node_Knot* StartKnot : AllKnots)
	{
		if (Visited.Contains(StartKnot)) continue;

		// Flood-fill connected component
		TArray<UK2Node_Knot*> Component;
		TQueue<UK2Node_Knot*> Queue;
		Queue.Enqueue(StartKnot);
		Visited.Add(StartKnot);
		bool bAnyTouchesReal = false;

		while (!Queue.IsEmpty())
		{
			UK2Node_Knot* Current = nullptr;
			Queue.Dequeue(Current);
			Component.Add(Current);

			if (TouchesReal[Current])
			{
				bAnyTouchesReal = true;
			}

			for (UK2Node_Knot* Neighbor : KnotNeighbors[Current])
			{
				if (!Visited.Contains(Neighbor))
				{
					Visited.Add(Neighbor);
					Queue.Enqueue(Neighbor);
				}
			}
		}

		if (!bAnyTouchesReal)
		{
			ToDelete.Append(Component);
		}
	}

	if (ToDelete.Num() == 0)
	{
		return 0;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Prune Orphan Reroute Nodes")));
	Blueprint->Modify();
	Graph->Modify();

	for (UK2Node_Knot* Knot : ToDelete)
	{
		for (UEdGraphPin* Pin : Knot->Pins)
		{
			if (Pin) Pin->BreakAllPinLinks();
		}
		Graph->RemoveNode(Knot);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	return ToDelete.Num();
}

// ---------------------------------------------------------------------------

FString ClaireonBlueprintGraphTool_PruneReroutes::GetOperation() const
{
	return TEXT("prune_reroutes");
}

FString ClaireonBlueprintGraphTool_PruneReroutes::GetDescription() const
{
	return TEXT("Delete all orphaned reroute (knot) node chains from a Blueprint graph. "
		"A reroute chain is orphaned when none of its nodes connect to a real (non-reroute) pin on either end -- "
		"including chains where reroutes only connect to other reroutes. "
		"Accepts session_id (open session) or asset_path + graph_name (stateless). "
		"Returns the number of reroute nodes removed.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_PruneReroutes::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior bp_open (or use asset_path for stateless mode)."), false);
	Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (stateless mode, or alternative to session_id)."), false);
	Builder.AddString(TEXT("graph_name"), TEXT("Graph name (required in stateless mode; defaults to EventGraph when using session_id)."), false);
	Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'status')."));
	return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_PruneReroutes::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	TSharedPtr<FJsonObject> Params = Arguments.IsValid() ? Arguments : MakeShared<FJsonObject>();
	if (Params->HasField(TEXT("params")))
	{
		const TSharedPtr<FJsonObject>* NestedObj = nullptr;
		if (Params->TryGetObjectField(TEXT("params"), NestedObj) && NestedObj && NestedObj->IsValid())
		{
			Params = *NestedObj;
		}
	}

	// Session-based path
	FString SessionId;
	if (Params->TryGetStringField(TEXT("session_id"), SessionId) && !SessionId.IsEmpty())
	{
		TSharedPtr<FJsonObject> SessionParams;
		FBlueprintEditToolData* Data = nullptr;
		FToolResult Error;
		if (!BeginSessionOp(Arguments, TEXT("prune_reroutes"), SessionParams, SessionId, Data, Error))
		{
			return Error;
		}

		UBlueprint* Blueprint = Data->Blueprint.Get();
		UEdGraph* Graph = Data->Graph.Get();
		if (!Blueprint || !Graph)
		{
			return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));
		}

		const int32 Removed = PruneOrphanReroutesInGraph(Blueprint, Graph);
		Data->Cursor.LastOperationStatus = FString::Printf(
			TEXT("Pruned %d orphaned reroute node(s)"), Removed);
		return BuildStateResponse(SessionId, Data);
	}

	// Stateless path
	FString AssetPath, GraphName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path (or session_id for session-based mode)"));
	}
	if (!Params->TryGetStringField(TEXT("graph_name"), GraphName))
	{
		GraphName = TEXT("EventGraph");
	}

	FString ValidationError;
	if (!ClaireonBlueprintHelpers::ValidateAssetPath(AssetPath, ValidationError))
	{
		return MakeErrorResult(ValidationError);
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath));
	}

	UEdGraph* Graph = ClaireonBlueprintHelpers::FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return MakeErrorResult(FString::Printf(TEXT("Graph '%s' not found in %s"), *GraphName, *AssetPath));
	}

	const int32 Removed = PruneOrphanReroutesInGraph(Blueprint, Graph);

	auto ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetNumberField(TEXT("removed_count"), Removed);
	return MakeSuccessResult(ResultObj,
		FString::Printf(TEXT("Pruned %d orphaned reroute node(s) from %s/%s"),
			Removed, *AssetPath, *GraphName));
}

#undef LOCTEXT_NAMESPACE
