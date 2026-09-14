// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonBlueprintGraphEditToolBase.h"

#include "EdGraph/EdGraphPin.h"

/**
 * Function, macro, and composite extraction use the engine collapse commands.
 * Event extraction has no menu equivalent and enforces its own boundary restrictions.
 */
DECLARE_BPGRAPH_TOOL_FULLDESC(ClaireonBlueprintGraphTool_ExtractFunction);
DECLARE_BPGRAPH_TOOL_FULLDESC(ClaireonBlueprintGraphTool_ExtractMacro);
DECLARE_BPGRAPH_TOOL_FULLDESC(ClaireonBlueprintGraphTool_ExtractComposite);
DECLARE_BPGRAPH_TOOL_FULLDESC(ClaireonBlueprintGraphTool_ExtractEvent);

class UEdGraph;

/**
 * Composite collapse uses the editor's focused graph, not an explicit graph argument.
 * Require it to match the resolved target before opening a transaction or mutating.
 */
namespace ClaireonExtractQuiescence
{
	/**
	 * Empty when Resolved and Focused are the same non-null graph -- the only case in which
	 * a composite collapse is safe to run. Otherwise the caller-facing refusal text,
	 * naming both graphs and stating that nothing was mutated.
	 */
	CLAIREON_API FString FocusedGraphMismatchRefusal(const UEdGraph* Resolved, const UEdGraph* Focused);
}

class UEdGraphNode;
class UEdGraphPin;

/** Selection predicates for automatic purity and enclosing-local promotion. */
namespace ClaireonExtractSemantics
{
	/**
	 * Enclosing-local reference classifications. Function extraction refuses all by default;
	 * promote_enclosing_locals permits read-only references only. Values map to stable wire strings.
	 */
	enum class EEnclosingLocalReason : uint8
	{
		/**
		 * An explicit VariableSet of an enclosing local. Wire: "explicit_write".
		 * Refused even under promotion: turning a read/write local into input-plus-output
		 * with downstream reads rewired is sound only given dominance and
		 * intervening-write analysis, which this tool does not perform.
		 */
		ExplicitWrite,

		/**
		 * Wire: "reaches_mutable_reference_pin". Data flow reaches a potentially mutable
		 * reference, including ArrayParm pins. Promotion would discard the mutation and is refused.
		 */
		ReachesMutableReferencePin,

		/**
		 * Wire: "read_only_reference". Accepted only when function extraction opts into
		 * promotion and all enclosing-local references are read-only.
		 */
		ReadOnlyReference,
	};

	/** One refused reference, named so the caller can act on it. */
	struct FEnclosingLocalFinding
	{
		/** The local or parameter's own name, which is what the caller has to see. */
		FString VariableName;

		/** Hyphenated NodeGuid of the Get/Set node inside the selection. */
		FString NodeGuid;

		EEnclosingLocalReason Reason = EEnclosingLocalReason::ExplicitWrite;

		/** For ReachesMutableReferencePin: all qualifying reached pins as "<node guid>.<pin name>". */
		TArray<FString> ReachedReferencePins;
	};

	/** The wire vocabulary for a finding's reason. */
	CLAIREON_API const TCHAR* ToWireString(EEnclosingLocalReason Reason);

	/**
	 * Walk data links through pure nodes and knots to find potentially mutable reference pins.
	 * A reference qualifies if non-const or marked ArrayParm: CustomThunk array functions
	 * can mutate arrays despite const native signatures. Test pins on impure nodes too.
	 *
	 * @param OutReachedReferencePins Appended with qualifying pins as "<node guid>.<pin name>".
	 */
	CLAIREON_API bool ReachesMutableReferencePin(
		const UEdGraphPin* Pin, TArray<FString>& OutReachedReferencePins);

	/** True when the call's ArrayParm metadata identifies this pin as an array parameter. */
	CLAIREON_API bool IsArrayParmPin(const UEdGraphPin* Pin);

