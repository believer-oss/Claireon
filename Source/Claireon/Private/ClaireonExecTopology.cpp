// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonExecTopology.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_Knot.h"

namespace ClaireonExecTopology
{

bool IsKnot(const UEdGraphNode* Node)
{
	return IsValid(Node) && Node->IsA<UK2Node_Knot>();
}

bool IsPureNonKnot(const UEdGraphNode* Node)
{
	const UK2Node* K2 = Cast<const UK2Node>(Node);
	return IsValid(K2) && K2->IsNodePure() && !IsKnot(Node);
}

namespace ClaireonExecTopologyInternal
{
	/** One frontier entry while walking a knot chain forward. */
	struct FClaireonExecTopologyKnotWalk
	{
		/** The knot input pin we arrived at. */
		UEdGraphPin* InputPin = nullptr;

		/** Knot GUIDs traversed to reach InputPin, in order. */
		TArray<FGuid> ViaKnots;
	};

	bool IsExecPin(const UEdGraphPin* Pin)
	{
		return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
	}

	/**
	 * Walk forward from a link that lands on a knot, emitting one collapsed edge per
	 * terminal non-knot input reached.
	 *
	 * The visited set is scoped to this single walk -- that is, to one originating
	 * output pin. Two branches from the same origin reaching the same knot are the
	 * same origin, so visiting it once is correct and also bounds knot cycles.
	 */
	void WalkKnotChain(
		UEdGraphNode* FromNode,
		UEdGraphPin* FromPin,
		UEdGraphPin* FirstKnotInput,
		TArray<FClaireonCollapsedEdge>& OutEdges)
	{
		TArray<FClaireonExecTopologyKnotWalk> Frontier;
		TSet<const UEdGraphNode*> VisitedKnots;
		Frontier.Add({FirstKnotInput, {}});

		while (Frontier.Num() > 0)
		{
			const FClaireonExecTopologyKnotWalk Entry = Frontier.Pop(EAllowShrinking::No);
			UEdGraphNode* KnotNode = Entry.InputPin ? Entry.InputPin->GetOwningNode() : nullptr;
			if (!IsValid(KnotNode) || VisitedKnots.Contains(KnotNode))
			{
				continue;
			}
			VisitedKnots.Add(KnotNode);

			TArray<FGuid> Via = Entry.ViaKnots;
			Via.Add(KnotNode->NodeGuid);

			for (UEdGraphPin* KnotPin : KnotNode->Pins)
			{
				if (!KnotPin || KnotPin->Direction != EGPD_Output)
				{
					continue;
				}

				for (UEdGraphPin* NextPin : KnotPin->LinkedTo)
				{
					UEdGraphNode* NextNode = NextPin ? NextPin->GetOwningNode() : nullptr;
					if (!IsValid(NextNode))
					{
						continue;
					}

					if (IsKnot(NextNode))
					{
						Frontier.Add({NextPin, Via});
						continue;
					}

					FClaireonCollapsedEdge Edge;
					Edge.FromNode = FromNode;
					Edge.FromPin = FromPin;
					Edge.ToNode = NextNode;
					Edge.ToPin = NextPin;
					Edge.ViaKnots = Via;
					OutEdges.Add(MoveTemp(Edge));
				}
			}
		}
	}
}
using namespace ClaireonExecTopologyInternal;

TArray<FClaireonCollapsedEdge> CollapseEdges(const UEdGraph* Graph, bool bIncludeDirect)
{
	TArray<FClaireonCollapsedEdge> Edges;
	if (!IsValid(Graph))
	{
		return Edges;
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!IsValid(Node) || IsKnot(Node))
		{
			continue;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output)
			{
				continue;
			}

			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				UEdGraphNode* Target = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				if (!IsValid(Target))
				{
					continue;
				}

				if (!IsKnot(Target))
				{
					if (bIncludeDirect)
					{
						FClaireonCollapsedEdge Edge;
						Edge.FromNode = Node;
						Edge.FromPin = Pin;
						Edge.ToNode = Target;
						Edge.ToPin = LinkedPin;
						Edges.Add(MoveTemp(Edge));
					}
					continue;
				}

				WalkKnotChain(Node, Pin, LinkedPin, Edges);
			}
		}
	}

	return Edges;
}

TArray<FClaireonExecJoin> FindExecJoins(const UEdGraph* Graph)
{
	TArray<FClaireonExecJoin> Joins;
	if (!IsValid(Graph))
	{
		return Joins;
	}

	// Group every exec arrival by its destination pin, deduplicating origins by
	// originating (node, pin). Several knot paths from one output pin are one origin.
	struct FGroup
	{
		UEdGraphNode* Node = nullptr;
		UEdGraphPin* Pin = nullptr;
		TArray<FClaireonCollapsedEdge> Origins;
		TSet<TPair<const UEdGraphNode*, const UEdGraphPin*>> SeenOrigins;
	};

	TMap<const UEdGraphPin*, FGroup> Groups;

	for (const FClaireonCollapsedEdge& Edge : CollapseEdges(Graph, /*bIncludeDirect=*/true))
	{
		if (!IsExecPin(Edge.ToPin) || Edge.ToPin->Direction != EGPD_Input)
		{
			continue;
		}

		FGroup& Group = Groups.FindOrAdd(Edge.ToPin);
		Group.Node = Edge.ToNode;
		Group.Pin = Edge.ToPin;

		const TPair<const UEdGraphNode*, const UEdGraphPin*> OriginKey(Edge.FromNode, Edge.FromPin);
		if (Group.SeenOrigins.Contains(OriginKey))
		{
			continue;
		}
		Group.SeenOrigins.Add(OriginKey);
		Group.Origins.Add(Edge);
	}

	for (TPair<const UEdGraphPin*, FGroup>& Pair : Groups)
	{
		FGroup& Group = Pair.Value;
		if (Group.Origins.Num() < 2)
		{
			continue;
		}

		FClaireonExecJoin Join;
		Join.Node = Group.Node;
		Join.Pin = Group.Pin;
		Join.Origins = MoveTemp(Group.Origins);
		for (const FClaireonCollapsedEdge& Origin : Join.Origins)
		{
			if (!Origin.IsDirect())
			{
				Join.bAnyViaKnots = true;
				break;
			}
		}
		Joins.Add(MoveTemp(Join));
	}

	// Sort for stable lint output.
	Joins.Sort([](const FClaireonExecJoin& A, const FClaireonExecJoin& B)
	{
		const FString AGuid = IsValid(A.Node) ? A.Node->NodeGuid.ToString() : FString();
		const FString BGuid = IsValid(B.Node) ? B.Node->NodeGuid.ToString() : FString();
		if (AGuid != BGuid)
		{
			return AGuid < BGuid;
		}
		const FString APin = A.Pin ? A.Pin->PinName.ToString() : FString();
		const FString BPin = B.Pin ? B.Pin->PinName.ToString() : FString();
		return APin < BPin;
	});

	return Joins;
}

}
