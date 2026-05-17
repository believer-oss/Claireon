// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintGraphTool_SetNodeProperty.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase_Internal.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "ClaireonBlueprintHelpers.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlueprintGraphTool_SetNodeProperty::GetOperation() const
{
	return TEXT("set_node_property");
}

TArray<FString> ClaireonBlueprintGraphTool_SetNodeProperty::GetSearchKeywords() const
{
	return {TEXT("bp"), TEXT("node"), TEXT("set"), TEXT("property"), TEXT("config"),
		TEXT("reconstruct"), TEXT("async"), TEXT("cast"), TEXT("switch"), TEXT("enum"),
		TEXT("makestruct"), TEXT("proxy"), TEXT("factory")};
}

FString ClaireonBlueprintGraphTool_SetNodeProperty::GetDescription() const
{
	return TEXT("Set a UPROPERTY on a K2 node in the current session's graph. Targets the node "
		"object itself (not its pins or the owning CDO). Use for protected node fields that drive "
		"pin layout: K2Node_DynamicCast.TargetType, K2Node_SwitchEnum.Enum, K2Node_MakeStruct.StructType, "
		"K2Node_BaseAsyncTask.ProxyFactoryClass/ProxyFactoryFunctionName, and similar. property_path "
		"supports dot/array navigation through nested structs (e.g. 'ProxyClass' or "
		"'StructType.Struct'). Calls ReconstructNode() after the write by default so dynamic pins "
		"materialize. Session-mode tool: open via blueprint_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_SetNodeProperty::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
	Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
	Builder.AddString(TEXT("node_guid"), TEXT("GUID of the node whose property is being set."), true);
	Builder.AddString(TEXT("property_name"), TEXT("Name of the property to set on the node."), true);
	Builder.AddString(TEXT("property_value"), TEXT("New value as a string (ImportText_Direct format)."), true);
	Builder.AddString(TEXT("property_path"), TEXT("Optional dot-separated path for nested struct/array access; the final segment is taken from property_name (e.g. property_path='StructType' with property_name='Struct')."), false);
	Builder.AddBoolean(TEXT("reconstruct"), TEXT("Call ReconstructNode() after the write so dynamic pins refresh. Default true."));
	Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
	return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_SetNodeProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	TSharedPtr<FJsonObject> Params;
	FString SessionId;
	FBlueprintEditToolData* Data = nullptr;
	FToolResult Error;
	if (!BeginSessionOp(Arguments, TEXT("set_node_property"), Params, SessionId, Data, Error))
	{
		return Error;
	}
	return CheckMutationAffectedNodes(TEXT("set_node_property"), Data, SetNodeProperty_Impl(SessionId, Data, Params));
}

FToolResult ClaireonBlueprintGraphTool_SetNodeProperty::SetNodeProperty_Impl(
	const FString& SessionId,
	FBlueprintEditToolData* Data,
	const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();

	if (!Blueprint || !Graph)
	{
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));
	}

	FString NodeGuidStr, PropertyName, PropertyValue;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
	{
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	}
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		return MakeErrorResult(TEXT("Missing required field: property_name"));
	}
	if (!Params->TryGetStringField(TEXT("property_value"), PropertyValue))
	{
		return MakeErrorResult(TEXT("Missing required field: property_value"));
	}

	FString PropertyPath;
	Params->TryGetStringField(TEXT("property_path"), PropertyPath);

	bool bReconstruct = true;
	Params->TryGetBoolField(TEXT("reconstruct"), bReconstruct);

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));
	}

	UEdGraphNode* Node = ClaireonBPGraphInternal::FindNodeForOperation(Graph, NodeGuid, Data);
	if (!Node)
	{
		const FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s in graph '%s'.\n%s"),
			*NodeGuidStr, *Graph->GetName(), *AvailableNodes));
	}

	const FString CombinedPath = PropertyPath.IsEmpty()
		? PropertyName
		: PropertyPath + TEXT(".") + PropertyName;

	// Peek the leaf to capture old value for the response and to fail fast if the path is wrong.
	FString OldValue;
	{
		FString ReadError;
		OldValue = ClaireonPropertyUtils::ReadPropertyByPath(Node, CombinedPath, ReadError);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Blueprint Node Property")));
	Blueprint->Modify();
	Graph->Modify();
	Node->Modify();

	FString WriteError;
	if (!ClaireonPropertyUtils::WritePropertyByPath(Node, CombinedPath, PropertyValue, WriteError))
	{
		return MakeErrorResult(WriteError);
	}

	if (bReconstruct)
	{
		Node->ReconstructNode();
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	const FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Set [%s].%s = '%s' (was '%s')"),
		*NodeTitle, *CombinedPath, *PropertyValue, *OldValue);

	Data->LastOperationAffectedNodes.Add(Node->NodeGuid);

	return BuildStateResponse(SessionId, Data);
}

FString ClaireonBlueprintGraphTool_SetNodeProperty::GetFullDescription() const
{
	return TEXT(
		"Sets a UPROPERTY on a K2 node in the current session's graph. Targets node "
		"fields whose values drive pin layout -- protected fields the Python "
		"set_editor_property surface refuses to touch and that "
		"blueprint_graph_set_property does not reach because that tool writes the "
		"Blueprint CDO / component templates, not graph nodes. Typical callers: "
		"K2Node_DynamicCast.TargetType, K2Node_SwitchEnum.Enum, "
		"K2Node_MakeStruct.StructType, K2Node_BaseAsyncTask.ProxyFactoryClass / "
		"ProxyFactoryFunctionName. Values pass through "
		"ClaireonPropertyUtils::WritePropertyByPath, which uses ImportText_Direct "
		"and supports dot-path navigation plus [N] array indexing through "
		"property_path. After the write the tool calls ReconstructNode() so "
		"dynamic delegate pins and proxy parameter pins materialize; pass "
		"reconstruct=false to suppress when the node is in a state where reconstruction "
		"would clobber pending edits.");
}

FString ClaireonBlueprintGraphTool_SetNodeProperty::GetExampleUsage() const
{
	return TEXT(
		"claireon.blueprint_graph_set_node_property session_id=\"...\" "
		"node_guid=\"<GUID>\" property_name=\"ProxyFactoryClass\" "
		"property_value=\"/Script/AbilitySystemBlueprintLibrary.AbilitySystemBlueprintLibrary\"");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_SetNodeProperty::GetParameterTooltips() const
{
	TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
	T->SetStringField(TEXT("session_id"), TEXT("Session ID returned by claireon.blueprint_graph_open or _create."));
	T->SetStringField(TEXT("node_guid"), TEXT("Node GUID (UEdGraphNode::NodeGuid)."));
	T->SetStringField(TEXT("property_name"), TEXT("UPROPERTY name on the node class (e.g. 'ProxyFactoryClass')."));
	T->SetStringField(TEXT("property_value"), TEXT("ImportText-formatted value. Class refs: '/Script/Module.ClassName'. Function refs / member names: bare string."));
	T->SetStringField(TEXT("property_path"), TEXT("Optional dot path for nested access. Final segment comes from property_name."));
	T->SetStringField(TEXT("reconstruct"), TEXT("Whether to call ReconstructNode() so dynamic pins refresh. Default true."));
	return T;
}

#undef LOCTEXT_NAMESPACE
