// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonBlueprintEditorAccess.h"

#include "BlueprintEditor.h"
#include "GraphEditor.h"

namespace ClaireonBlueprintEditorAccess
{
namespace ClaireonBPEditorAccessInternal
{

	/**
	 * Explicit instantiation bypasses member access checks ([temp.spec]/6).
	 * Each instantiation exposes its member pointer through a friend found by tag ADL.
	 */
	template <typename Tag, typename Tag::FType Member>
	struct TClaireonAccess
	{
		friend constexpr typename Tag::FType ClaireonAccessGet(Tag) { return Member; }
	};

	struct FTagCanCollapseFunction
	{
		using FType = bool (FBlueprintEditor::*)(TSet<UEdGraphNode*>&) const;
		friend constexpr FType ClaireonAccessGet(FTagCanCollapseFunction);
	};
	template struct TClaireonAccess<FTagCanCollapseFunction,
		&FBlueprintEditor::CanCollapseSelectionToFunction>;

	struct FTagCanCollapseMacro
	{
		using FType = bool (FBlueprintEditor::*)(TSet<UEdGraphNode*>&) const;
		friend constexpr FType ClaireonAccessGet(FTagCanCollapseMacro);
	};
	template struct TClaireonAccess<FTagCanCollapseMacro,
		&FBlueprintEditor::CanCollapseSelectionToMacro>;

	struct FTagCollapseFunction
	{
		using FType = UEdGraph* (FBlueprintEditor::*)(
			TSharedPtr<SGraphEditor>, TSet<UEdGraphNode*>&, UEdGraphNode*&);
		friend constexpr FType ClaireonAccessGet(FTagCollapseFunction);
	};
	template struct TClaireonAccess<FTagCollapseFunction,
		&FBlueprintEditor::CollapseSelectionToFunction>;

	struct FTagCollapseMacro
	{
		using FType = UEdGraph* (FBlueprintEditor::*)(
			TSharedPtr<SGraphEditor>, TSet<UEdGraphNode*>&, UEdGraphNode*&);
		friend constexpr FType ClaireonAccessGet(FTagCollapseMacro);
	};
	template struct TClaireonAccess<FTagCollapseMacro,
		&FBlueprintEditor::CollapseSelectionToMacro>;

	struct FTagCollapseNodes
	{
		using FType = void (FBlueprintEditor::*)(TSet<UEdGraphNode*>&);
		friend constexpr FType ClaireonAccessGet(FTagCollapseNodes);
	};
	template struct TClaireonAccess<FTagCollapseNodes, &FBlueprintEditor::CollapseNodes>;
}

bool CanCollapseToFunction(const FBlueprintEditor& Editor, TSet<UEdGraphNode*>& Selection)
{
	using namespace ClaireonBPEditorAccessInternal;
	return (Editor.*ClaireonAccessGet(FTagCanCollapseFunction{}))(Selection);
}

bool CanCollapseToMacro(const FBlueprintEditor& Editor, TSet<UEdGraphNode*>& Selection)
{
	using namespace ClaireonBPEditorAccessInternal;
	return (Editor.*ClaireonAccessGet(FTagCanCollapseMacro{}))(Selection);
}

UEdGraph* CollapseToFunction(FBlueprintEditor& Editor, TSharedPtr<SGraphEditor> RootGraph,
                             TSet<UEdGraphNode*>& Selection, UEdGraphNode*& OutGatewayNode)
{
	using namespace ClaireonBPEditorAccessInternal;
	return (Editor.*ClaireonAccessGet(FTagCollapseFunction{}))(RootGraph, Selection, OutGatewayNode);
}

UEdGraph* CollapseToMacro(FBlueprintEditor& Editor, TSharedPtr<SGraphEditor> RootGraph,
                          TSet<UEdGraphNode*>& Selection, UEdGraphNode*& OutGatewayNode)
{
	using namespace ClaireonBPEditorAccessInternal;
	return (Editor.*ClaireonAccessGet(FTagCollapseMacro{}))(RootGraph, Selection, OutGatewayNode);
}

void CollapseToComposite(FBlueprintEditor& Editor, TSet<UEdGraphNode*>& Selection)
{
	using namespace ClaireonBPEditorAccessInternal;
	(Editor.*ClaireonAccessGet(FTagCollapseNodes{}))(Selection);
}
}
