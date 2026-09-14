// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UEdGraph;
struct FClaireonExecJoin;

namespace ClaireonBlueprintHelpers { enum class EClaireonGraphKind : uint8; }

/** Report-only severity. bp_lint never mutates, so nothing here authorises a change. */
enum class EClaireonLintSeverity : uint8
{
	Info,
	Warning,
};

/**
 * How much the rule actually knows.
 *
 * Unverified is not a weaker Low -- it means the question cannot be answered from
 * this asset alone. Anything reachable from child Blueprints, level scripts,
 * Sequencer, data assets or native code by name is Unverified no matter how the
 * local evidence looks.
 */
enum class EClaireonLintConfidence : uint8
{
	Unverified,
	Medium,
	High,
};

/** Rule scope. Geometry is opt-in; see the default in ResolveScopes. */
enum class EClaireonLintScope : uint8
{
	Variables,
	Graph,
	Layout,
	Functions,
	Hygiene,
};

struct FClaireonLintFinding
{
	/** Stable rule id, e.g. "exec-join". Part of the output contract. */
	FString Rule;

	/**
	 * Bumped when a rule's predicate changes meaning. Emitted per finding so a
	 * fixture diff can distinguish intentional rule evolution from a regression.
	 */
	int32 RuleVersion = 1;

	EClaireonLintSeverity Severity = EClaireonLintSeverity::Info;
	EClaireonLintConfidence Confidence = EClaireonLintConfidence::Medium;
	EClaireonLintScope Scope = EClaireonLintScope::Graph;

	/** Owning graph, stamped centrally by Execute; empty for Blueprint-wide findings. */
	FString GraphName;

	/** Stable identifier for what the finding is about, e.g. "<node-guid>.<pin>". */
	FString Target;

	/** Human-readable. Must never read as authorisation to delete anything. */
	FString Message;

	/** Rule-specific supporting detail. Always populated; never a bare restatement. */
	TSharedPtr<FJsonObject> Evidence;

	/**
	 * A complete callable argument set for a tool that would address the finding,
	 * never a delta. bp_lint does not execute it. May be null when no mechanical
	 * fix is safe -- which is the common case for topology findings.
	 */
	TSharedPtr<FJsonObject> SuggestedFix;
};

/** Default lint thresholds in graph units or node counts. */
struct FClaireonLintThresholds
{
	/** measured: 18 wires above this on BEFORE, 0 on AFTER. */
	double LongWire = 2000.0;

	/** measured: the AFTER event-rail floor. */
	double LocalGetDistance = 512.0;

	/** chosen: verified AFTER island width is 10,368; BEFORE is 22,272. */
	double IslandTooWide = 12000.0;

	/** chosen. */
	double RerouteSpanExec = 1024.0;

	/** chosen. */
	double RerouteSpanData = 2048.0;

	/** chosen. */
	int32 PureIslandMin = 6;

	/** Minimum exec-chain length reported by duplicate-subgraph. */
	int32 DuplicateChainMin = 4;

	/** chosen. */
	int32 UncommentedIslandMin = 20;

	/** Maximum island-entry offset to the right of the rail before island-column reports it. */
	double IslandRailTolerance = 640.0;

	/**
	 * Reportable exec-join count that raises the graph finding from Info to Warning.
	 * Dispositioned joins, such as function exits and Gate Enter pins, do not count.
	 */
	int32 ExecJoinDensityWarn = 4;

};

/** Rule inputs, including precomputed exec topology; rules must not traverse knots. */
struct FClaireonLintContext
{
	const UEdGraph* Graph = nullptr;
	FString GraphName;

	/** Blueprint owning Graph, populated for graph-scoped passes. */
	class UBlueprint* Blueprint = nullptr;
	const TArray<FClaireonExecJoin>* ExecJoins = nullptr;

	/**
	 * Graph population, when known. Rules requiring it must skip when bHasGraphKind
	 * is false; function, interface, and delegate graphs can share an entry-node type.
	 */
	ClaireonBlueprintHelpers::EClaireonGraphKind GraphKind{};
	bool bHasGraphKind = false;

	FClaireonLintThresholds Thresholds;
};

namespace ClaireonLint
{
	CLAIREON_API const TCHAR* ToString(EClaireonLintSeverity Severity);
	CLAIREON_API const TCHAR* ToString(EClaireonLintConfidence Confidence);
	CLAIREON_API const TCHAR* ToString(EClaireonLintScope Scope);

	/** Parse a scope name. Returns false for an unknown string -- callers must error, not ignore. */
	CLAIREON_API bool ParseScope(const FString& Name, EClaireonLintScope& OutScope);

	/** Rule families. Each appends to OutFindings and must not traverse knots. */
	void RunGraphRules(const FClaireonLintContext& Context, TArray<FClaireonLintFinding>& OutFindings);
	void RunLayoutRules(const FClaireonLintContext& Context, TArray<FClaireonLintFinding>& OutFindings);
	void RunHygieneRules(const FClaireonLintContext& Context, TArray<FClaireonLintFinding>& OutFindings);
	void RunVariableRules(const FClaireonLintContext& Context, class UBlueprint* Blueprint, TArray<FClaireonLintFinding>& OutFindings);

	/**
	 * Lint one function declaration for purity and categorization. Graph-scoped so
	 * graph_name filters apply; graph-content rules belong to the other families.
	 */
	void RunFunctionRules(const FClaireonLintContext& Context, TArray<FClaireonLintFinding>& OutFindings);
}
