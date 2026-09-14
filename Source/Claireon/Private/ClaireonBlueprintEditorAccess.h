// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class FBlueprintEditor;
class SGraphEditor;
class UEdGraph;
class UEdGraphNode;

/**
 * Access to the engine's collapse commands through protected member pointers.
 * Bound to engine member signatures; signature changes intentionally fail compilation.
 */
namespace ClaireonBlueprintEditorAccess
{
	/**
	 * The engine's own eligibility check, so a refusal here matches a greyed-out menu
	 * item. Non-const TSet& because that is the engine's signature.
	 */
	bool CanCollapseToFunction(const FBlueprintEditor& Editor, TSet<UEdGraphNode*>& Selection);
	bool CanCollapseToMacro(const FBlueprintEditor& Editor, TSet<UEdGraphNode*>& Selection);

	/**
	 * Collapse Selection into a new function graph. Returns the created graph and, via
	 * OutGatewayNode, the call node left behind at the original site.
	 */
	UEdGraph* CollapseToFunction(FBlueprintEditor& Editor, TSharedPtr<SGraphEditor> RootGraph,
	                             TSet<UEdGraphNode*>& Selection, UEdGraphNode*& OutGatewayNode);

	UEdGraph* CollapseToMacro(FBlueprintEditor& Editor, TSharedPtr<SGraphEditor> RootGraph,
	                          TSet<UEdGraphNode*>& Selection, UEdGraphNode*& OutGatewayNode);

	/** Collapse into a composite (sub-)graph -- the menu's plain "Collapse Nodes". */
	void CollapseToComposite(FBlueprintEditor& Editor, TSet<UEdGraphNode*>& Selection);
}
