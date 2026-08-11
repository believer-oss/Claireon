// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

// Internal helpers shared across the decomposed ClaireonBlueprintGraphTool_*.cpp
// translation units. The helpers live here and in a single companion
// translation unit (ClaireonBlueprintGraphEditToolBase_Internal.cpp) so the
// decomposed bodies compile without duplication.
//
// Not exported via CLAIREON_API -- intra-module only.

#include "CoreMinimal.h"
#include "ClaireonBlueprintHelpers.h"

class UBlueprint;
class UEdGraph;
class UEdGraphNode;
struct FGuid;

namespace ClaireonMacroShorthand
{
	/** Rewrite Params: expand known macro-shorthand node_type into full MacroInstance spec. No-op otherwise. */
	void ResolveIfShorthand(const TSharedPtr<FJsonObject>& Params);
}

namespace ClaireonNodeTypeAlias
{
	/** Lookup friendly alias for a node UClass (e.g. UK2Node_CallFunction -> "CallFunction"). */
	FString GetAliasForNodeClass(const UClass* NodeClass);

	/** Rewrite Params->node_type in-place: resolve a raw UClass name to its alias (or Generic+class_name). */
	void ResolveNodeTypeAlias(const TSharedPtr<FJsonObject>& Params);
}

namespace ClaireonBPGraphInternal
{
	/** Pick the first "entry" node appropriate to the graph type (anim pose root, function entry, macro tunnel, ubergraph root). */
	UEdGraphNode* SelectEntryNodeForSwitch(const UBlueprint* Blueprint, UEdGraph* Graph);

	/** Wraps FindNodeByGuid and records A-field fallback corrections into Data (when non-null). */
	UEdGraphNode* FindNodeForOperation(UEdGraph* Graph, const FGuid& RequestedGuid, FBlueprintEditToolData* Data);

	/**
	 * String-input sibling of FindNodeForOperation. Resolves NodeGuidStr via the shared
	 * ClaireonBlueprintHelpers::ResolveNodeGuidString (full GUID or >=8-hex prefix) and,
	 * when the A-field recompile-recovery fallback fires on a full-GUID input, records the
	 * correction into Data->GuidCorrections exactly as FindNodeForOperation did -- so the
	 * "GUID Corrections" response note keeps firing. Prefix-resolved lookups record no
	 * correction (there is no requested full GUID to key). On failure returns nullptr and
	 * fills OutError. FieldName customizes the "Invalid <field> format" text.
	 */
	UEdGraphNode* FindNodeForOperationStr(UEdGraph* Graph, const FString& NodeGuidStr, FBlueprintEditToolData* Data,
	                                      FString& OutError, const TCHAR* FieldName = TEXT("node_guid"));

	/**
	 * Remove references to trashed (bWasTrashed) pins from every live pin's LinkedTo across
	 * all graphs of the Blueprint. A trashed pin left in a LinkedTo array asserts during
	 * SavePackage (EdGraphPin.cpp "serialized while trashed") and bakes load-crash corruption
	 * into the saved asset. Returns the number of stale references removed; fills
	 * OutDetails with one human-readable line per removal.
	 */
	int32 ScrubTrashedPinLinks(UBlueprint* Blueprint, TArray<FString>& OutDetails);
}
