// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class UEdGraph;
class UEdGraphNode;

/**
 * Connected components over exec and data links, including knots but excluding comment nodes.
 * Members sort by GUID string; islands sort by their first member's GUID.
 */
namespace ClaireonGraphIslands
{
	/** Bounds over node anchors, excluding widget sizes, which are unavailable headless. */
	struct CLAIREON_API FIslandBox
	{
		double MinX = 0.0;
		double MinY = 0.0;
		double MaxX = 0.0;
		double MaxY = 0.0;

		double Width() const { return MaxX - MinX; }
		double Height() const { return MaxY - MinY; }

		bool Intersects(const FIslandBox& Other) const
		{
			return MinX <= Other.MaxX && Other.MinX <= MaxX
				&& MinY <= Other.MaxY && Other.MinY <= MaxY;
		}
	};

	/** One connected component of the graph, over links of any category. */
	struct CLAIREON_API FIsland
	{
		/** Members, sorted by GUID string. Never empty. */
		TArray<UEdGraphNode*> Nodes;

		FIslandBox Box;

		/**
		 * The first member's GUID as a hyphenated string. Formatting can change this identity
		 * by adding or deleting knots; match post-mutation reports using member GUID sets.
		 */
		FString Representative;

		bool Contains(const UEdGraphNode* Node) const { return Nodes.Contains(Node); }
	};

	/** True for valid nodes other than comment nodes, including knots. */
	CLAIREON_API bool IsIslandMember(const UEdGraphNode* Node);

	/** A node's GUID as a hyphenated string, or "<null>". */
	CLAIREON_API FString NodeId(const UEdGraphNode* Node);

	/** True for UK2Node_Event (including subclasses) or UK2Node_FunctionEntry. */
	CLAIREON_API bool IsEntryNode(const UEdGraphNode* Node);

	/**
	 * X of the leftmost entry node, with GUID-string tie breaking; Box.MinX if no entry exists.
	 * Align this entry to the rail, rather than the island bounds, to keep stacking idempotent.
	 */
	CLAIREON_API double ResolveIslandEntryX(const FIsland& Island);

	/** The minimum entry-node X, falling back to the minimum island Box.MinX, then zero. */
	CLAIREON_API double ResolveRailX(const UEdGraph* Graph);

	/**
	 * Root position for a new island: rail X and maximum node-anchor Y plus Gutter.
	 * Includes comment anchors but excludes node heights; an empty graph uses Y = 0.
	 */
	struct CLAIREON_API FStackSlot
	{
		double X = 0.0;
		double Y = 0.0;
	};
	CLAIREON_API FStackSlot AllocateSlot(const UEdGraph* Graph, double Gutter);

	/** Default vertical gap in graph units, measured between anchors rather than widget bounds. */
	inline constexpr double DefaultStackGutter = 512.0;

	/**
	 * Replace OutIslands with components in ascending representative-GUID order.
	 * An invalid graph clears OutIslands.
	 */
	CLAIREON_API void Build(const UEdGraph* Graph, TArray<FIsland>& OutIslands);
}
