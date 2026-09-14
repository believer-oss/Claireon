// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintGraphTool_Selection.h"

#include "ClaireonBlueprintHelpers.h"
#include "ClaireonScopedAssetEditor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "GraphEditor.h"
#include "Tools/FToolSchemaBuilder.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;

namespace ClaireonBPSelectionInternal
{
	// Prefix helpers to avoid unity-build collisions.

	/** Machine-readable refusal reasons with distinct recoveries. */
	const TCHAR* SEL_ReasonNoWindow()      { return TEXT("no_editor_window"); }
	const TCHAR* SEL_ReasonUnresolved()    { return TEXT("node_not_found"); }
	const TCHAR* SEL_ReasonForeignGraph()  { return TEXT("node_not_in_session_graph"); }
	const TCHAR* SEL_ReasonBadArgument()   { return TEXT("bad_argument"); }

	FToolResult SEL_Refuse(const FString& Message, const TCHAR* Reason,
		TSharedPtr<FJsonObject> Evidence = nullptr)
	{
		FToolResult Result = IClaireonTool::MakeErrorResult(Message);
		Result.Data = Evidence.IsValid() ? Evidence : MakeShared<FJsonObject>();
		Result.Data->SetStringField(TEXT("refusal_reason"), Reason);
		return Result;
	}

	/**
	 * Resolve the bound widget or return a reason and recovery hint.
	 * Unbound sessions need an editor open; dead bindings require closing the session before reopening.
	 */
	bool SEL_ResolveWidget(const FString& ToolName, const FString& SessionId,
		FBlueprintEditToolData* Data, TSharedPtr<SGraphEditor>& OutWidget, FToolResult& OutError)
	{
		OutWidget.Reset();

		UEdGraph* Graph = Data ? Data->Graph.Get() : nullptr;
		if (!IsValid(Graph))
		{
			OutError = SEL_Refuse(
				TEXT("This session has no active graph, so there is no selection to read or set."),
				SEL_ReasonNoWindow());
			return false;
		}

		EClaireonEditorBindingStatus Status = EClaireonEditorBindingStatus::NotBound;
		OutWidget = Data->EditorBinding.ResolveGraphEditor(Graph, Status);
		if (OutWidget.IsValid())
		{
			return true;
		}

		TSharedPtr<FJsonObject> Evidence = MakeShared<FJsonObject>();
		Evidence->SetStringField(TEXT("binding"), ClaireonEditorBindingStatusToWireString(Status));

		const FString AssetPath = Data->Blueprint.IsValid() ? Data->Blueprint->GetPathName() : FString();

		if (Status == EClaireonEditorBindingStatus::NotBound)
		{
			// No editor has been bound to this session.
			const FString Why = Data->EditorWindow.Reason.IsEmpty()
				? FString(TEXT("this session is not bound to an editor window"))
				: Data->EditorWindow.Reason;

			TSharedPtr<FJsonObject> HintArgs = MakeShared<FJsonObject>();
			HintArgs->SetStringField(TEXT("asset_path"), AssetPath);

			OutError = SEL_Refuse(FString::Printf(
				TEXT("%s needs an open Blueprint editor window and this session has none: %s. "
				     "The selection is Slate state -- it does not exist without a window, and "
				     "reporting success here would claim an effect that did not happen."),
				*ToolName, *Why),
				SEL_ReasonNoWindow(), Evidence);
			OutError.AddHint(IClaireonTool::MakeGuidanceHint(TEXT("bp_open"),
				TEXT("bp_open on this asset opens its editor window and binds the session to it."),
				HintArgs));
			return false;
		}

		// Do not adopt a replacement editor after the binding dies.
		TSharedPtr<FJsonObject> HintArgs = MakeShared<FJsonObject>();
		HintArgs->SetStringField(TEXT("session_id"), SessionId);

		OutError = SEL_Refuse(FString::Printf(
			TEXT("%s cannot reach the editor this session bound to (%s). The session will not "
			     "silently move to a different window, so it needs replacing rather than "
			     "reopening: close it and open a new one."),
			*ToolName, ClaireonEditorBindingStatusToWireString(Status)),
			SEL_ReasonNoWindow(), Evidence);
		OutError.AddHint(IClaireonTool::MakeGuidanceHint(TEXT("bp_close"),
			TEXT("close this session, then bp_open the asset again -- a session bound to a dead "
			     "editor cannot be repaired by opening a window."),
			HintArgs));
		return false;
	}

