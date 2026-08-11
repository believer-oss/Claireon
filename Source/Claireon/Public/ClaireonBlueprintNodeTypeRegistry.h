// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

/**
 * Single source of truth for bp_add_node's node_type space.
 *
 * bp_add_node's dispatch and bp_list_node_types both read this table, so the
 * documented surface and the dispatchable surface cannot drift: previously the
 * authoritative sets were function-local (FactoryHandledNodeTypes in
 * ClaireonBlueprintGraphTool_AddNode.cpp, GKnownMacros in
 * ClaireonBlueprintGraphEditToolBase_Internal.cpp) and the schema prose listing
 * them was hand-maintained and non-exhaustive.
 */
namespace ClaireonBlueprintNodeTypes
{
	/** How bp_add_node builds this node_type. */
	enum class ENodeTypeKind : uint8
	{
		/** Routed to ClaireonBlueprintNodeFactory::CreateNode. */
		Factory,

		/** Constructed by a dedicated branch inside bp_add_node's Execute. */
		Inline,

		/** Rewritten to a MacroInstance of an engine StandardMacros entry. */
		Shorthand,

		/** The Generic + class_name escape hatch. */
		Generic
	};

	struct FNodeTypeInfo
	{
		/** The node_type string callers pass. */
		const TCHAR* Alias = nullptr;

		ENodeTypeKind Kind = ENodeTypeKind::Factory;

		/** Params bp_add_node rejects the call without, beyond session/asset identity. */
		TArray<FString> RequiredParams;

		/** Params that change the result but are not required. */
		TArray<FString> OptionalParams;

		const TCHAR* Description = nullptr;
	};

	/** The full table, one entry per dispatchable node_type. */
	CLAIREON_API const TArray<FNodeTypeInfo>& GetRegistry();

	/** True when bp_add_node routes this node_type through the node factory. */
	CLAIREON_API bool IsFactoryHandled(const FString& NodeType);

	/** True when this node_type is a shorthand for an engine StandardMacros macro. */
	CLAIREON_API bool IsMacroShorthand(const FString& NodeType);

	/** Every Shorthand-kind alias, for the macro-shorthand rewrite. */
	CLAIREON_API const TArray<FString>& GetMacroShorthandNames();
}
