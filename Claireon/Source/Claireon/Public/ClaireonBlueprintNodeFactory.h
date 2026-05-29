// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;

/**
 * Single source of truth for K2 Blueprint node construction from a typed JSON spec.
 *
 * Used by all three Blueprint node-creation call sites:
 *   - bp_add_node (session-mode, ClaireonBlueprintGraphTool_AddNode::AddNode_Impl)
 *   - bp_apply_spec (batch, ClaireonSpecApplicator_Blueprint::ApplyPass1_CreateEntities)
 *   - bp_apply_delta (batch direct, ClaireonTool_ApplyBlueprintDelta::Execute)
 *
 * Routes by node_type to typed branches that handle pin/property allocation,
 * AllocateDefaultPins, and (for dynamic-pin nodes) ReconstructNode.
 *
 * Factory responsibilities:
 *   1. Resolve node_type to a UK2Node class (typed dispatch, same surface as Operation_AddNode).
 *   2. Construct + configure the node (typed properties, struct/class/enum resolution).
 *   3. Add to graph, AllocateDefaultPins(), apply num_extra_pins.
 *
 * Callers remain responsible for: transaction wrapping, cursor updates, auto_connect,
 * MarkBlueprintAsStructurallyModified, response building.
 *
 * Node types not yet absorbed (handled inline in AddNode_Impl):
 *   EventOverride, FunctionEntry, FunctionResult, Tunnel, Timeline,
 *   AddDelegate, RemoveDelegate, ClearDelegate, CallDelegate, CreateDelegate,
 *   AssignDelegate, ComponentBoundEvent.
 */
namespace ClaireonBlueprintNodeFactory
{
	/**
	 * Result of a node-creation attempt.
	 */
	struct FCreateResult
	{
		/** Created node. null on failure. */
		UEdGraphNode* Node = nullptr;

		/** Human-readable node description for status/log messages. */
		FString Description;

		/** Non-fatal resolution notes (e.g., fuzzy-match warnings). */
		TArray<FString> Warnings;

		/** Populated on failure. */
		FString Error;

		/**
		 * True if the factory already called Graph->AddNode + AllocateDefaultPins itself
		 * (e.g., AssignDelegate with a companion CustomEvent).
		 * When false, the factory has produced a configured UEdGraphNode* that still
		 * needs Graph->AddNode + AllocateDefaultPins — but the factory DOES handle that
		 * by default; this flag is for advanced callers that want to override placement.
		 */
		bool bAlreadyAdded = false;

		bool IsOk() const { return Error.IsEmpty() && Node != nullptr; }
	};

	/**
	 * Parameters understood by CreateNode. See Operation_AddNode's GetFullDescription for the
	 * per-type schema; CreateNode accepts the same `node_type` dispatch plus typed params.
	 *
	 * Required JSON field: `node_type` (string).
	 * Optional: `position` object {x,y} — if unset, caller places the node.
	 * Optional: type-specific params (function_name, variable_name, struct_type, ...).
	 * Optional: `num_extra_pins` (int, 0-50) — applied after AllocateDefaultPins.
	 *
	 * Returns a FCreateResult whose Node (on success) is already added to Graph and has
	 * AllocateDefaultPins called. On failure, Node is null and Error is populated.
	 */
	CLAIREON_API FCreateResult CreateNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TSharedPtr<FJsonObject>& Params,
		const FVector2D& Position);
}