	/** The selection as node GUIDs, plus whatever had to be filtered out and why. */
	void SEL_ReadSelection(const TSharedPtr<SGraphEditor>& Widget, UEdGraph* Graph,
		TArray<UEdGraphNode*>& OutNodes, TArray<TSharedPtr<FJsonValue>>& OutFiltered)
	{
		for (UObject* Selected : Widget->GetSelectedNodes())
		{
			UEdGraphNode* Node = Cast<UEdGraphNode>(Selected);
			if (!IsValid(Node))
			{
				// Report stale or non-node selection entries as exclusions.
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("object"),
					IsValid(Selected) ? Selected->GetPathName() : TEXT("<null>"));
				Entry->SetStringField(TEXT("reason"), TEXT("not_a_graph_node"));
				OutFiltered.Add(MakeShared<FJsonValueObject>(Entry));
				continue;
			}
			if (Node->GetGraph() != Graph)
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("node_guid"),
					Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				Entry->SetStringField(TEXT("reason"), TEXT("not_in_session_graph"));
				OutFiltered.Add(MakeShared<FJsonValueObject>(Entry));
				continue;
			}
			OutNodes.Add(Node);
		}
	}

	TArray<TSharedPtr<FJsonValue>> SEL_GuidArray(const TArray<UEdGraphNode*>& Nodes)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Nodes.Num());
		for (const UEdGraphNode* Node : Nodes)
		{
			Out.Add(MakeShared<FJsonValueString>(
				Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)));
		}
		return Out;
	}
}

using namespace ClaireonBPSelectionInternal;



FString ClaireonBlueprintGraphTool_SelectionGet::GetOperation() const { return TEXT("selection_get"); }

