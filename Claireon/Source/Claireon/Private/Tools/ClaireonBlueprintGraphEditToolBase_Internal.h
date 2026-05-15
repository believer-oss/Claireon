// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

// Internal helpers shared across the decomposed ClaireonBlueprintGraphTool_*.cpp
// translation units. Previously these lived as file-local (anonymous namespace
// or static) helpers inside ClaireonBlueprintGraphEditToolBase.cpp. Stage 023
// extracted each per-operation body into its own decomposed cpp; to keep those
// extracted bodies compiling without duplication, the helpers were promoted
// to this internal header and a single companion translation unit
// (ClaireonBlueprintGraphEditToolBase_Internal.cpp).
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
}
