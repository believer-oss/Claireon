// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonLintTypes.h"
#include "ClaireonExecTopology.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_BaseAsyncTask.h"
#include "K2Node_CallFunction.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Switch.h"
#include "K2Node_Timeline.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"

namespace ClaireonLint
{

namespace ClaireonLintGraphInternal
{
	FString ClaireonLintGraph_TargetId(const UEdGraphNode* Node, const UEdGraphPin* Pin)
	{
		const FString Guid = IsValid(Node) ? Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : TEXT("<null>");
		return Pin ? Guid + TEXT(".") + Pin->PinName.ToString() : Guid;
	}

	/**
	 * Latent lanes make Sequence conversion unsafe: later lanes start before latent
	 * completion. Detect latency by metadata, not the number of exec outputs.
	 */
	bool ClaireonLintGraph_IsLatent(const UEdGraphNode* Node)
	{
		if (!IsValid(Node))
		{
			return false;
		}

		// Async action nodes (UK2Node_BaseAsyncTask and its ability-task subclasses)
		// are latent by construction: they complete through output delegates.
		if (Node->IsA<UK2Node_BaseAsyncTask>())
		{
			return true;
		}

		if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node); IsValid(Call))
		{
			if (const UFunction* Target = Call->GetTargetFunction(); IsValid(Target))
			{
				return Target->HasMetaData(FBlueprintMetadata::MD_Latent);
			}
		}

