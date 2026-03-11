// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class UStateTree;
class UStateTreeEditorData;
class UStateTreeState;
class UScriptStruct;
struct FStateTreeEditorNode;
struct FStateTreeTransition;

/**
 * Shared utility functions for State Tree MCP tools.
 * Provides asset loading, node lookup, formatting, and property manipulation.
 */
namespace ClaireonStateTreeHelpers
{
	/** Load and validate a State Tree asset from an asset path. */
	UStateTree* LoadStateTreeAsset(const FString& AssetPath, FString& OutError);

	/** Get the editor data from a State Tree asset. */
	UStateTreeEditorData* GetEditorData(UStateTree* StateTree, FString& OutError);

	/** Recursively search for a state by GUID through SubTrees/Children. */
	UStateTreeState* FindStateById(UStateTreeEditorData* EditorData, const FGuid& StateId);

	/** Search all node arrays for a node by GUID (evaluators, global tasks, per-state tasks/conditions/considerations). */
	FStateTreeEditorNode* FindNodeById(UStateTreeEditorData* EditorData, const FGuid& NodeId);

	/** Search a state's transitions for a transition by GUID. */
	FStateTreeTransition* FindTransitionById(UStateTreeState* State, const FGuid& TransitionId);

	/** Format the full State Tree structure as structured text. Optionally focus on a specific state. */
	FString FormatStateTreeStructure(UStateTreeEditorData* EditorData, const FGuid* FocusStateId = nullptr);

	/** Format a single state and its immediate children. */
	FString FormatStateArea(UStateTreeState* State);

	/** Format a single editor node (type name + key property values). */
	FString FormatEditorNode(const FStateTreeEditorNode& Node);

	/** Resolve a struct name string to a UScriptStruct*. */
	UScriptStruct* ResolveNodeStruct(const FString& StructName, FString& OutError);

	/** Create and initialize an FStateTreeEditorNode with correct instance data (handles both struct and UObject patterns). */
	bool CreateEditorNode(FStateTreeEditorNode& OutNode, UScriptStruct* NodeStruct, UObject* Outer, FString& OutError);

	/** Set a property value on a node via ImportText. */
	bool SetNodeProperty(FStateTreeEditorNode& Node, const FString& PropertyName, const FString& PropertyValue, bool bOnInstanceData, FString& OutError);
} // namespace ClaireonStateTreeHelpers
