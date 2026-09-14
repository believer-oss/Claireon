// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;

/** Shared knot traversal for lint topology and collapsed-edge output. */

/** One edge with any intervening knot chain collapsed away. */
struct FClaireonCollapsedEdge
{
	/** Originating non-knot node. */
	UEdGraphNode* FromNode = nullptr;

	/** Originating output pin on FromNode. */
	UEdGraphPin* FromPin = nullptr;

	/** First non-knot node reached. */
	UEdGraphNode* ToNode = nullptr;

	/** Input pin on ToNode. */
	UEdGraphPin* ToPin = nullptr;

	/**
	 * Knot node GUIDs traversed, in order from FromPin toward ToPin.
	 * Empty means FromPin links directly to ToPin with no reroute between them.
	 */
	TArray<FGuid> ViaKnots;

	bool IsDirect() const { return ViaKnots.Num() == 0; }
};

/**
 * An exec input reached by at least two distinct originating (node, pin) pairs.
 * Multiple knot paths from the same output pin count as one origin.
 */
struct FClaireonExecJoin
{
	/** The non-knot node owning the joined input pin. Never a knot. */
	UEdGraphNode* Node = nullptr;

	/** The exec input pin that two or more origins reach. */
	UEdGraphPin* Pin = nullptr;

	/** The collapsed edges arriving at Pin, one per distinct origin. */
	TArray<FClaireonCollapsedEdge> Origins;

	/** True when at least one origin arrives through a reroute chain. */
	bool bAnyViaKnots = false;
};

namespace ClaireonExecTopology
{
	/**
	 * Every edge in Graph, with knot chains collapsed.
	 *
	 * Knots never appear as FromNode or ToNode. A knot output that fans out to
	 * several terminal non-knot inputs yields one edge per terminal input, each
	 * carrying its own ViaKnots path. Knot cycles are guarded and contribute
	 * nothing.
	 *
	 * @param bIncludeDirect  include zero-knot edges. bp_get_graph passes false
	 *                        because direct edges are already in its connections[];
	 *                        rules that reason about topology pass true.
	 */
	CLAIREON_API TArray<FClaireonCollapsedEdge> CollapseEdges(const UEdGraph* Graph, bool bIncludeDirect);

	/**
	 * Exec input pins reached by two or more distinct origins, per the definition
	 * on FClaireonExecJoin.
	 *
	 * Results are ordered deterministically so lint findings are stable across
	 * runs: by node GUID, then by pin name.
	 */
	CLAIREON_API TArray<FClaireonExecJoin> FindExecJoins(const UEdGraph* Graph);

	/** True when Node is a reroute node. */
	CLAIREON_API bool IsKnot(const UEdGraphNode* Node);

	/**
	 * True for pure K2 nodes other than reroutes. Shared by lint and extraction.
	 * Excludes lint eligibility checks and extraction's separate by-reference mutation analysis.
	 */
	CLAIREON_API bool IsPureNonKnot(const UEdGraphNode* Node);
}
