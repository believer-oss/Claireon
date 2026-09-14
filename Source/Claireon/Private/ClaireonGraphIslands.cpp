// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonGraphIslands.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"

namespace ClaireonGraphIslands
{
	bool IsIslandMember(const UEdGraphNode* Node)
	{
		return IsValid(Node) && !Node->IsA<UEdGraphNode_Comment>();
	}

	FString NodeId(const UEdGraphNode* Node)
	{
		return IsValid(Node) ? Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : TEXT("<null>");
	}

	void Build(const UEdGraph* Graph, TArray<FIsland>& OutIslands)
	{
		OutIslands.Reset();

		if (!IsValid(Graph))
		{
			return;
		}

		TSet<const UEdGraphNode*> Visited;
		for (UEdGraphNode* Seed : Graph->Nodes)
		{
			if (!IsIslandMember(Seed) || Visited.Contains(Seed))
			{
				continue;
			}

			FIsland Island;
			TArray<UEdGraphNode*> Frontier;
			Frontier.Add(Seed);
			Visited.Add(Seed);

			while (Frontier.Num() > 0)
			{
				UEdGraphNode* Current = Frontier.Pop(EAllowShrinking::No);
				Island.Nodes.Add(Current);

				for (UEdGraphPin* Pin : Current->Pins)
				{
					if (!Pin)
					{
						continue;
					}
					for (UEdGraphPin* Linked : Pin->LinkedTo)
					{
						UEdGraphNode* Neighbour = Linked ? Linked->GetOwningNode() : nullptr;
						if (!IsIslandMember(Neighbour) || Visited.Contains(Neighbour))
						{
							continue;
						}
						Visited.Add(Neighbour);
						Frontier.Add(Neighbour);
					}
				}
			}

			Island.Nodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
			{
				return A.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)
					< B.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
			});
			Island.Representative = NodeId(Island.Nodes[0]);

			Island.Box.MinX = Island.Box.MaxX = static_cast<double>(Island.Nodes[0]->NodePosX);
			Island.Box.MinY = Island.Box.MaxY = static_cast<double>(Island.Nodes[0]->NodePosY);
			for (const UEdGraphNode* Node : Island.Nodes)
			{
				Island.Box.MinX = FMath::Min(Island.Box.MinX, static_cast<double>(Node->NodePosX));
				Island.Box.MaxX = FMath::Max(Island.Box.MaxX, static_cast<double>(Node->NodePosX));
				Island.Box.MinY = FMath::Min(Island.Box.MinY, static_cast<double>(Node->NodePosY));
				Island.Box.MaxY = FMath::Max(Island.Box.MaxY, static_cast<double>(Node->NodePosY));
			}

			OutIslands.Add(MoveTemp(Island));
		}

		OutIslands.Sort([](const FIsland& A, const FIsland& B)
		{
			return A.Representative < B.Representative;
		});
	}

	bool IsEntryNode(const UEdGraphNode* Node)
	{
		return IsValid(Node)
			&& (Node->IsA<UK2Node_Event>() || Node->IsA<UK2Node_FunctionEntry>());
	}

	double ResolveIslandEntryX(const FIsland& Island)
	{
		// Build sorts by GUID, so the first minimum-X entry wins ties.
		const UEdGraphNode* Primary = nullptr;
		for (const UEdGraphNode* Node : Island.Nodes)
		{
			if (!IsEntryNode(Node))
			{
				continue;
			}
			if (!IsValid(Primary) || Node->NodePosX < Primary->NodePosX)
			{
				Primary = Node;
			}
		}

		return IsValid(Primary)
			? static_cast<double>(Primary->NodePosX)
			: Island.Box.MinX;
	}

	double ResolveRailX(const UEdGraph* Graph)
	{
		if (!IsValid(Graph))
		{
			return 0.0;
		}

		// Measure the same entry coordinate that stacking aligns to keep stacking idempotent.
		bool bFoundEntry = false;
		double EntryMinX = 0.0;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsEntryNode(Node))
			{
				continue;
			}
			const double X = static_cast<double>(Node->NodePosX);
			EntryMinX = bFoundEntry ? FMath::Min(EntryMinX, X) : X;
			bFoundEntry = true;
		}
		if (bFoundEntry)
		{
			return EntryMinX;
		}

		TArray<FIsland> Islands;
		Build(Graph, Islands);
		bool bFoundIsland = false;
		double IslandMinX = 0.0;
		for (const FIsland& Island : Islands)
		{
			IslandMinX = bFoundIsland ? FMath::Min(IslandMinX, Island.Box.MinX) : Island.Box.MinX;
			bFoundIsland = true;
		}
		return bFoundIsland ? IslandMinX : 0.0;
	}

	FStackSlot AllocateSlot(const UEdGraph* Graph, double Gutter)
	{
		FStackSlot Slot;
		Slot.X = ResolveRailX(Graph);

		if (!IsValid(Graph))
		{
			return Slot;
		}

		// Include comment anchors; the gutter provides clearance for unmeasured node heights.
		bool bAny = false;
		double MaxY = 0.0;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node))
			{
				continue;
			}
			const double Y = static_cast<double>(Node->NodePosY);
			MaxY = bAny ? FMath::Max(MaxY, Y) : Y;
			bAny = true;
		}

		Slot.Y = bAny ? MaxY + Gutter : 0.0;
		return Slot;
	}
}