	/**
	 * Append local/parameter references identified by IsLocalScope, sorted by node GUID.
	 * The reason determines whether promotion can accept each reference.
	 */
	CLAIREON_API void FindEnclosingLocalViolations(
		const TSet<UEdGraphNode*>& Selection, TArray<FEnclosingLocalFinding>& OutFindings);

	/** Caller-facing refusal text for a non-empty finding set. Empty when Findings is empty. */
	CLAIREON_API FString EnclosingLocalRefusal(
		const TCHAR* ToolName, const TArray<FEnclosingLocalFinding>& Findings);

	/**
	 * Opt-in function promotion replaces read-only local Gets with entry input parameters
	 * and wires equivalent reads at the call site. Macro/composite locals remain in the
	 * enclosing scope; event extraction refuses crossing data pins.
	 */

	/** One local accepted for promotion. Sort keys are captured BEFORE the collapse moves anything. */
	struct FPromotionCandidate
	{
		/** The enclosing local's own name. The parameter takes this name unless it collides. */
		FString VariableName;

		/**
		 * The parameter's pin type: the earliest referencing VariableGet's VALUE PIN type,
		 * copied verbatim by PromotedParameterPinType and then adjusted by exactly two rules.
		 */
		FEdGraphPinType ParameterPinType;

		/** Hyphenated NodeGuid of the earliest referencing VariableGet, for evidence. */
		FString EarliestNodeGuid;

		/** Every referencing VariableGet of this local inside the selection. One parameter for all of them. */
		TArray<UEdGraphNode*> ReferencingGetNodes;

		/** The comparator's three keys, read off the earliest value pin before the collapse. */
		int32 SortNodePosY = 0;
		int32 SortNodePosX = 0;
		int32 SortPinIndex = 0;
	};

	/**
	 * Copy the resolved pin type without a string-grammar round trip, preserving qualifiers
	 * and container detail. Clear bIsReference for input-copy semantics and bIsWeakPointer
	 * for non-containers, matching engine boundary promotion.
	 */
	CLAIREON_API FEdGraphPinType PromotedParameterPinType(const FEdGraphPinType& SourcePinType);

	/**
	 * Order by exec pins first, then node Y, node X, and pin index, matching the engine
	 * boundary-pin comparator. Promotion compares Get pins directly, not gateway links.
	 */
	CLAIREON_API bool PinPrecedesByConnectionPosition(const UEdGraphPin* A, const UEdGraphPin* B);

	/**
	 * Build a plan before collapse: one parameter per local, ordered by its earliest Get.
	 * Refuse writes, mutable references, unresolved Gets, connected split subpins, and
	 * value links outside the selection; deleting those Gets would lose data flow.
	 *
	 * @return True for a usable plan, including an empty plan when nothing needs promotion.
	 */
	CLAIREON_API bool BuildPromotionPlan(
		const TSet<UEdGraphNode*>& Selection,
		const TArray<FEnclosingLocalFinding>& Findings,
		TArray<FPromotionCandidate>& OutPlan,
		FString& OutRefusal);

	/**
	 * True when every selected non-knot node is pure and no exec edge enters or leaves.
	 * Does not require lint's single-external-consumer restriction.
	 *
	 * @param OutWhyNot On false, the failed condition.
	 */
	CLAIREON_API bool IsSelectionExecFree(const TSet<UEdGraphNode*>& Selection, FString& OutWhyNot);

	/**
	 * Process-wide extraction diagnostic counts, including messages without a resolvable
	 * graph token. Also logged per compile under CLAIREON_EXTRACT_DIAG_TALLY; resettable for tests.
	 */
	CLAIREON_API void GetDiagnosticTokenTally(int32& OutTotalDiagnostics, int32& OutTokenlessDiagnostics);

	/** Zero the tally, so one driver run can be measured in isolation. */
	CLAIREON_API void ResetDiagnosticTokenTally();
}
