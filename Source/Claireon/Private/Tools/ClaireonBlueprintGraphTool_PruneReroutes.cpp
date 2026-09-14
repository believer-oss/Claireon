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

// Prune orphaned components and dangling ends to a fixpoint.
// Prefix helpers to avoid unity-build collisions.
namespace ClaireonPruneReroutes
{

namespace ClaireonPruneReroutesInternal
{
	/** Recollect live knots each pass because removal can expose new dangling ends. */
	TArray<UK2Node_Knot*> ClaireonPruneReroutes_CollectKnots(UEdGraph* Graph)
	{
		TArray<UK2Node_Knot*> Knots;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node); IsValid(Knot))
			{
				Knots.Add(Knot);
			}
		}
		return Knots;
	}

	/** True when Pin carries a literal a downstream consumer might still be relying on. */
	bool ClaireonPruneReroutes_PinHasNonEmptyDefault(const UEdGraphPin* Pin)
	{
		return Pin && (!Pin->DefaultValue.IsEmpty() || Pin->DefaultObject != nullptr || !Pin->DefaultTextValue.IsEmpty());
	}

	/** Break every pin link on each knot in ToDelete, then remove it from Graph. */
	void ClaireonPruneReroutes_DeleteKnots(UBlueprint* Blueprint, UEdGraph* Graph, const TArray<UK2Node_Knot*>& ToDelete)
	{
		if (ToDelete.Num() == 0)
		{
			return;
		}

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
	}

	/** Remove knot-only components; this catches cycles with no dangling ends. */
	int32 ClaireonPruneReroutes_RunComponentPass(UBlueprint* Blueprint, UEdGraph* Graph, FPruneResult& InOutResult)
	{
		TArray<UK2Node_Knot*> AllKnots = ClaireonPruneReroutes_CollectKnots(Graph);
		if (AllKnots.Num() == 0)
		{
			return 0;
		}

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
					if (!LinkedPin || !IsValid(LinkedPin->GetOwningNode())) continue;
					if (UK2Node_Knot* LinkedKnot = Cast<UK2Node_Knot>(LinkedPin->GetOwningNode()); IsValid(LinkedKnot))
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

		TSet<UK2Node_Knot*> Visited;
		TArray<UK2Node_Knot*> ToDelete;

		for (UK2Node_Knot* StartKnot : AllKnots)
		{
			if (Visited.Contains(StartKnot)) continue;

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

		ClaireonPruneReroutes_DeleteKnots(Blueprint, Graph, ToDelete);
		InOutResult.RemovedOrphanComponent += ToDelete.Num();
		return ToDelete.Num();
	}

	/**
	 * Remove outputless knots. Preserve inputless knots with defaults because compiler expansion
	 * propagates those literals to consumers, while direct deletion does not.
	 */
	int32 ClaireonPruneReroutes_RunDanglingEndPass(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		TSet<UK2Node_Knot*>& InOutReportedSkips,
		FPruneResult& InOutResult)
	{
		TArray<UK2Node_Knot*> AllKnots = ClaireonPruneReroutes_CollectKnots(Graph);
		if (AllKnots.Num() == 0)
		{
			return 0;
		}

		TArray<UK2Node_Knot*> ToDelete;

		for (UK2Node_Knot* Knot : AllKnots)
		{
			UEdGraphPin* InputPin = Knot->GetInputPin();
			UEdGraphPin* OutputPin = Knot->GetOutputPin();
			const bool bHasInput = InputPin && InputPin->LinkedTo.Num() > 0;
			const bool bHasOutput = OutputPin && OutputPin->LinkedTo.Num() > 0;

			if (!bHasOutput)
			{
				ToDelete.Add(Knot);
				++InOutResult.RemovedDanglingNoOutput;
				continue;
			}

			if (!bHasInput)
			{
				if (ClaireonPruneReroutes_PinHasNonEmptyDefault(InputPin))
				{
					// Count guarded knots once across all rounds.
					if (!InOutReportedSkips.Contains(Knot))
					{
						InOutReportedSkips.Add(Knot);
						++InOutResult.SkippedNoInputWithDefault;
					}
					continue;
				}

				ToDelete.Add(Knot);
				++InOutResult.RemovedDanglingNoInput;
			}
		}

		ClaireonPruneReroutes_DeleteKnots(Blueprint, Graph, ToDelete);
		return ToDelete.Num();
	}
}
using namespace ClaireonPruneReroutesInternal;

