// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UAnimBlueprint;
class UAnimBlueprintGeneratedClass;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UAnimGraphNode_Base;
class UAnimationStateMachineGraph;
class UAnimStateTransitionNode;
class UAnimStateNode;

/**
 * Shared utility functions for Animation Graph MCP inspection tools.
 * Provides asset loading, graph enumeration, node serialization,
 * property binding inspection, fast path analysis, and thread safety checks.
 */
namespace ClaireonAnimGraphHelpers
{
	// ========================================================================
	// Data Structures
	// ========================================================================

	/** Metadata about a graph found within an animation blueprint. */
	struct FAnimGraphInfo
	{
		/** Display name of the graph */
		FString Name;

		/** Type classification: AnimGraph, StateMachine, StateGraph, TransitionGraph, ConduitGraph */
		FString Type;

		/** Number of nodes in this graph */
		int32 NodeCount = 0;

		/** Pointer to the actual graph */
		UEdGraph* Graph = nullptr;

		/** Name of the parent graph (empty for root AnimGraph) */
		FString ParentGraphName;
	};

	// ========================================================================
	// Asset Loading
	// ========================================================================

	/** Load and validate a UAnimBlueprint from an asset path. */
	UAnimBlueprint* LoadAnimBlueprint(const FString& AssetPath, FString& OutError);

	// ========================================================================
	// Graph Enumeration
	// ========================================================================

	/**
	 * Recursively collect all graphs within an animation blueprint.
	 * Traverses AnimGraph roots, state machines, state pose graphs,
	 * transition condition graphs, linked anim graphs, and conduits.
	 */
	TArray<FAnimGraphInfo> CollectAllGraphs(UAnimBlueprint* AnimBP);

	/**
	 * Find a graph by name within an animation blueprint.
	 * Searches the flattened CollectAllGraphs list.
	 * On failure, populates OutError with available graph names.
	 */
	UEdGraph* FindAnimGraphByName(UAnimBlueprint* AnimBP, const FString& GraphName, FString& OutError);

	// ========================================================================
	// Node Classification
	// ========================================================================

	/**
	 * Classify an animation graph node into a human-readable category.
	 * Returns: state_machine, sequence_player, blend_list, layered_bone_blend,
	 * blend_space, aim_offset, linked_anim_graph, linked_anim_layer, output_pose,
	 * cached_pose_save, cached_pose_use, montage_slot, transition_result,
	 * state, state_entry, transition, conduit, pose_search, blend_stack,
	 * control_rig, inertialization, anim_node (generic), unknown.
	 */
	FString GetAnimNodeCategory(const UEdGraphNode* Node);

	// ========================================================================
	// Node Serialization
	// ========================================================================

	/**
	 * Serialize an animation graph node to JSON.
	 * DetailLevel: "summary" (type/title/connections), "full" (all pins, bindings, fast path),
	 *              "nodes" (all pins but no bindings).
	 */
	TSharedPtr<FJsonObject> SerializeAnimGraphNode(UEdGraphNode* Node, const FString& DetailLevel, UAnimBlueprint* AnimBP = nullptr);

	/** Serialize all pins on a node with full connection details and type info. */
	TArray<TSharedPtr<FJsonValue>> SerializeAllPins(const UEdGraphNode* Node, bool bIncludeDefaults = true);

	/**
	 * Serialize the runtime FAnimNode struct properties from an UAnimGraphNode_Base.
	 * Uses UProperty reflection to read the inner FAnimNode member.
	 */
	TSharedPtr<FJsonObject> SerializeAnimNodeProperties(UAnimGraphNode_Base* AnimNode);

	// ========================================================================
	// Property Bindings & Fast Path
	// ========================================================================

	/** Serialize property bindings on an anim graph node. */
	TSharedPtr<FJsonObject> SerializePropertyBindings(UAnimGraphNode_Base* AnimNode);

	/**
	 * Analyze fast path compliance for an anim graph node.
	 * Returns true if all bindings are fast-path. Populates OutWarnings with details.
	 */
	bool AnalyzeFastPath(UAnimGraphNode_Base* AnimNode, TArray<FString>& OutWarnings);

	// ========================================================================
	// Linked Layer Inspection
	// ========================================================================

	/**
	 * For linked anim layer nodes, extract the interface they implement,
	 * whether they're connected, and the bound class.
	 * Returns nullptr if the node is not a linked layer.
	 */
	TSharedPtr<FJsonObject> SerializeLinkedLayerInfo(UAnimGraphNode_Base* AnimNode);

	// ========================================================================
	// Node-Bound Event Functions
	// ========================================================================

	/**
	 * Enumerate event functions bound to a node (OnBecomeRelevant, OnUpdate,
	 * OnStateEntered, OnStateLeft, OnStateFullyBlended, etc.).
	 */
	TArray<TSharedPtr<FJsonValue>> SerializeNodeBoundEvents(UEdGraphNode* Node, UAnimBlueprint* AnimBP);

	// ========================================================================
	// State Machine
	// ========================================================================

	/** Serialize state machine topology: entry state, states, transitions, conduits. */
	TSharedPtr<FJsonObject> SerializeStateMachine(UAnimationStateMachineGraph* SMGraph);

	/** Serialize full transition details including blend settings and condition graph. */
	TSharedPtr<FJsonObject> SerializeTransition(UAnimStateTransitionNode* TransNode);

	// ========================================================================
	// Class Settings & Blueprint Metadata
	// ========================================================================

	/** Serialize animation blueprint class settings (parent, skeleton, interfaces, flags). */
	TSharedPtr<FJsonObject> SerializeClassSettings(UAnimBlueprint* AnimBP);

	/** Serialize all blueprint variables. */
	TArray<TSharedPtr<FJsonValue>> SerializeVariables(UAnimBlueprint* AnimBP);

	/** Serialize all blueprint functions with thread safety flags. */
	TArray<TSharedPtr<FJsonValue>> SerializeFunctions(UAnimBlueprint* AnimBP);

	// ========================================================================
	// Analysis & Warnings
	// ========================================================================

	/** Analyze thread safety of functions in the animation blueprint. */
	TSharedPtr<FJsonObject> AnalyzeThreadSafety(UAnimBlueprint* AnimBP);

	/** Collect all warnings: fast path, thread safety, compiler messages. */
	TSharedPtr<FJsonObject> CollectWarnings(UAnimBlueprint* AnimBP);

} // namespace ClaireonAnimGraphHelpers