		return false;
	}

	/**
	 * Build knot-collapsed exec flow once per graph and index it by node and pin.
	 * Use ClaireonExecTopology so direct and rerouted joins classify consistently.
	 */
	struct FClaireonLintGraphExecFlow
	{
		/** Exec edges only. A knot never appears as either endpoint. */
		TArray<FClaireonCollapsedEdge> Edges;

		/** FromNode -> indices into Edges, for the node-level forward walk. */
		TMap<const UEdGraphNode*, TArray<int32>> OutOfNode;

		/** Index by output pin: all-branches requires each branch to reach the join. */
		TMap<const UEdGraphPin*, TArray<int32>> OutOfPin;
	};

	FClaireonLintGraphExecFlow ClaireonLintGraph_BuildExecFlow(const UEdGraph* Graph)
	{
		FClaireonLintGraphExecFlow Flow;
		if (!IsValid(Graph))
		{
			return Flow;
		}

		// Include direct edges as well as rerouted edges for complete topology.
		for (const FClaireonCollapsedEdge& Edge :
			ClaireonExecTopology::CollapseEdges(Graph, /*bIncludeDirect=*/true))
		{
			// Read the source pin category; knot pins may still be wildcard.
			if (!Edge.FromPin || Edge.FromPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			if (!IsValid(Edge.FromNode) || !IsValid(Edge.ToNode))
			{
				continue;
			}
			const int32 Index = Flow.Edges.Add(Edge);
			Flow.OutOfNode.FindOrAdd(Edge.FromNode).Add(Index);
			Flow.OutOfPin.FindOrAdd(Edge.FromPin).Add(Index);
		}
		return Flow;
	}

	/**
	 * Forward exec closure, including Start and excluding knots. Guard cycles with
	 * a visited set; reroutes must not inflate the real-node count.
	 */
	TSet<const UEdGraphNode*> ClaireonLintGraph_ForwardExecClosure(
		const FClaireonLintGraphExecFlow& Flow, const UEdGraphNode* Start)
	{
		TSet<const UEdGraphNode*> Closure;
		if (!IsValid(Start))
		{
			return Closure;
		}

		TArray<const UEdGraphNode*> Frontier;
		Closure.Add(Start);
		Frontier.Add(Start);

		while (Frontier.Num() > 0)
		{
			const UEdGraphNode* Current = Frontier.Pop(EAllowShrinking::No);
			const TArray<int32>* Indices = Flow.OutOfNode.Find(Current);
			if (!Indices)
			{
				continue;
			}
			for (const int32 Index : *Indices)
			{
				const UEdGraphNode* Next = Flow.Edges[Index].ToNode;
				if (!IsValid(Next) || Closure.Contains(Next))
				{
					continue;
				}
				Closure.Add(Next);
				Frontier.Add(Next);
			}
		}
		return Closure;
	}

	/**
	 * Count edges entering the closure except at JoinPin. Such entries reach the
	 * region independently, so extracting it can change execution.
	 */
	int32 ClaireonLintGraph_CountForeignExecEntries(
		const FClaireonLintGraphExecFlow& Flow,
		const TSet<const UEdGraphNode*>& Closure,
		const UEdGraphPin* JoinPin)
	{
		int32 Count = 0;
		for (const FClaireonCollapsedEdge& Edge : Flow.Edges)
		{
			if (Edge.ToPin == JoinPin)
			{
				continue;
			}
			if (!Closure.Contains(Edge.ToNode) || Closure.Contains(Edge.FromNode))
			{
				continue;
			}
			++Count;
		}
		return Count;
	}

	/** Count executable exec pins, excluding hidden and orphaned pins. */
	int32 ClaireonLintGraph_ExecPinCount(const UEdGraphNode* Node, EEdGraphPinDirection Direction)
	{
		if (!IsValid(Node))
		{
			return 0;
		}

		int32 Count = 0;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != Direction || Pin->bHidden || Pin->bOrphanedPin)
			{
				continue;
			}
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				++Count;
			}
		}
		return Count;
	}

	/**
	 * Shortest collapsed-edge distance from one output pin to TargetPin, or INDEX_NONE.
	 * Use BFS with a per-node cycle guard; one knot chain counts as one edge.
	 */
	int32 ClaireonLintGraph_ExecPinDistance(
		const FClaireonLintGraphExecFlow& Flow, const UEdGraphPin* FromPin, const UEdGraphPin* TargetPin)
	{
		if (!FromPin || !TargetPin)
		{
			return INDEX_NONE;
		}

		TSet<const UEdGraphNode*> Visited;
		TArray<const UEdGraphPin*> Layer;
		TArray<const UEdGraphPin*> NextLayer;
		Layer.Add(FromPin);

		int32 Hops = 0;
		while (Layer.Num() > 0)
		{
			++Hops;
			NextLayer.Reset();

			for (const UEdGraphPin* Pin : Layer)
			{
				const TArray<int32>* Indices = Flow.OutOfPin.Find(Pin);
				if (!Indices)
				{
					continue;
				}
				for (const int32 Index : *Indices)
				{
					const FClaireonCollapsedEdge& Edge = Flow.Edges[Index];
					if (Edge.ToPin == TargetPin)
					{
						return Hops;
					}
					if (!IsValid(Edge.ToNode) || Visited.Contains(Edge.ToNode))
					{
						continue;
					}
					Visited.Add(Edge.ToNode);
					for (UEdGraphPin* OutPin : Edge.ToNode->Pins)
					{
						if (OutPin && OutPin->Direction == EGPD_Output && !OutPin->bHidden && !OutPin->bOrphanedPin
							&& OutPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
						{
							NextLayer.Add(OutPin);
						}
					}
				}
			}

			Layer = MoveTemp(NextLayer);
		}
		return INDEX_NONE;
	}

	/**
	 * Detect macro exec cycles with iterative three-color DFS. Loop outputs are not
	 * exclusive, so loops cannot support branch-hoisting suggestions. Iteration avoids
	 * stack overflow on deeply nested graphs.
	 */
	bool ClaireonLintGraph_MacroGraphHasExecCycle(const UEdGraph* MacroGraph)
	{
		if (!IsValid(MacroGraph))
		{
			return false;
		}

		TMap<const UEdGraphNode*, TArray<const UEdGraphNode*>> Next;
		for (const FClaireonCollapsedEdge& Edge :
			ClaireonExecTopology::CollapseEdges(MacroGraph, /*bIncludeDirect=*/true))
		{
			if (!Edge.FromPin || Edge.FromPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			if (!IsValid(Edge.FromNode) || !IsValid(Edge.ToNode))
			{
				continue;
			}
			Next.FindOrAdd(Edge.FromNode).AddUnique(Edge.ToNode);
		}

		// 0 unvisited, 1 on the current DFS stack, 2 finished. A child marked 1 is a back
		// edge, which is the cycle; a child marked 2 is a diamond, which is not.
		TMap<const UEdGraphNode*, uint8> Mark;
		TArray<TPair<const UEdGraphNode*, int32>> Stack;

		for (const UEdGraphNode* Root : MacroGraph->Nodes)
		{
			if (!IsValid(Root) || Mark.FindRef(Root) != 0)
			{
				continue;
			}

			Stack.Reset();
			Stack.Emplace(Root, 0);
			Mark.Add(Root, 1);

			while (Stack.Num() > 0)
			{
				TPair<const UEdGraphNode*, int32>& Top = Stack.Last();
				const TArray<const UEdGraphNode*>* Succ = Next.Find(Top.Key);
				if (!Succ || Top.Value >= Succ->Num())
				{
					Mark.Add(Top.Key, 2);
					Stack.Pop(EAllowShrinking::No);
					continue;
				}

				// Advance the child cursor BEFORE the push: Top is a reference into Stack
				// and Push may reallocate it.
				const UEdGraphNode* Child = (*Succ)[Top.Value++];
				const uint8 ChildMark = Mark.FindRef(Child);
				if (ChildMark == 1)
				{
					return true;
				}
				if (ChildMark == 0)
				{
					Mark.Add(Child, 1);
					Stack.Emplace(Child, 0);
				}
			}
		}
		return false;
	}

	/**
	 * Branch, Switch, and Cast have known exclusive outputs. An acyclic macro is
	 * only assumed exclusive; other multi-output nodes may fire multiple outputs.
	 */
	bool ClaireonLintGraph_IsBranchExclusivityKnown(const UEdGraphNode* Branch)
	{
		return IsValid(Branch)
			&& (Branch->IsA<UK2Node_IfThenElse>()
				|| Branch->IsA<UK2Node_Switch>()
				|| Branch->IsA<UK2Node_DynamicCast>());
	}

	/** The `branch_exclusivity` evidence value for the branch node the class chose. */
	const TCHAR* ClaireonLintGraph_BranchExclusivity(const UEdGraphNode* Branch)
	{
		if (ClaireonLintGraph_IsBranchExclusivityKnown(Branch))
		{
			return TEXT("known-exclusive");
		}
		if (IsValid(Branch) && Branch->IsA<UK2Node_MacroInstance>())
		{
			return TEXT("assumed-exclusive-macro");
		}
		return TEXT("unknown-exclusivity");
	}

	/**
	 * Find a node whose every executable output reaches Join.Pin. Exclude the joined
	 * node, Sequence, cyclic macros, and unresolved macros. Choose the nearest
	 * candidate by shortest collapsed-edge distance, breaking ties by GUID string.
	 */
	UEdGraphNode* ClaireonLintGraph_FindAllBranchesNode(
		const UEdGraph* Graph,
		const FClaireonExecJoin& Join,
		const FClaireonLintGraphExecFlow& Flow,
		int32& OutCandidateCount,
		int32& OutExecDistance)
	{
		OutCandidateCount = 0;
		OutExecDistance = INDEX_NONE;
		if (!IsValid(Graph) || !Join.Pin)
		{
			return nullptr;
		}

		UEdGraphNode* Chosen = nullptr;
		FString ChosenGuid;
		int32 ChosenDistance = MAX_int32;

		for (UEdGraphNode* Candidate : Graph->Nodes)
		{
			if (!IsValid(Candidate) || Candidate == Join.Node || ClaireonExecTopology::IsKnot(Candidate))
			{
				continue;
			}
			if (ClaireonLintGraph_ExecPinCount(Candidate, EGPD_Output) < 2)
			{
				continue;
			}
			if (Candidate->IsA<UK2Node_ExecutionSequence>())
			{
				continue;
			}
			if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Candidate); IsValid(Macro))
			{
				const UEdGraph* MacroGraph = Macro->GetMacroGraph();
				if (!IsValid(MacroGraph) || ClaireonLintGraph_MacroGraphHasExecCycle(MacroGraph))
				{
					continue;
				}
			}

			// Require every executable output to reach the join; use the shortest of those paths.
			bool bEveryBranchReaches = true;
			int32 CandidateDistance = MAX_int32;
			for (const UEdGraphPin* Pin : Candidate->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output || Pin->bHidden || Pin->bOrphanedPin
					|| Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					continue;
				}
				const int32 PinDistance = ClaireonLintGraph_ExecPinDistance(Flow, Pin, Join.Pin);
				if (PinDistance == INDEX_NONE)
				{
					bEveryBranchReaches = false;
					break;
				}
				CandidateDistance = FMath::Min(CandidateDistance, PinDistance);
			}
			if (!bEveryBranchReaches || CandidateDistance == MAX_int32)
			{
				continue;
			}

			++OutCandidateCount;

			// Break distance ties by fixed-format GUID, not node-array order.
			const FString Guid = Candidate->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
			const bool bBetter = !IsValid(Chosen)
				|| CandidateDistance < ChosenDistance
				|| (CandidateDistance == ChosenDistance && Guid < ChosenGuid);
			if (bBetter)
			{
				Chosen = Candidate;
				ChosenGuid = Guid;
				ChosenDistance = CandidateDistance;
			}
		}

		OutExecDistance = IsValid(Chosen) ? ChosenDistance : INDEX_NONE;
		return Chosen;
	}

	/** Classification precedence: all-branches overrides control-pin, so test it first. */
	enum class EClaireonLintGraphJoinClass : uint8
	{
		FunctionResult = 0,
		AllBranches,
		ControlPin,
		TerminalSingleCallTail,
		SharedTail,
		Count,
	};

	const TCHAR* ClaireonLintGraph_JoinClassName(EClaireonLintGraphJoinClass Class)
	{
		switch (Class)
		{
		case EClaireonLintGraphJoinClass::FunctionResult:         return TEXT("function-result");
		case EClaireonLintGraphJoinClass::AllBranches:            return TEXT("all-branches");
		case EClaireonLintGraphJoinClass::ControlPin:             return TEXT("control-pin");
		case EClaireonLintGraphJoinClass::TerminalSingleCallTail: return TEXT("terminal-single-call-tail");
		default:                                                  return TEXT("shared-tail");
		}
	}

	/** Suggested action for each join class. */
	const TCHAR* ClaireonLintGraph_JoinDisposition(EClaireonLintGraphJoinClass Class)
	{
		switch (Class)
		{
		case EClaireonLintGraphJoinClass::FunctionResult:         return TEXT("ignore");
		case EClaireonLintGraphJoinClass::AllBranches:            return TEXT("offer");
		case EClaireonLintGraphJoinClass::ControlPin:             return TEXT("ignore");
		case EClaireonLintGraphJoinClass::TerminalSingleCallTail: return TEXT("leave");
		default:                                                  return TEXT("extract");
		}
	}

	/**
	 * Exclude ignored classes from severity density so legitimate convergence does
	 * not raise the severity of other joins.
	 */
	bool ClaireonLintGraph_IsJoinReportable(EClaireonLintGraphJoinClass Class)
	{
		return Class == EClaireonLintGraphJoinClass::AllBranches
			|| Class == EClaireonLintGraphJoinClass::TerminalSingleCallTail
			|| Class == EClaireonLintGraphJoinClass::SharedTail;
	}

	/** One join's class and the facts the classification was made from. */
	struct FClaireonLintGraphJoinFacts
	{
		EClaireonLintGraphJoinClass Class = EClaireonLintGraphJoinClass::SharedTail;
		int32 ClosureSize = 0;
		int32 ForeignExecEntries = 0;

		/** All-branches only; null otherwise. */
		UEdGraphNode* BranchNode = nullptr;
		int32 BranchCandidateCount = 0;

		/** Distance used to select BranchNode; INDEX_NONE when no branch qualifies. */
		int32 BranchExecDistance = INDEX_NONE;

		const TCHAR* BranchExclusivity = nullptr;
		bool bBranchExclusivityKnown = false;
	};

	FClaireonLintGraphJoinFacts ClaireonLintGraph_ClassifyJoin(
		const UEdGraph* Graph, const FClaireonLintGraphExecFlow& Flow, const FClaireonExecJoin& Join)
	{
		FClaireonLintGraphJoinFacts Facts;

		// Report closure facts for every class so callers can assess the classification.
		const TSet<const UEdGraphNode*> Closure = ClaireonLintGraph_ForwardExecClosure(Flow, Join.Node);
		Facts.ClosureSize = Closure.Num();
		Facts.ForeignExecEntries = ClaireonLintGraph_CountForeignExecEntries(Flow, Closure, Join.Pin);

		// Function exits legitimately converge at the return node.
		if (IsValid(Join.Node) && Join.Node->IsA<UK2Node_FunctionResult>())
		{
			Facts.Class = EClaireonLintGraphJoinClass::FunctionResult;
			return Facts;
		}

		Facts.BranchNode = ClaireonLintGraph_FindAllBranchesNode(
			Graph, Join, Flow, Facts.BranchCandidateCount, Facts.BranchExecDistance);
		if (IsValid(Facts.BranchNode))
		{
			Facts.Class = EClaireonLintGraphJoinClass::AllBranches;
			Facts.BranchExclusivity = ClaireonLintGraph_BranchExclusivity(Facts.BranchNode);
			Facts.bBranchExclusivityKnown = ClaireonLintGraph_IsBranchExclusivityKnown(Facts.BranchNode);
			return Facts;
		}

		// Ignore control-input convergence only on macros or timelines with multiple
		// executable inputs. A single-input macro remains an ordinary shared tail.
		if (IsValid(Join.Node)
			&& (Join.Node->IsA<UK2Node_MacroInstance>() || Join.Node->IsA<UK2Node_Timeline>())
			&& ClaireonLintGraph_ExecPinCount(Join.Node, EGPD_Input) > 1)
		{
			Facts.Class = EClaireonLintGraphJoinClass::ControlPin;
			return Facts;
		}

		// Leave a one-node terminal tail: extraction adds call sites without simplifying it.
		if (Facts.ClosureSize == 1 && Facts.ForeignExecEntries == 0)
		{
			Facts.Class = EClaireonLintGraphJoinClass::TerminalSingleCallTail;
			return Facts;
		}

		// Other shared tails are extraction candidates, subject to density and semantic checks.
		Facts.Class = EClaireonLintGraphJoinClass::SharedTail;
		return Facts;
	}

	double ClaireonLintGraph_AnchorDistance(const UEdGraphNode* A, const UEdGraphNode* B)
	{
		const double DX = static_cast<double>(B->NodePosX - A->NodePosX);
		const double DY = static_cast<double>(B->NodePosY - A->NodePosY);

		// Round anchor distance to graph units for compact evidence.
		return FMath::RoundToDouble(FMath::Sqrt(DX * DX + DY * DY));
	}

	/** Collect distinct non-knot data consumers through reroutes. */
	void ClaireonLintGraph_CollectDataConsumers(const UEdGraphNode* Node, TArray<UEdGraphNode*>& OutConsumers)
	{
		TSet<const UEdGraphNode*> Visited;
		TArray<const UEdGraphNode*> Frontier;
		Frontier.Add(Node);
		Visited.Add(Node);

		while (Frontier.Num() > 0)
		{
			const UEdGraphNode* Current = Frontier.Pop(EAllowShrinking::No);
			for (UEdGraphPin* Pin : Current->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output)
				{
					continue;
				}
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					UEdGraphNode* Next = Linked ? Linked->GetOwningNode() : nullptr;
					if (!IsValid(Next) || Visited.Contains(Next))
					{
						continue;
					}
					if (ClaireonExecTopology::IsKnot(Next))
					{
						// Presentation only: keep walking to the real consumer.
						Visited.Add(Next);
						Frontier.Add(Next);
						continue;
					}
					Visited.Add(Next);
					OutConsumers.AddUnique(Next);
				}
			}
		}
	}

	/** Deterministic member ordering for a multi-node finding. */
	void ClaireonLintGraph_SortByGuid(TArray<UEdGraphNode*>& Nodes)
	{
		Nodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
		{
			return A.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)
				< B.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
		});
	}

	/** Use the extraction family's shared purity predicate. */
	bool ClaireonLintGraph_IsPureNonKnot(const UEdGraphNode* Node)
	{
		return ClaireonExecTopology::IsPureNonKnot(Node);
	}
	/**
	 * Signature includes class, member reference, and ordered non-exec pin names.
	 * Exclude titles and literal values so equal operations on different constants match.
	 */
	FString ClaireonLintGraph_NodeSignature(const UEdGraphNode* Node)
	{
		if (!IsValid(Node))
		{
			return TEXT("<null>");
		}

		FString Signature = Node->GetClass()->GetName();

		if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node); IsValid(Call))
		{
			Signature += TEXT("|fn=") + Call->FunctionReference.GetMemberName().ToString();
		}
		else if (const UK2Node_Variable* Var = Cast<UK2Node_Variable>(Node); IsValid(Var))
		{
			Signature += TEXT("|var=") + Var->VariableReference.GetMemberName().ToString();
		}

		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				Signature += TEXT("|") + Pin->PinName.ToString();
			}
		}
		return Signature;
	}
}
using namespace ClaireonLintGraphInternal;