FString ClaireonBlueprintGraphTool_SelectionGet::GetDescription() const
{
    return TEXT("Get the editor's current node selection for the session's graph, as node GUIDs. "
                "Reads live Slate state through the editor instance this session bound to, never "
                "an ambient one, so it needs an open window and refuses without one. Selected "
                "objects that are not nodes of that graph are reported, not silently dropped.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_SelectionGet::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'status')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_SelectionGet::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	TSharedPtr<FJsonObject> Params;
	FString SessionId;
	FBlueprintEditToolData* Data = nullptr;
	FToolResult Error;
	if (!BeginSessionOp(Arguments, TEXT("selection_get"), Params, SessionId, Data, Error))
	{
		return Error;
	}

	TSharedPtr<SGraphEditor> Widget;
	if (!SEL_ResolveWidget(GetName(), SessionId, Data, Widget, Error))
	{
		return Error;
	}

	TArray<UEdGraphNode*> Nodes;
	TArray<TSharedPtr<FJsonValue>> Filtered;
	SEL_ReadSelection(Widget, Data->Graph.Get(), Nodes, Filtered);

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetArrayField(TEXT("selected_node_guids"), SEL_GuidArray(Nodes));
	ResponseData->SetNumberField(TEXT("selected_count"), Nodes.Num());
	ResponseData->SetArrayField(TEXT("filtered_out"), Filtered);
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetStringField(TEXT("graph_name"), Data->Graph->GetName());

	return MakeSuccessResult(ResponseData, FString::Printf(
		TEXT("%d node(s) selected in '%s'%s."), Nodes.Num(), *Data->Graph->GetName(),
		Filtered.Num() > 0
			? *FString::Printf(TEXT(", %d selected object(s) filtered out"), Filtered.Num())
			: TEXT("")));
}



FString ClaireonBlueprintGraphTool_SelectionSet::GetOperation() const { return TEXT("selection_set"); }

FString ClaireonBlueprintGraphTool_SelectionSet::GetDescription() const
{
    return TEXT("Set the editor's node selection to exactly these GUIDs, resolved against the "
                "session's graph. Validates the whole set before touching Slate, so a bad GUID "
                "refuses atomically and changes nothing; duplicates are deduplicated, not "
                "refused. Opens no transaction, so transaction_undo cannot restore a selection.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_SelectionSet::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddArray(TEXT("node_guids"), TEXT("Node GUIDs to select (full GUID or >=8-hex prefix). An empty array clears the selection; use bp_selection_clear when that is the intent."), true);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'status')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_SelectionSet::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	TSharedPtr<FJsonObject> Params;
	FString SessionId;
	FBlueprintEditToolData* Data = nullptr;
	FToolResult Error;
	if (!BeginSessionOp(Arguments, TEXT("selection_set"), Params, SessionId, Data, Error))
	{
		return Error;
	}

	const TArray<TSharedPtr<FJsonValue>>* RawGuids = nullptr;
	if (!Params->TryGetArrayField(TEXT("node_guids"), RawGuids) || !RawGuids)
	{
		return SEL_Refuse(TEXT("Missing required field: node_guids (an array of node GUID strings)."),
			SEL_ReasonBadArgument());
	}

	TSharedPtr<SGraphEditor> Widget;
	if (!SEL_ResolveWidget(GetName(), SessionId, Data, Widget, Error))
	{
		return Error;
	}
	UEdGraph* Graph = Data->Graph.Get();

	// Validate the full set before changing Slate selection; undo cannot restore partial selection changes.
	TArray<UEdGraphNode*> Resolved;
	TSet<FGuid> Seen;
	int32 DuplicatesRemoved = 0;

	for (const TSharedPtr<FJsonValue>& Value : *RawGuids)
	{
		FString GuidString;
		if (!Value.IsValid() || !Value->TryGetString(GuidString))
		{
			return SEL_Refuse(
				TEXT("node_guids must contain only strings. Nothing was selected or deselected."),
				SEL_ReasonBadArgument());
		}

		FString ResolveError;
		UEdGraphNode* Node = nullptr;
		// Use the shared resolver for full GUIDs, prefixes, and recompile recovery.
		ClaireonBlueprintHelpers::ResolveNodeGuidString(Graph, GuidString, Node, ResolveError,
			TEXT("node_guids"));
		if (!IsValid(Node))
		{
			return SEL_Refuse(FString::Printf(
				TEXT("bp_selection_set refuses the whole set because '%s' did not resolve: %s. "
				     "The selection is unchanged -- validation runs before any Slate state is "
				     "touched, so a bad entry cannot leave a partial selection behind."),
				*GuidString, *ResolveError),
				SEL_ReasonUnresolved());
		}
		if (Node->GetGraph() != Graph)
		{
			return SEL_Refuse(FString::Printf(
				TEXT("bp_selection_set refuses the whole set because '%s' belongs to graph '%s', "
				     "not the session's graph '%s'. The selection is unchanged."),
				*GuidString,
				IsValid(Node->GetGraph()) ? *Node->GetGraph()->GetName() : TEXT("<none>"),
				*Graph->GetName()),
				SEL_ReasonForeignGraph());
		}

		bool bAlreadySeen = false;
		Seen.Add(Node->NodeGuid, &bAlreadySeen);
		if (bAlreadySeen)
		{
			++DuplicatesRemoved;
			continue;
		}
		Resolved.Add(Node);
	}

	Widget->ClearSelectionSet();
	for (UEdGraphNode* Node : Resolved)
	{
		Widget->SetNodeSelection(Node, /*bSelect=*/true);
	}

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Selected %d node(s) in %s"), Resolved.Num(), *Graph->GetName());

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetArrayField(TEXT("selected_node_guids"), SEL_GuidArray(Resolved));
	ResponseData->SetNumberField(TEXT("selected_count"), Resolved.Num());
	ResponseData->SetNumberField(TEXT("duplicates_removed"), DuplicatesRemoved);
	ResponseData->SetBoolField(TEXT("opened_transaction"), false);
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetStringField(TEXT("graph_name"), Graph->GetName());

	return MakeSuccessResult(ResponseData, FString::Printf(
		TEXT("Selected %d node(s) in '%s'%s. No transaction was opened; transaction_undo will "
		     "not restore the previous selection."),
		Resolved.Num(), *Graph->GetName(),
		DuplicatesRemoved > 0
			? *FString::Printf(TEXT(" (%d duplicate(s) deduplicated)"), DuplicatesRemoved)
			: TEXT("")));
}



FString ClaireonBlueprintGraphTool_SelectionClear::GetOperation() const { return TEXT("selection_clear"); }

FString ClaireonBlueprintGraphTool_SelectionClear::GetDescription() const
{
    return TEXT("Clear the editor's node selection for the session's graph, through the editor "
                "instance this session bound to. Needs an open window and refuses without one "
                "rather than reporting a clear that did not happen. Opens no transaction and "
                "creates no undo entry, so transaction_undo cannot put the selection back.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_SelectionClear::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'status')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_SelectionClear::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	TSharedPtr<FJsonObject> Params;
	FString SessionId;
	FBlueprintEditToolData* Data = nullptr;
	FToolResult Error;
	if (!BeginSessionOp(Arguments, TEXT("selection_clear"), Params, SessionId, Data, Error))
	{
		return Error;
	}

	TSharedPtr<SGraphEditor> Widget;
	if (!SEL_ResolveWidget(GetName(), SessionId, Data, Widget, Error))
	{
		return Error;
	}

	TArray<UEdGraphNode*> Before;
	TArray<TSharedPtr<FJsonValue>> Filtered;
	SEL_ReadSelection(Widget, Data->Graph.Get(), Before, Filtered);

	Widget->ClearSelectionSet();

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Cleared the selection in %s"), *Data->Graph->GetName());

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetNumberField(TEXT("cleared_count"), Before.Num());
	ResponseData->SetArrayField(TEXT("cleared_node_guids"), SEL_GuidArray(Before));
	ResponseData->SetBoolField(TEXT("opened_transaction"), false);
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetStringField(TEXT("graph_name"), Data->Graph->GetName());

	return MakeSuccessResult(ResponseData, FString::Printf(
		TEXT("Cleared %d selected node(s) in '%s'. No transaction was opened; transaction_undo "
		     "will not put the selection back."),
		Before.Num(), *Data->Graph->GetName()));
}

#undef LOCTEXT_NAMESPACE
