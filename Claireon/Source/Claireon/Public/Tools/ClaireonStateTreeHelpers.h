// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UStateTree;
class UStateTreeEditorData;
class UStateTreeState;
class UStateTreeSchema;
class UScriptStruct;
struct FStateTreeEditorNode;
struct FStateTreeTransition;
struct FStateTreeBindableStructDesc;

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

	/**
	 * Format the full State Tree structure as structured text. Optionally focus on a specific state.
	 *
	 * `IncludedSections`: empty/null -> emit all four sections. Otherwise only sections whose
	 * lower-case name appears in the array are emitted. Recognized names:
	 *   "global_evaluators", "global_tasks", "states", "bindings"
	 * Unknown names are ignored (no error). The header block is always emitted.
	 */
	FString FormatStateTreeStructure(
		UStateTreeEditorData* EditorData,
		const FGuid* FocusStateId = nullptr,
		const TArray<FString>* IncludedSections = nullptr);

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

	/**
	 * Walks a dot-path on a UStruct, returning the leaf FProperty + the address inside StructPtr.
	 * Returns false and sets OutError if any segment is unresolvable.
	 *
	 * Owned by F1. F2 and F4 depend on this signature only -- they do NOT redeclare or fork it.
	 */
	bool ResolvePropertyPath(
		const UStruct* RootStruct,
		void* StructPtr,
		const FString& PropertyPath,
		FProperty*& OutLeafProperty,
		void*& OutLeafAddress,
		FString& OutError);

	/** Sets a property on UStateTreeState by dot-path. Mirrors SetNodeProperty's ImportText_Direct flow. */
	bool SetStateProperty(
		UStateTreeState& State,
		const FString& PropertyName,
		const FString& PropertyValue,
		FString& OutError);

	/**
	 * Sets a property on FStateTreeTransition by dot-path. Refuses fields owned by ModifyTransition
	 * (Target / State / StateLink). Bare `bConsumeEventOnSelect` is normalized to `RequiredEvent.bConsumeEventOnSelect`.
	 */
	bool SetTransitionProperty(
		FStateTreeTransition& Transition,
		const FString& PropertyName,
		const FString& PropertyValue,
		FString& OutError);

	/**
	 * Emits a single FStateTreeBindableStructDesc as a JSON record (name, guid, struct, source_type, state_path).
	 *
	 * Owned by F3. F5 (list_binding_sources) consumes this signature only -- it does NOT redeclare or fork it.
	 */
	TSharedPtr<FJsonObject> EmitBindingSourceRecord(const FStateTreeBindableStructDesc& Desc);

	/**
	 * Sets a property on a UStateTreeSchema by dot-path via ImportText_Direct.
	 * Caller is responsible for invoking PostEditChangeChainProperty after a successful write
	 * so subclass overrides re-run their reflection updaters.
	 */
	bool SetSchemaProperty(
		UStateTreeSchema& Schema,
		const FString& PropertyName,
		const FString& PropertyValue,
		FString& OutError);
} // namespace ClaireonStateTreeHelpers