void RunGraphRules(const FClaireonLintContext& Context, TArray<FClaireonLintFinding>& OutFindings)
{
	if (!IsValid(Context.Graph))
	{
		return;
	}

	// Emit one graph-level finding with evidence for every join, including ignored
	// classes. RuleVersion 2 identifies this aggregation and eligibility policy.
	// Only this rule requires Context.ExecJoins; other graph rules run without it.
	const TArray<FClaireonExecJoin> NoJoins;
	{
		const TArray<FClaireonExecJoin>& Joins = Context.ExecJoins ? *Context.ExecJoins : NoJoins;

		if (Joins.Num() > 0)
		{
			const FClaireonLintGraphExecFlow Flow = ClaireonLintGraph_BuildExecFlow(Context.Graph);

			int32 ClassCounts[static_cast<int32>(EClaireonLintGraphJoinClass::Count)] = {};
			int32 ReportableCount = 0;
			int32 AllBranchesCount = 0;
			bool bAnyAssumedExclusivity = false;

			// Navigate to the first reportable join; retain every target in evidence.
			bool bHasReportableTarget = false;
			FString ReportableTarget;
			FString FirstJoinTarget;

			TArray<TSharedPtr<FJsonValue>> JoinValues;

			// Preserve FindExecJoins' stable node-GUID/pin order.
			for (const FClaireonExecJoin& Join : Joins)
			{
				const FClaireonLintGraphJoinFacts Facts =
					ClaireonLintGraph_ClassifyJoin(Context.Graph, Flow, Join);
				++ClassCounts[static_cast<int32>(Facts.Class)];

				const FString JoinTarget = ClaireonLintGraph_TargetId(Join.Node, Join.Pin);
				if (FirstJoinTarget.IsEmpty())
				{
					FirstJoinTarget = JoinTarget;
				}
				if (ClaireonLintGraph_IsJoinReportable(Facts.Class))
				{
					++ReportableCount;
					if (!bHasReportableTarget)
					{
						bHasReportableTarget = true;
						ReportableTarget = JoinTarget;
					}
				}
				if (Facts.Class == EClaireonLintGraphJoinClass::AllBranches)
				{
					++AllBranchesCount;
					if (!Facts.bBranchExclusivityKnown)
					{
						bAnyAssumedExclusivity = true;
					}
				}

				bool bAnyLatentOrigin = false;
				TArray<TSharedPtr<FJsonValue>> OriginValues;
				for (const FClaireonCollapsedEdge& Origin : Join.Origins)
				{
					TSharedPtr<FJsonObject> OriginObj = MakeShared<FJsonObject>();
					OriginObj->SetStringField(TEXT("from"), ClaireonLintGraph_TargetId(Origin.FromNode, Origin.FromPin));
					OriginObj->SetStringField(TEXT("from_title"),
						IsValid(Origin.FromNode) ? Origin.FromNode->GetNodeTitle(ENodeTitleType::ListView).ToString() : FString());

					TArray<TSharedPtr<FJsonValue>> ViaValues;
					for (const FGuid& Knot : Origin.ViaKnots)
					{
						ViaValues.Add(MakeShared<FJsonValueString>(Knot.ToString(EGuidFormats::DigitsWithHyphens)));
					}
					OriginObj->SetArrayField(TEXT("via_knots"), ViaValues);
					OriginValues.Add(MakeShared<FJsonValueObject>(OriginObj));

					if (ClaireonLintGraph_IsLatent(Origin.FromNode))
					{
						bAnyLatentOrigin = true;
					}
				}

				TSharedPtr<FJsonObject> JoinObj = MakeShared<FJsonObject>();
				JoinObj->SetStringField(TEXT("target"), JoinTarget);
				JoinObj->SetStringField(TEXT("class"), ClaireonLintGraph_JoinClassName(Facts.Class));
				JoinObj->SetStringField(TEXT("disposition"), ClaireonLintGraph_JoinDisposition(Facts.Class));
				JoinObj->SetNumberField(TEXT("origin_count"), Join.Origins.Num());
				JoinObj->SetArrayField(TEXT("origins"), OriginValues);
				JoinObj->SetBoolField(TEXT("reached_through_reroute"), Join.bAnyViaKnots);

				// Disclose detected latent origins before suggesting Sequence conversion.
				JoinObj->SetBoolField(TEXT("sequence_conversion_safe"), !bAnyLatentOrigin);
				JoinObj->SetStringField(TEXT("sequence_conversion_reason"), bAnyLatentOrigin
					? TEXT("At least one origin is a latent or async node. Converting to Sequence lanes would run the next lane when the latent action starts, not when it completes.")
					: TEXT("No latent or async origin detected. Sequence lanes would preserve ordering, but step reordering is still a judgement call."));

				JoinObj->SetNumberField(TEXT("closure_size"), Facts.ClosureSize);
				JoinObj->SetNumberField(TEXT("foreign_exec_entries"), Facts.ForeignExecEntries);

				if (Facts.Class == EClaireonLintGraphJoinClass::AllBranches)
				{
					JoinObj->SetStringField(TEXT("branch_node"),
						ClaireonLintGraph_TargetId(Facts.BranchNode, nullptr));
					JoinObj->SetStringField(TEXT("branch_node_title"),
						IsValid(Facts.BranchNode)
							? Facts.BranchNode->GetNodeTitle(ENodeTitleType::ListView).ToString()
							: FString());
					JoinObj->SetStringField(TEXT("branch_exclusivity"), Facts.BranchExclusivity);
					JoinObj->SetNumberField(TEXT("branch_candidate_count"), Facts.BranchCandidateCount);

					JoinObj->SetNumberField(TEXT("branch_exec_distance"), Facts.BranchExecDistance);
					if (!Facts.bBranchExclusivityKnown)
					{
						JoinObj->SetStringField(TEXT("branch_exclusivity_note"),
							TEXT("Exclusivity was assumed, not proven: if this branch node can take more than one exec output in a single execution, hoisting the shared call above it changes behaviour. The operator must confirm before acting."));
					}
				}

				JoinValues.Add(MakeShared<FJsonValueObject>(JoinObj));
			}

			FClaireonLintFinding Finding;
			Finding.Rule = TEXT("exec-join");
			Finding.RuleVersion = 2;
			Finding.Scope = EClaireonLintScope::Graph;

			// Warn for all-branches convergence or high reportable-join density.
			Finding.Severity = (AllBranchesCount > 0
					|| ReportableCount >= Context.Thresholds.ExecJoinDensityWarn)
				? EClaireonLintSeverity::Warning
				: EClaireonLintSeverity::Info;

			// Lower the whole finding's confidence when branch exclusivity is assumed.
			Finding.Confidence = bAnyAssumedExclusivity
				? EClaireonLintConfidence::Medium
				: EClaireonLintConfidence::High;

			Finding.Target = bHasReportableTarget ? ReportableTarget : FirstJoinTarget;

			const FString ClassBreakdown = FString::Printf(
				TEXT("%d function-result, %d all-branches, %d control-pin, %d terminal-single-call-tail, %d shared-tail"),
				ClassCounts[static_cast<int32>(EClaireonLintGraphJoinClass::FunctionResult)],
				ClassCounts[static_cast<int32>(EClaireonLintGraphJoinClass::AllBranches)],
				ClassCounts[static_cast<int32>(EClaireonLintGraphJoinClass::ControlPin)],
				ClassCounts[static_cast<int32>(EClaireonLintGraphJoinClass::TerminalSingleCallTail)],
				ClassCounts[static_cast<int32>(EClaireonLintGraphJoinClass::SharedTail)]);

			FString AllBranchesSentence;
			if (AllBranchesCount > 0)
			{
				AllBranchesSentence = FString::Printf(
					TEXT("%d of them classified all-branches: every exec output of one branching node reaches the joined pin, which says the same action runs on every branch. That is worth a human's attention as a possible bug rather than as a wire shape -- hoisting the call above the branch node would leave one unconditional call and no join -- but it is surfaced, not fixed: ordering against the skipped nodes can matter and no tool hoists a node above a branch. "),
					AllBranchesCount);
			}

			Finding.Message = FString::Printf(
				TEXT("%d execution join(s) in this graph, %d reportable (%s). A single join is not the concern -- density is: interlocking joins are what turn a graph into a rat's nest, and the ignored classes are not that at any count, because a function's exits converge by construction and a macro or Timeline control pin is the construct working as designed. %sExtraction does not remove a join: bp_extract_event RELOCATES it to the gateway call, where both origins still converge, so see extraction_caveat in evidence before treating any row as a repair."),
				Joins.Num(), ReportableCount, *ClassBreakdown, *AllBranchesSentence);

			Finding.Evidence = MakeShared<FJsonObject>();
			Finding.Evidence->SetNumberField(TEXT("join_count"), Joins.Num());
			Finding.Evidence->SetNumberField(TEXT("reportable_count"), ReportableCount);

			// Emit all classes, including zero counts, for stable reports.
			TSharedPtr<FJsonObject> ByClass = MakeShared<FJsonObject>();
			for (int32 ClassIndex = 0; ClassIndex < static_cast<int32>(EClaireonLintGraphJoinClass::Count); ++ClassIndex)
			{
				const EClaireonLintGraphJoinClass Class = static_cast<EClaireonLintGraphJoinClass>(ClassIndex);
				ByClass->SetNumberField(ClaireonLintGraph_JoinClassName(Class), ClassCounts[ClassIndex]);
			}
			Finding.Evidence->SetObjectField(TEXT("by_class"), ByClass);
			Finding.Evidence->SetNumberField(TEXT("density_threshold"), Context.Thresholds.ExecJoinDensityWarn);
			Finding.Evidence->SetArrayField(TEXT("joins"), JoinValues);

			Finding.Evidence->SetStringField(TEXT("extraction_caveat"),
				TEXT("bp_extract_event RELOCATES a join to its gateway call rather than removing it: both origins still converge on the single gateway call node. For the join to actually disappear the tail must ALSO be duplicated per origin."));

			// No automatic fix: extraction may reorder work, and no tool performs branch hoisting.
			OutFindings.Add(MoveTemp(Finding));
		}
	}

	for (UEdGraphNode* Node : Context.Graph->Nodes)
	{
		UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node);
		if (!IsValid(Entry))
		{
			continue;
		}

		for (UEdGraphPin* Pin : Entry->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output)
			{
				continue;
			}
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec || Pin->LinkedTo.Num() == 0)
			{
				continue;
			}

			FClaireonLintFinding Finding;
			Finding.Rule = TEXT("entry-param-wire");
			Finding.Scope = EClaireonLintScope::Graph;
			Finding.Severity = EClaireonLintSeverity::Info;
			Finding.Confidence = EClaireonLintConfidence::High;
			Finding.Target = ClaireonLintGraph_TargetId(Entry, Pin);
			Finding.Evidence = MakeShared<FJsonObject>();
			Finding.Evidence->SetNumberField(TEXT("link_count"), Pin->LinkedTo.Num());
			Finding.Message = FString::Printf(
				TEXT("Parameter '%s' is wired from the function entry node to %d consumer(s). A local-scope get placed next to each consumer reads the same value without the wire."),
				*Pin->PinName.ToString(), Pin->LinkedTo.Num());
			OutFindings.Add(MoveTemp(Finding));
		}
	}

	// Flag distant consumers and wide consumer spread, including fan-out whose
	// individual distances remain below threshold.
	for (UEdGraphNode* Node : Context.Graph->Nodes)
	{
		const UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Node);
		if (!IsValid(Get))
		{
			continue;
		}

		TArray<UEdGraphNode*> Consumers;
		ClaireonLintGraph_CollectDataConsumers(Node, Consumers);
		if (Consumers.Num() == 0)
		{
			// Unused gets are orphan-nodes' business, not this rule's.
			continue;
		}
		ClaireonLintGraph_SortByGuid(Consumers);

		double MaxDistance = 0.0;
		for (const UEdGraphNode* Consumer : Consumers)
		{
			MaxDistance = FMath::Max(MaxDistance, ClaireonLintGraph_AnchorDistance(Node, Consumer));
		}

		// Measure spread between consumers to detect one get serving separated regions.
		double Spread = 0.0;
		for (int32 I = 0; I < Consumers.Num(); ++I)
		{
			for (int32 J = I + 1; J < Consumers.Num(); ++J)
			{
				Spread = FMath::Max(Spread, ClaireonLintGraph_AnchorDistance(Consumers[I], Consumers[J]));
			}
		}

		const double Threshold = Context.Thresholds.LocalGetDistance;
		const bool bTooFar = MaxDistance > Threshold;
		const bool bTooSpread = Consumers.Num() > 1 && Spread > Threshold;
		if (!bTooFar && !bTooSpread)
		{
			continue;
		}

		FClaireonLintFinding Finding;
		Finding.Rule = TEXT("distant-get");
		Finding.Scope = EClaireonLintScope::Graph;
		Finding.Severity = EClaireonLintSeverity::Info;
		Finding.Confidence = EClaireonLintConfidence::High;
		Finding.Target = ClaireonLintGraph_TargetId(Node, nullptr);

		TArray<TSharedPtr<FJsonValue>> ConsumerValues;
		for (const UEdGraphNode* Consumer : Consumers)
		{
			TSharedPtr<FJsonObject> ConsumerObj = MakeShared<FJsonObject>();
			ConsumerObj->SetStringField(TEXT("node"), ClaireonLintGraph_TargetId(Consumer, nullptr));
			ConsumerObj->SetStringField(TEXT("title"), Consumer->GetNodeTitle(ENodeTitleType::ListView).ToString());
			ConsumerObj->SetNumberField(TEXT("distance"), ClaireonLintGraph_AnchorDistance(Node, Consumer));
			ConsumerValues.Add(MakeShared<FJsonValueObject>(ConsumerObj));
		}

		const FString VariableName = Get->GetVarName().ToString();
		Finding.Evidence = MakeShared<FJsonObject>();
		Finding.Evidence->SetStringField(TEXT("variable"), VariableName);
		Finding.Evidence->SetNumberField(TEXT("consumer_count"), Consumers.Num());
		Finding.Evidence->SetNumberField(TEXT("max_distance"), MaxDistance);
		Finding.Evidence->SetNumberField(TEXT("consumer_spread"), Spread);
		Finding.Evidence->SetNumberField(TEXT("threshold"), Threshold);
		Finding.Evidence->SetStringField(TEXT("trigger"), bTooFar ? TEXT("distance") : TEXT("spread"));
		Finding.Evidence->SetArrayField(TEXT("consumers"), ConsumerValues);

		Finding.Message = FString::Printf(
			TEXT("Get of '%s' is %.0f units from its furthest of %d consumer(s) (threshold %.0f, spread %.0f). A get placed next to each consumer reads the same value without the wire."),
			*VariableName, MaxDistance, Consumers.Num(), Threshold, Spread);

		// Shared reads may be intentional, so do not offer an automatic duplication fix.
		OutFindings.Add(MoveTemp(Finding));
	}

	// Pure clusters feeding one consumer are extraction candidates. Traverse knots
	// without counting them so layout changes cannot cross the size threshold.
	{
		TSet<const UEdGraphNode*> Assigned;
		for (UEdGraphNode* Seed : Context.Graph->Nodes)
		{
			if (!ClaireonLintGraph_IsPureNonKnot(Seed) || Assigned.Contains(Seed))
			{
				continue;
			}

			// Flood fill over data links between pure nodes, through knots.
			TArray<UEdGraphNode*> Members;
			TArray<UEdGraphNode*> Knots;
			TArray<UEdGraphNode*> Frontier;
			TSet<const UEdGraphNode*> Seen;
			Frontier.Add(Seed);
			Seen.Add(Seed);

			while (Frontier.Num() > 0)
			{
				UEdGraphNode* Current = Frontier.Pop(EAllowShrinking::No);
				if (ClaireonExecTopology::IsKnot(Current))
				{
					Knots.AddUnique(Current);
				}
				else
				{
					Members.AddUnique(Current);
					Assigned.Add(Current);
				}

				for (UEdGraphPin* Pin : Current->Pins)
				{
					if (!Pin)
					{
						continue;
					}
					for (UEdGraphPin* Linked : Pin->LinkedTo)
					{
						UEdGraphNode* Neighbour = Linked ? Linked->GetOwningNode() : nullptr;
						if (!IsValid(Neighbour) || Seen.Contains(Neighbour))
						{
							continue;
						}
						const bool bTraversable = ClaireonExecTopology::IsKnot(Neighbour)
							|| ClaireonLintGraph_IsPureNonKnot(Neighbour);
						if (!bTraversable)
						{
							continue;
						}
						Seen.Add(Neighbour);
						Frontier.Add(Neighbour);
					}
				}
			}

			if (Members.Num() < Context.Thresholds.PureIslandMin)
			{
				continue;
			}

			// Require one distinct external impure consumer; multiple consumers share computation.
			TArray<UEdGraphNode*> ExternalConsumers;
			for (const UEdGraphNode* Member : Seen)
			{
				for (UEdGraphPin* Pin : Member->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Output)
					{
						continue;
					}
					for (UEdGraphPin* Linked : Pin->LinkedTo)
					{
						UEdGraphNode* Consumer = Linked ? Linked->GetOwningNode() : nullptr;
						if (IsValid(Consumer) && !Seen.Contains(Consumer))
						{
							ExternalConsumers.AddUnique(Consumer);
						}
					}
				}
			}

			if (ExternalConsumers.Num() != 1)
			{
				continue;
			}

			ClaireonLintGraph_SortByGuid(Members);

			FClaireonLintFinding Finding;
			Finding.Rule = TEXT("pure-data-island");
			Finding.Scope = EClaireonLintScope::Graph;
			Finding.Severity = EClaireonLintSeverity::Info;
			Finding.Confidence = EClaireonLintConfidence::Medium;
			Finding.Target = ClaireonLintGraph_TargetId(Members[0], nullptr);

			TArray<TSharedPtr<FJsonValue>> MemberValues;
			for (const UEdGraphNode* Member : Members)
			{
				MemberValues.Add(MakeShared<FJsonValueString>(ClaireonLintGraph_TargetId(Member, nullptr)));
			}

			Finding.Evidence = MakeShared<FJsonObject>();
			Finding.Evidence->SetNumberField(TEXT("pure_node_count"), Members.Num());
			Finding.Evidence->SetNumberField(TEXT("reroute_count"), Knots.Num());
			Finding.Evidence->SetNumberField(TEXT("threshold"), Context.Thresholds.PureIslandMin);
			Finding.Evidence->SetArrayField(TEXT("members"), MemberValues);
			Finding.Evidence->SetStringField(TEXT("consumer"),
				ClaireonLintGraph_TargetId(ExternalConsumers[0], nullptr));
			Finding.Evidence->SetStringField(TEXT("consumer_title"),
				ExternalConsumers[0]->GetNodeTitle(ENodeTitleType::ListView).ToString());

			Finding.Message = FString::Printf(
				TEXT("%d pure nodes compute one value used only by '%s'. A pure function named for the value it returns replaces the block with one node at the call site."),
				Members.Num(),
				*ExternalConsumers[0]->GetNodeTitle(ENodeTitleType::ListView).ToString());
			OutFindings.Add(MoveTemp(Finding));
		}
	}
	// Compare knot-collapsed exec chains structurally. Report possible duplication,
	// not proven equivalence: differing values may require extraction parameters.
	{
		const TArray<FClaireonCollapsedEdge> DupEdges =
			ClaireonExecTopology::CollapseEdges(Context.Graph, /*bIncludeDirect=*/true);

		// Exec-only adjacency, plus the in-degrees a linear run is defined by.
		TMap<UEdGraphNode*, TArray<UEdGraphNode*>> DupNext;
		TMap<UEdGraphNode*, int32> DupInDegree;
		for (const FClaireonCollapsedEdge& Edge : DupEdges)
		{
			if (!Edge.FromPin || Edge.FromPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			if (!IsValid(Edge.FromNode) || !IsValid(Edge.ToNode))
			{
				continue;
			}
			DupNext.FindOrAdd(Edge.FromNode).Add(Edge.ToNode);
			DupInDegree.FindOrAdd(Edge.ToNode)++;
			DupInDegree.FindOrAdd(Edge.FromNode);
		}

		// Start only at run heads so a chain does not match its own suffixes.
		TMap<UEdGraphNode*, int32> DupLinearPredecessors;
		for (const TPair<UEdGraphNode*, TArray<UEdGraphNode*>>& Pair : DupNext)
		{
			if (Pair.Value.Num() == 1)
			{
				DupLinearPredecessors.FindOrAdd(Pair.Value[0])++;
			}
		}

		TMap<FString, TArray<UEdGraphNode*>> DupChainsBySignature;
		TMap<FString, int32> DupLengthBySignature;
		TSet<UEdGraphNode*> DupConsumed;

		// Walk node order rather than map order for deterministic findings.
		for (UEdGraphNode* Node : Context.Graph->Nodes)
		{
			if (!IsValid(Node) || Cast<UK2Node_Knot>(Node) || DupConsumed.Contains(Node))
			{
				continue;
			}

			// Mid-run: reached by its predecessor's walk instead.
			const int32* In = DupInDegree.Find(Node);
			const int32* LinearPreds = DupLinearPredecessors.Find(Node);
			if (In && *In == 1 && LinearPreds && *LinearPreds == 1)
			{
				continue;
			}

			TArray<UEdGraphNode*> Chain;
			UEdGraphNode* Cursor = Node;
			while (IsValid(Cursor) && !DupConsumed.Contains(Cursor))
			{
				Chain.Add(Cursor);
				DupConsumed.Add(Cursor);

				const TArray<UEdGraphNode*>* Succ = DupNext.Find(Cursor);
				if (!Succ || Succ->Num() != 1)
				{
					break; // a branch, or the end of the run
				}
				UEdGraphNode* Candidate = (*Succ)[0];
				const int32* CandidateIn = DupInDegree.Find(Candidate);
				if (!CandidateIn || *CandidateIn != 1)
				{
					break; // a join: the run ends rather than absorbing it
				}
				Cursor = Candidate;
			}

			if (Chain.Num() < Context.Thresholds.DuplicateChainMin)
			{
				continue;
			}

			FString Signature;
			for (const UEdGraphNode* ChainNode : Chain)
			{
				Signature += ClaireonLintGraph_NodeSignature(ChainNode) + TEXT(";");
			}
			DupChainsBySignature.FindOrAdd(Signature).Add(Chain[0]);
			DupLengthBySignature.FindOrAdd(Signature) = Chain.Num();
		}

		TArray<FString> DupSignatures;
		DupChainsBySignature.GetKeys(DupSignatures);
		DupSignatures.Sort();

		for (const FString& Signature : DupSignatures)
		{
			const TArray<UEdGraphNode*>& Starts = DupChainsBySignature[Signature];
			if (Starts.Num() < 2)
			{
				continue;
			}

			const int32 Length = DupLengthBySignature[Signature];

			FClaireonLintFinding Finding;
			Finding.Rule = TEXT("duplicate-subgraph");
			Finding.Scope = EClaireonLintScope::Graph;
			Finding.Severity = EClaireonLintSeverity::Info;
			Finding.Confidence = EClaireonLintConfidence::Medium;
			// Use the first occurrence as the target for one finding per duplicate group.
			Finding.Target = ClaireonLintGraph_TargetId(Starts[0], nullptr);
			Finding.Message = FString::Printf(
				TEXT("%d structurally identical exec chains of %d nodes. One extracted ")
				TEXT("function called %d times would remove %d duplicated nodes. Structural ")
				TEXT("identity is not proof the chains mean the same thing -- compare their ")
				TEXT("data inputs before extracting."),
				Starts.Num(), Length, Starts.Num(), (Starts.Num() - 1) * Length);

			TSharedPtr<FJsonObject> Evidence = MakeShared<FJsonObject>();
			Evidence->SetNumberField(TEXT("occurrences"), Starts.Num());
			Evidence->SetNumberField(TEXT("chain_length"), Length);
			Evidence->SetNumberField(TEXT("threshold"), Context.Thresholds.DuplicateChainMin);
			TArray<TSharedPtr<FJsonValue>> StartValues;
			for (UEdGraphNode* Start : Starts)
			{
				StartValues.Add(MakeShared<FJsonValueString>(
					ClaireonLintGraph_TargetId(Start, nullptr)));
			}
			Evidence->SetArrayField(TEXT("chain_starts"), StartValues);
			Evidence->SetStringField(TEXT("signature"), Signature);
			Finding.Evidence = Evidence;

			// Extraction needs a chosen name and parameters for boundary pins.
			OutFindings.Add(MoveTemp(Finding));
		}
	}
}

}