FPruneResult PruneOrphanReroutesInGraph(UBlueprint* Blueprint, UEdGraph* Graph)
{
	FPruneResult Result;

	const int32 InitialKnotCount = ClaireonPruneReroutes_CollectKnots(Graph).Num();
	if (InitialKnotCount == 0)
	{
		// An empty graph still completes one confirming round.
		Result.Passes = 1;
		Result.bConverged = true;
		return Result;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Prune Orphan Reroute Nodes")));

	TSet<UK2Node_Knot*> ReportedSkips;

	// At most one productive round per original knot, plus a confirming round, is needed.
	const int32 MaxPasses = InitialKnotCount + 1;

	for (;;)
	{
		++Result.Passes;

		const int32 ComponentRemoved = ClaireonPruneReroutes_RunComponentPass(Blueprint, Graph, Result);
		const int32 DanglingRemoved = ClaireonPruneReroutes_RunDanglingEndPass(Blueprint, Graph, ReportedSkips, Result);

		if (ComponentRemoved + DanglingRemoved == 0)
		{
			Result.bConverged = true;
			break;
		}

		if (Result.Passes >= MaxPasses)
		{
			// Report an exhausted cap rather than claiming convergence.
			break;
		}
	}

	if (!Result.bConverged)
	{
		UE_LOG(LogClaireon, Error,
			TEXT("[prune_reroutes] Hit the %d-pass cap on graph '%s' without converging ")
			TEXT("(removed %d of %d initial knot(s) so far). Each productive round should ")
			TEXT("remove at least one knot from a finite graph, so this is a bug in the ")
			TEXT("component or dangling-end pass, not a property of this Blueprint."),
			MaxPasses, *Graph->GetName(), Result.Total(), InitialKnotCount);
	}

	if (Result.Total() > 0)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	return Result;
}

/** Report removal count and convergence in the shared status line. */
FString BuildStatusMessage(const FPruneResult& Result)
{
	if (!Result.bConverged)
	{
		return FString::Printf(
			TEXT("Pruned %d reroute node(s) in %d pass(es) -- DID NOT CONVERGE (hit the pass ")
			TEXT("cap; this is a bug -- more orphaned or dangling reroutes may remain)"),
			Result.Total(), Result.Passes);
	}

	FString Msg = FString::Printf(
		TEXT("Pruned %d reroute node(s) in %d pass(es) (converged)"),
		Result.Total(), Result.Passes);

	if (Result.SkippedNoInputWithDefault > 0)
	{
		Msg += FString::Printf(
			TEXT("; skipped %d knot(s) with no input link because the input pin carries a literal default"),
			Result.SkippedNoInputWithDefault);
	}

	return Msg;
}

}


FString ClaireonBlueprintGraphTool_PruneReroutes::GetOperation() const
{
	return TEXT("prune_reroutes");
}

FString ClaireonBlueprintGraphTool_PruneReroutes::GetDescription() const
{
	return TEXT("Delete orphaned and dangling reroute (knot) nodes from a Blueprint graph, "
		"iterating to a fixpoint so a chain of deletions cascades fully in one call. "
		"A knot is removed when its component touches nothing real, or it has no incoming "
		"or no outgoing link (guarded against dropping a literal default). "
		"Accepts session_id (open session) or asset_path + graph_name (stateless).");
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
		if (!IsValid(Blueprint) || !IsValid(Graph))
		{
			return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));
		}

		const ClaireonPruneReroutes::FPruneResult PruneResult = ClaireonPruneReroutes::PruneOrphanReroutesInGraph(Blueprint, Graph);
		Data->Cursor.LastOperationStatus = ClaireonPruneReroutes::BuildStatusMessage(PruneResult);

		FToolResult Result = BuildStateResponse(SessionId, Data);
		if (Result.Data.IsValid())
		{
			Result.Data->SetNumberField(TEXT("removed_total"), PruneResult.Total());
			Result.Data->SetNumberField(TEXT("removed_orphan_component"), PruneResult.RemovedOrphanComponent);
			Result.Data->SetNumberField(TEXT("removed_dangling_no_input"), PruneResult.RemovedDanglingNoInput);
			Result.Data->SetNumberField(TEXT("removed_dangling_no_output"), PruneResult.RemovedDanglingNoOutput);
			Result.Data->SetNumberField(TEXT("skipped_no_input_with_default"), PruneResult.SkippedNoInputWithDefault);
			Result.Data->SetNumberField(TEXT("passes"), PruneResult.Passes);
			Result.Data->SetBoolField(TEXT("converged"), PruneResult.bConverged);
		}
		return Result;
	}

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
	if (!IsValid(Blueprint))
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath));
	}

	UEdGraph* Graph = ClaireonBlueprintHelpers::FindGraphByName(Blueprint, GraphName);
	if (!IsValid(Graph))
	{
		return MakeErrorResult(FString::Printf(TEXT("Graph '%s' not found in %s"), *GraphName, *AssetPath));
	}

	const ClaireonPruneReroutes::FPruneResult PruneResult = ClaireonPruneReroutes::PruneOrphanReroutesInGraph(Blueprint, Graph);

	auto ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetNumberField(TEXT("removed_total"), PruneResult.Total());
	ResultObj->SetNumberField(TEXT("removed_orphan_component"), PruneResult.RemovedOrphanComponent);
	ResultObj->SetNumberField(TEXT("removed_dangling_no_input"), PruneResult.RemovedDanglingNoInput);
	ResultObj->SetNumberField(TEXT("removed_dangling_no_output"), PruneResult.RemovedDanglingNoOutput);
	ResultObj->SetNumberField(TEXT("skipped_no_input_with_default"), PruneResult.SkippedNoInputWithDefault);
	ResultObj->SetNumberField(TEXT("passes"), PruneResult.Passes);
	ResultObj->SetBoolField(TEXT("converged"), PruneResult.bConverged);
	return MakeSuccessResult(ResultObj,
		FString::Printf(TEXT("%s (%s/%s)"),
			*ClaireonPruneReroutes::BuildStatusMessage(PruneResult), *AssetPath, *GraphName));
}

#undef LOCTEXT_NAMESPACE
