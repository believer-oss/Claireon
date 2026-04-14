// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_BehaviorTreeEdit.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonNameResolver.h"
#include "ClaireonLog.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSafeExec.h"
#include "ClaireonSessionManager.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/Tasks/BTTask_RunBehavior.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "AIGraphNode.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "FileHelpers.h"
#include "Misc/Guid.h"

using FToolResult = IClaireonTool::FToolResult;

// Static tool data storage
TMap<FString, FBehaviorTreeEditToolData> ClaireonTool_BehaviorTreeEdit::ToolData;
bool ClaireonTool_BehaviorTreeEdit::bDelegateRegistered = false;

// ============================================================================
// Helper: Parse GUID from JSON string
// ============================================================================

namespace
{
	bool ParseGuidParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, FGuid& OutGuid, FString& OutError)
	{
		FString GuidStr;
		if (!Params->TryGetStringField(FieldName, GuidStr) || GuidStr.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Missing required parameter: %s"), *FieldName);
			return false;
		}
		if (!FGuid::Parse(GuidStr, OutGuid))
		{
			OutError = FString::Printf(TEXT("Invalid GUID for %s: %s"), *FieldName, *GuidStr);
			return false;
		}
		return true;
	}
} // namespace

// ============================================================================
// Session Management
// ============================================================================

void ClaireonTool_BehaviorTreeEdit::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == TEXT("claireon.behaviortree_edit"))
	{
		ToolData.Remove(Info.SessionId);
	}
}

// ============================================================================
// Tool Interface
// ============================================================================

FString ClaireonTool_BehaviorTreeEdit::GetName() const
{
	return TEXT("claireon.behaviortree_edit");
}

FString ClaireonTool_BehaviorTreeEdit::GetDescription() const
{
	return TEXT("Session-based Behavior Tree editor. Manage composites, tasks, decorators, services, and subtrees. Start with 'open', then 'update_asset' and 'save'.");
}

FString ClaireonTool_BehaviorTreeEdit::GetFullDescription() const
{
	return TEXT("Session-based Behavior Tree editor. Supports full node manipulation including "
				"composites, tasks, decorators, services, property editing, and asset update/save.\n\n"
				"Session operations: open, close, status\n"
				"Node operations: add_node, remove_node, move_node, set_node_property\n"
				"Decorator/Service operations: add_decorator, remove_decorator, add_service, remove_service\n"
				"Subtree operations: set_subtree_asset\n"
				"Build operations: update_asset, save\n"
				"Discovery: list_node_types");
}

TSharedPtr<FJsonObject> ClaireonTool_BehaviorTreeEdit::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// operation - required
	TSharedPtr<FJsonObject> OpProp = MakeShared<FJsonObject>();
	OpProp->SetStringField(TEXT("type"), TEXT("string"));
	OpProp->SetStringField(TEXT("description"), TEXT("The editing operation to perform."));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("open")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("close")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("status")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("add_node")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("remove_node")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("move_node")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_node_property")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("add_decorator")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("remove_decorator")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("add_service")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("remove_service")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_subtree_asset")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("update_asset")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("save")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("list_node_types")));
		OpProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("operation"), OpProp);

	// session_id
	TSharedPtr<FJsonObject> SessionProp = MakeShared<FJsonObject>();
	SessionProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionProp->SetStringField(TEXT("description"), TEXT("Session identifier from a previous 'open' operation. Required for all operations except 'open' and 'list_node_types'."));
	Properties->SetObjectField(TEXT("session_id"), SessionProp);

	// params
	TSharedPtr<FJsonObject> ParamsProp = MakeShared<FJsonObject>();
	ParamsProp->SetStringField(TEXT("type"), TEXT("object"));
	ParamsProp->SetStringField(TEXT("description"), TEXT("Operation-specific parameters."));
	Properties->SetObjectField(TEXT("params"), ParamsProp);

	// suppress_output
	TSharedPtr<FJsonObject> SuppressOutputProp = MakeShared<FJsonObject>();
	SuppressOutputProp->SetStringField(TEXT("type"), TEXT("boolean"));
	SuppressOutputProp->SetStringField(TEXT("description"),
		TEXT("When true, returns only a brief status instead of the full tree state. "
			 "Use for intermediate operations in a batch to reduce response size."));
	Properties->SetObjectField(TEXT("suppress_output"), SuppressOutputProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("operation")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonTool_BehaviorTreeEdit::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Operation;
	if (!Arguments->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: operation"));
	}

	// Params sub-object (optional)
	TSharedPtr<FJsonObject> Params;
	const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
	if (Arguments->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr)
	{
		Params = *ParamsPtr;
	}
	else
	{
		Params = Arguments;
	}

	bool bSuppressOutput = false;
	if (Arguments->HasField(TEXT("suppress_output")))
	{
		bSuppressOutput = Arguments->GetBoolField(TEXT("suppress_output"));
	}

	// Operations that don't need a session
	if (Operation == TEXT("open"))
		return Operation_Open(Params);
	if (Operation == TEXT("list_node_types"))
		return Operation_ListNodeTypes(Params);

	// All other operations require session_id
	FString SessionId;
	if (!Arguments->TryGetStringField(TEXT("session_id"), SessionId) || SessionId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: session_id"));
	}

	FMCPSession* Session = FClaireonSessionManager::Get().FindSession(SessionId);
	if (!Session)
	{
		return MakeErrorResult(FString::Printf(TEXT("Session not found or expired: %s"), *SessionId));
	}

	FBehaviorTreeEditToolData* Data = ToolData.Find(SessionId);
	if (!Data)
	{
		return MakeErrorResult(TEXT("Session tool data not found"));
	}

	Data->bSuppressOutput = bSuppressOutput;

	if (Operation == TEXT("close"))
		return Operation_Close(SessionId, Data, Params);
	if (Operation == TEXT("status"))
		return Operation_Status(SessionId, Data, Params);
	if (Operation == TEXT("add_node"))
		return Operation_AddNode(SessionId, Data, Params);
	if (Operation == TEXT("remove_node"))
		return Operation_RemoveNode(SessionId, Data, Params);
	if (Operation == TEXT("move_node"))
		return Operation_MoveNode(SessionId, Data, Params);
	if (Operation == TEXT("set_node_property"))
		return Operation_SetNodeProperty(SessionId, Data, Params);
	if (Operation == TEXT("add_decorator"))
		return Operation_AddDecorator(SessionId, Data, Params);
	if (Operation == TEXT("remove_decorator"))
		return Operation_RemoveDecorator(SessionId, Data, Params);
	if (Operation == TEXT("add_service"))
		return Operation_AddService(SessionId, Data, Params);
	if (Operation == TEXT("remove_service"))
		return Operation_RemoveService(SessionId, Data, Params);
	if (Operation == TEXT("set_subtree_asset"))
		return Operation_SetSubtreeAsset(SessionId, Data, Params);
	if (Operation == TEXT("update_asset"))
		return Operation_UpdateAsset(SessionId, Data, Params);
	if (Operation == TEXT("save"))
		return Operation_Save(SessionId, Data, Params);

	return MakeErrorResult(FString::Printf(TEXT("Unknown operation: %s"), *Operation));
}

// ============================================================================
// Response Building
// ============================================================================

FToolResult ClaireonTool_BehaviorTreeEdit::BuildStateResponse(const FString& SessionId, FBehaviorTreeEditToolData* Data)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	if (Data->bSuppressOutput)
	{
		const FString StatusMsg = Data->LastOperationStatus.IsEmpty()
			? TEXT("ok")
			: FString::Printf(TEXT("ok: %s"), *Data->LastOperationStatus);
		TSharedPtr<FJsonObject> SuppressData = MakeShared<FJsonObject>();
		SuppressData->SetStringField(TEXT("session_id"), SessionId);
		SuppressData->SetStringField(TEXT("status"), StatusMsg);
		return MakeSuccessResult(SuppressData, StatusMsg);
	}

	FString Error;
	UBehaviorTreeGraph* Graph = ClaireonBehaviorTreeHelpers::GetBTGraph(Data->BehaviorTree.Get(), Error);
	if (!Graph)
	{
		return MakeErrorResult(FString::Printf(TEXT("Session error: %s"), *Error));
	}

	FString Output;
	Output += TEXT("=== Session Status ===\n");
	Output += FString::Printf(TEXT("Session: %s\n"), *SessionId);
	Output += FString::Printf(TEXT("Asset: %s\n"), *Data->BehaviorTree->GetPathName());
	Output += FString::Printf(TEXT("Last Operation: %s\n"), *Data->LastOperationStatus);
	Output += TEXT("\n");
	Output += ClaireonBehaviorTreeHelpers::FormatBTGraphStructure(Graph, false);

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("asset_path"), Data->BehaviorTree->GetPathName());
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetStringField(TEXT("last_operation"), Data->LastOperationStatus);
	ResponseData->SetStringField(TEXT("tree_view"), Output);

	const FString Summary = FString::Printf(TEXT("Session %s: %s"),
		*SessionId.Left(8), *Data->LastOperationStatus);

	return MakeSuccessResult(ResponseData, Summary);
}

// ============================================================================
// Session Operations
// ============================================================================

FToolResult ClaireonTool_BehaviorTreeEdit::Operation_Open(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'open' requires params.asset_path"));
	}

	// Resolve path to canonical form
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	FString Error;
	UBehaviorTree* BT = ClaireonBehaviorTreeHelpers::LoadBehaviorTreeAsset(AssetPath, Error);
	if (!BT)
	{
		return MakeErrorResult(Error);
	}

	UBehaviorTreeGraph* BTGraph = ClaireonBehaviorTreeHelpers::GetBTGraph(BT, Error);
	if (!BTGraph)
	{
		return MakeErrorResult(Error);
	}

	// Register delegate if not done yet
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonTool_BehaviorTreeEdit::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	const FString ResolvedAssetPath = BT->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, TEXT("claireon.behaviortree_edit"));
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		const FTimespan Elapsed = FDateTime::UtcNow() - Blocker.LastAccessTime;
		return MakeErrorResult(FString::Printf(
			TEXT("Asset is locked by %s session %s (last activity %dm %ds ago). Close that session first."),
			*Blocker.ToolName, *Blocker.SessionId,
			static_cast<int32>(Elapsed.GetTotalMinutes()),
			static_cast<int32>(Elapsed.GetTotalSeconds()) % 60));
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *ResolvedAssetPath));
	}
	const FString SessionId = OpenResult.SessionId;

	// If ReusedExistingSession, still update tool data
	FBehaviorTreeEditToolData NewData;
	NewData.BehaviorTree = BT;
	NewData.BTGraph = BTGraph;
	NewData.LastOperationStatus = TEXT("Session opened");
	ToolData.Add(SessionId, MoveTemp(NewData));

	FString StructureText = ClaireonBehaviorTreeHelpers::FormatBTGraphStructure(BTGraph, false);

	TSharedPtr<FJsonObject> OpenData = MakeShared<FJsonObject>();
	OpenData->SetStringField(TEXT("session_id"), SessionId);
	OpenData->SetStringField(TEXT("asset_path"), AssetPath);
	OpenData->SetStringField(TEXT("status"), TEXT("Session opened"));
	OpenData->SetStringField(TEXT("structure"), StructureText);

	return MakeSuccessResult(OpenData, FString::Printf(TEXT("Opened session for %s"), *FPaths::GetBaseFilename(AssetPath)));
}

FToolResult ClaireonTool_BehaviorTreeEdit::Operation_Close(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	bool bUpdateFirst = false;
	bool bSaveFirst = false;
	Params->TryGetBoolField(TEXT("update_first"), bUpdateFirst);
	Params->TryGetBoolField(TEXT("save_first"), bSaveFirst);

	if (bUpdateFirst)
	{
		Operation_UpdateAsset(SessionId, Data, MakeShared<FJsonObject>());
	}
	if (bSaveFirst)
	{
		Operation_Save(SessionId, Data, MakeShared<FJsonObject>());
	}

	FClaireonSessionManager::Get().CloseSession(SessionId);
	ToolData.Remove(SessionId);

	TSharedPtr<FJsonObject> CloseData = MakeShared<FJsonObject>();
	CloseData->SetStringField(TEXT("session_id"), SessionId);
	CloseData->SetStringField(TEXT("status"), TEXT("closed"));
	return MakeSuccessResult(CloseData, FString::Printf(TEXT("Session closed: %s"), *SessionId));
}

FToolResult ClaireonTool_BehaviorTreeEdit::Operation_Status(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Node Operations
// ============================================================================

FToolResult ClaireonTool_BehaviorTreeEdit::Operation_AddNode(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UBehaviorTreeGraph* Graph = ClaireonBehaviorTreeHelpers::GetBTGraph(Data->BehaviorTree.Get(), Error);
	if (!Graph)
	{
		return MakeErrorResult(Error);
	}

	// parent_node_id: GUID of the parent node (or root node)
	FGuid ParentGuid;
	if (!ParseGuidParam(Params, TEXT("parent_node_id"), ParentGuid, Error))
	{
		return MakeErrorResult(Error);
	}

	FString NodeClassName;
	if (!Params->TryGetStringField(TEXT("node_class"), NodeClassName) || NodeClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: node_class"));
	}

	int32 ChildIndex = -1;
	Params->TryGetNumberField(TEXT("child_index"), ChildIndex);

	// Resolve the class
	ClaireonNameResolver::FNameResolveResult NameResult;
	UClass* NodeClass = ClaireonNameResolver::ResolveClassName(NodeClassName, UBTNode::StaticClass(), NameResult);
	if (!NodeClass)
	{
		return MakeErrorResult(NameResult.Error);
	}

	// Verify it's a composite or task
	if (!NodeClass->IsChildOf(UBTCompositeNode::StaticClass()) && !NodeClass->IsChildOf(UBTTaskNode::StaticClass()))
	{
		return MakeErrorResult(FString::Printf(TEXT("Class '%s' is not a composite or task node. Use add_decorator or add_service for those types."), *NodeClassName));
	}

	// Find parent graph node
	UBehaviorTreeGraphNode* ParentGraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, ParentGuid);
	if (!ParentGraphNode)
	{
		// Check if it's the root node
		UBehaviorTreeGraphNode_Root* RootNode = ClaireonBehaviorTreeHelpers::FindRootGraphNode(Graph);
		if (RootNode && RootNode->NodeGuid == ParentGuid)
		{
			ParentGraphNode = RootNode;
		}
		else
		{
			return MakeErrorResult(FString::Printf(TEXT("Parent node not found with GUID: %s"), *ParentGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add BT Node")));
	Data->BehaviorTree->Modify();

	// Create the new graph node
	UBehaviorTreeGraphNode* NewNode = ClaireonBehaviorTreeHelpers::CreateGraphNodeForClass(Graph, NodeClass,
		FVector2D(ParentGraphNode->NodePosX + 200, ParentGraphNode->NodePosY + 100), Error);
	if (!NewNode)
	{
		return MakeErrorResult(Error);
	}

	// Connect to parent
	if (!ClaireonBehaviorTreeHelpers::ConnectNodes(ParentGraphNode, NewNode, ChildIndex, Error))
	{
		return MakeErrorResult(FString::Printf(TEXT("Node created but failed to connect: %s"), *Error));
	}

	Data->FocusedNodeGuid = NewNode->NodeGuid;
	Data->LastOperationStatus = FString::Printf(TEXT("add_node â Added %s under parent {%s}"),
		*NodeClassName, *ParentGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	if (!NameResult.ResolutionNote.IsEmpty())
	{
		Data->LastOperationStatus += FString::Printf(TEXT(" [note: %s]"), *NameResult.ResolutionNote);
	}

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_BehaviorTreeEdit::Operation_RemoveNode(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UBehaviorTreeGraph* Graph = ClaireonBehaviorTreeHelpers::GetBTGraph(Data->BehaviorTree.Get(), Error);
	if (!Graph)
	{
		return MakeErrorResult(Error);
	}

	FGuid NodeGuid;
	if (!ParseGuidParam(Params, TEXT("node_id"), NodeGuid, Error))
	{
		return MakeErrorResult(Error);
	}

	UBehaviorTreeGraphNode* GraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, NodeGuid);
	if (!GraphNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s"), *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	// Prevent removing root node
	if (Cast<UBehaviorTreeGraphNode_Root>(GraphNode))
	{
		return MakeErrorResult(TEXT("Cannot remove the root node"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove BT Node")));
	Data->BehaviorTree->Modify();

	// Disconnect from parent
	ClaireonBehaviorTreeHelpers::DisconnectNode(GraphNode, Error);

	// Break all output connections too
	for (UEdGraphPin* Pin : GraphNode->Pins)
	{
		if (Pin)
		{
			Pin->BreakAllPinLinks();
		}
	}

	// Remove from graph
	Graph->RemoveNode(GraphNode);

	Data->FocusedNodeGuid = FGuid();
	Data->LastOperationStatus = FString::Printf(TEXT("remove_node â Removed {%s}"),
		*NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_BehaviorTreeEdit::Operation_MoveNode(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UBehaviorTreeGraph* Graph = ClaireonBehaviorTreeHelpers::GetBTGraph(Data->BehaviorTree.Get(), Error);
	if (!Graph)
	{
		return MakeErrorResult(Error);
	}

	FGuid NodeGuid, NewParentGuid;
	if (!ParseGuidParam(Params, TEXT("node_id"), NodeGuid, Error))
	{
		return MakeErrorResult(Error);
	}
	if (!ParseGuidParam(Params, TEXT("new_parent_id"), NewParentGuid, Error))
	{
		return MakeErrorResult(Error);
	}

	int32 ChildIndex = -1;
	Params->TryGetNumberField(TEXT("child_index"), ChildIndex);

	UBehaviorTreeGraphNode* GraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, NodeGuid);
	if (!GraphNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	UBehaviorTreeGraphNode* NewParentNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, NewParentGuid);
	if (!NewParentNode)
	{
		UBehaviorTreeGraphNode_Root* RootNode = ClaireonBehaviorTreeHelpers::FindRootGraphNode(Graph);
		if (RootNode && RootNode->NodeGuid == NewParentGuid)
		{
			NewParentNode = RootNode;
		}
		else
		{
			return MakeErrorResult(FString::Printf(TEXT("New parent node not found: %s"), *NewParentGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Move BT Node")));
	Data->BehaviorTree->Modify();

	// Disconnect from old parent
	ClaireonBehaviorTreeHelpers::DisconnectNode(GraphNode, Error);

	// Connect to new parent
	if (!ClaireonBehaviorTreeHelpers::ConnectNodes(NewParentNode, GraphNode, ChildIndex, Error))
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to connect to new parent: %s"), *Error));
	}

	Data->FocusedNodeGuid = NodeGuid;
	Data->LastOperationStatus = FString::Printf(TEXT("move_node â Moved {%s} under {%s}"),
		*NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower),
		*NewParentGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_BehaviorTreeEdit::Operation_SetNodeProperty(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UBehaviorTreeGraph* Graph = ClaireonBehaviorTreeHelpers::GetBTGraph(Data->BehaviorTree.Get(), Error);
	if (!Graph)
	{
		return MakeErrorResult(Error);
	}

	FGuid NodeGuid;
	if (!ParseGuidParam(Params, TEXT("node_id"), NodeGuid, Error))
	{
		return MakeErrorResult(Error);
	}

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_name"));
	}

	FString PropertyValue;
	if (!Params->TryGetStringField(TEXT("property_value"), PropertyValue))
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_value"));
	}

	UBehaviorTreeGraphNode* GraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, NodeGuid);
	if (!GraphNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	UBTNode* NodeInstance = Cast<UBTNode>(GraphNode->NodeInstance);
	if (!NodeInstance)
	{
		return MakeErrorResult(TEXT("Node has no BT node instance"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set BT Node Property")));
	Data->BehaviorTree->Modify();

	if (!ClaireonBehaviorTreeHelpers::SetBTNodeProperty(NodeInstance, PropertyName, PropertyValue, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->FocusedNodeGuid = NodeGuid;
	Data->LastOperationStatus = FString::Printf(TEXT("set_node_property â Set '%s' = '%s' on {%s}"),
		*PropertyName, *PropertyValue, *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Decorator/Service Operations
// ============================================================================

FToolResult ClaireonTool_BehaviorTreeEdit::Operation_AddDecorator(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UBehaviorTreeGraph* Graph = ClaireonBehaviorTreeHelpers::GetBTGraph(Data->BehaviorTree.Get(), Error);
	if (!Graph)
	{
		return MakeErrorResult(Error);
	}

	FGuid NodeGuid;
	if (!ParseGuidParam(Params, TEXT("node_id"), NodeGuid, Error))
	{
		return MakeErrorResult(Error);
	}

	FString DecoratorClassName;
	if (!Params->TryGetStringField(TEXT("decorator_class"), DecoratorClassName) || DecoratorClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: decorator_class"));
	}

	UBehaviorTreeGraphNode* ParentGraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, NodeGuid);
	if (!ParentGraphNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	ClaireonNameResolver::FNameResolveResult DecoratorNameResult;
	UClass* DecoratorClass = ClaireonNameResolver::ResolveClassName(DecoratorClassName, UBTDecorator::StaticClass(), DecoratorNameResult);
	if (!DecoratorClass)
	{
		return MakeErrorResult(DecoratorNameResult.Error);
	}

	if (!DecoratorClass->IsChildOf(UBTDecorator::StaticClass()))
	{
		return MakeErrorResult(FString::Printf(TEXT("Class '%s' is not a BTDecorator subclass"), *DecoratorClassName));
	}

	// Create decorator graph node â AddSubNode creates its own transaction internally
	UBehaviorTreeGraphNode* DecoratorGraphNode = ClaireonBehaviorTreeHelpers::CreateGraphNodeForClass(Graph, DecoratorClass,
		FVector2D(ParentGraphNode->NodePosX, ParentGraphNode->NodePosY - 50), Error);
	if (!DecoratorGraphNode)
	{
		return MakeErrorResult(Error);
	}

	// AddSubNode manages its own FScopedTransaction
	ParentGraphNode->AddSubNode(DecoratorGraphNode, Graph);

	Data->FocusedNodeGuid = DecoratorGraphNode->NodeGuid;
	Data->LastOperationStatus = FString::Printf(TEXT("add_decorator â Added %s to {%s}"),
		*DecoratorClassName, *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	if (!DecoratorNameResult.ResolutionNote.IsEmpty())
	{
		Data->LastOperationStatus += FString::Printf(TEXT(" [note: %s]"), *DecoratorNameResult.ResolutionNote);
	}

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_BehaviorTreeEdit::Operation_RemoveDecorator(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UBehaviorTreeGraph* Graph = ClaireonBehaviorTreeHelpers::GetBTGraph(Data->BehaviorTree.Get(), Error);
	if (!Graph)
	{
		return MakeErrorResult(Error);
	}

	FGuid NodeGuid;
	if (!ParseGuidParam(Params, TEXT("node_id"), NodeGuid, Error))
	{
		return MakeErrorResult(Error);
	}

	FGuid DecoratorGuid;
	if (!ParseGuidParam(Params, TEXT("decorator_id"), DecoratorGuid, Error))
	{
		return MakeErrorResult(Error);
	}

	UBehaviorTreeGraphNode* ParentGraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, NodeGuid);
	if (!ParentGraphNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	// Find the decorator sub-node by GUID
	UAIGraphNode* DecoratorSubNode = nullptr;
	for (UAIGraphNode* SubNode : ParentGraphNode->SubNodes)
	{
		UBehaviorTreeGraphNode* SubBTNode = Cast<UBehaviorTreeGraphNode>(SubNode);
		if (SubBTNode && SubBTNode->NodeGuid == DecoratorGuid)
		{
			DecoratorSubNode = SubNode;
			break;
		}
	}

	if (!DecoratorSubNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Decorator not found with GUID: %s"), *DecoratorGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	// RemoveSubNode manages its own FScopedTransaction
	ParentGraphNode->RemoveSubNode(DecoratorSubNode);

	Data->FocusedNodeGuid = NodeGuid;
	Data->LastOperationStatus = FString::Printf(TEXT("remove_decorator â Removed {%s} from {%s}"),
		*DecoratorGuid.ToString(EGuidFormats::DigitsWithHyphensLower),
		*NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_BehaviorTreeEdit::Operation_AddService(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UBehaviorTreeGraph* Graph = ClaireonBehaviorTreeHelpers::GetBTGraph(Data->BehaviorTree.Get(), Error);
	if (!Graph)
	{
		return MakeErrorResult(Error);
	}

	FGuid NodeGuid;
	if (!ParseGuidParam(Params, TEXT("node_id"), NodeGuid, Error))
	{
		return MakeErrorResult(Error);
	}

	FString ServiceClassName;
	if (!Params->TryGetStringField(TEXT("service_class"), ServiceClassName) || ServiceClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: service_class"));
	}

	UBehaviorTreeGraphNode* ParentGraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, NodeGuid);
	if (!ParentGraphNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	ClaireonNameResolver::FNameResolveResult ServiceNameResult;
	UClass* ServiceClass = ClaireonNameResolver::ResolveClassName(ServiceClassName, UBTService::StaticClass(), ServiceNameResult);
	if (!ServiceClass)
	{
		return MakeErrorResult(ServiceNameResult.Error);
	}

	if (!ServiceClass->IsChildOf(UBTService::StaticClass()))
	{
		return MakeErrorResult(FString::Printf(TEXT("Class '%s' is not a BTService subclass"), *ServiceClassName));
	}

	UBehaviorTreeGraphNode* ServiceGraphNode = ClaireonBehaviorTreeHelpers::CreateGraphNodeForClass(Graph, ServiceClass,
		FVector2D(ParentGraphNode->NodePosX, ParentGraphNode->NodePosY + 50), Error);
	if (!ServiceGraphNode)
	{
		return MakeErrorResult(Error);
	}

	// AddSubNode manages its own FScopedTransaction
	ParentGraphNode->AddSubNode(ServiceGraphNode, Graph);

	Data->FocusedNodeGuid = ServiceGraphNode->NodeGuid;
	Data->LastOperationStatus = FString::Printf(TEXT("add_service â Added %s to {%s}"),
		*ServiceClassName, *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	if (!ServiceNameResult.ResolutionNote.IsEmpty())
	{
		Data->LastOperationStatus += FString::Printf(TEXT(" [note: %s]"), *ServiceNameResult.ResolutionNote);
	}

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_BehaviorTreeEdit::Operation_RemoveService(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UBehaviorTreeGraph* Graph = ClaireonBehaviorTreeHelpers::GetBTGraph(Data->BehaviorTree.Get(), Error);
	if (!Graph)
	{
		return MakeErrorResult(Error);
	}

	FGuid NodeGuid;
	if (!ParseGuidParam(Params, TEXT("node_id"), NodeGuid, Error))
	{
		return MakeErrorResult(Error);
	}

	FGuid ServiceGuid;
	if (!ParseGuidParam(Params, TEXT("service_id"), ServiceGuid, Error))
	{
		return MakeErrorResult(Error);
	}

	UBehaviorTreeGraphNode* ParentGraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, NodeGuid);
	if (!ParentGraphNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	UAIGraphNode* ServiceSubNode = nullptr;
	for (UAIGraphNode* SubNode : ParentGraphNode->SubNodes)
	{
		UBehaviorTreeGraphNode* SubBTNode = Cast<UBehaviorTreeGraphNode>(SubNode);
		if (SubBTNode && SubBTNode->NodeGuid == ServiceGuid)
		{
			ServiceSubNode = SubNode;
			break;
		}
	}

	if (!ServiceSubNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Service not found with GUID: %s"), *ServiceGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	// RemoveSubNode manages its own FScopedTransaction
	ParentGraphNode->RemoveSubNode(ServiceSubNode);

	Data->FocusedNodeGuid = NodeGuid;
	Data->LastOperationStatus = FString::Printf(TEXT("remove_service â Removed {%s} from {%s}"),
		*ServiceGuid.ToString(EGuidFormats::DigitsWithHyphensLower),
		*NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Subtree Operations
// ============================================================================

FToolResult ClaireonTool_BehaviorTreeEdit::Operation_SetSubtreeAsset(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UBehaviorTreeGraph* Graph = ClaireonBehaviorTreeHelpers::GetBTGraph(Data->BehaviorTree.Get(), Error);
	if (!Graph)
	{
		return MakeErrorResult(Error);
	}

	FGuid NodeGuid;
	if (!ParseGuidParam(Params, TEXT("node_id"), NodeGuid, Error))
	{
		return MakeErrorResult(Error);
	}

	FString SubtreePath;
	if (!Params->TryGetStringField(TEXT("subtree_path"), SubtreePath) || SubtreePath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: subtree_path"));
	}

	UBehaviorTreeGraphNode* GraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, NodeGuid);
	if (!GraphNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	UBTTask_RunBehavior* RunBehaviorNode = Cast<UBTTask_RunBehavior>(GraphNode->NodeInstance);
	if (!RunBehaviorNode)
	{
		return MakeErrorResult(TEXT("Node is not a BTTask_RunBehavior â set_subtree_asset only works on RunBehavior task nodes"));
	}

	UBehaviorTree* SubtreeBT = ClaireonBehaviorTreeHelpers::LoadBehaviorTreeAsset(SubtreePath, Error);
	if (!SubtreeBT)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Subtree Asset")));
	Data->BehaviorTree->Modify();

	// Set the BehaviorAsset property via reflection
	if (!ClaireonBehaviorTreeHelpers::SetBTNodeProperty(RunBehaviorNode, TEXT("BehaviorAsset"), SubtreePath, Error))
	{
		// Fallback: try direct property assignment
		FProperty* BehaviorAssetProp = FindFProperty<FProperty>(RunBehaviorNode->GetClass(), TEXT("BehaviorAsset"));
		if (BehaviorAssetProp)
		{
			void* ValuePtr = BehaviorAssetProp->ContainerPtrToValuePtr<void>(RunBehaviorNode);
			FObjectProperty* ObjProp = CastField<FObjectProperty>(BehaviorAssetProp);
			if (ObjProp)
			{
				ObjProp->SetObjectPropertyValue(ValuePtr, SubtreeBT);
			}
		}
	}

	Data->FocusedNodeGuid = NodeGuid;
	Data->LastOperationStatus = FString::Printf(TEXT("set_subtree_asset â Set subtree to %s on {%s}"),
		*SubtreePath, *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Build Operations
// ============================================================================

FToolResult ClaireonTool_BehaviorTreeEdit::Operation_UpdateAsset(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	FString Error;
	UBehaviorTreeGraph* Graph = ClaireonBehaviorTreeHelpers::GetBTGraph(Data->BehaviorTree.Get(), Error);
	if (!Graph)
	{
		return MakeErrorResult(Error);
	}

	// UpdateAsset rebuilds the runtime BT from the graph (equivalent of compile for BTs)
	Graph->UpdateAsset();

	Data->LastOperationStatus = TEXT("update_asset â Rebuilt runtime BT from graph");
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_BehaviorTreeEdit::Operation_Save(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	UBehaviorTree* BT = Data->BehaviorTree.Get();
	UPackage* Package = BT->GetPackage();
	Package->SetDirtyFlag(true);

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		return MakeErrorResult(TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor."));
	}
	bool bSuccess = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);

	if (bSuccess)
	{
		Data->LastOperationStatus = FString::Printf(TEXT("save â Saved %s"), *BT->GetPathName());
		return BuildStateResponse(SessionId, Data);
	}
	else
	{
		Data->LastOperationStatus = TEXT("save â Failed");
		return MakeErrorResult(TEXT("Failed to save Behavior Tree package"));
	}
}

// ============================================================================
// Discovery
// ============================================================================

FToolResult ClaireonTool_BehaviorTreeEdit::Operation_ListNodeTypes(const TSharedPtr<FJsonObject>& Params)
{
	FString Category;
	Params->TryGetStringField(TEXT("category"), Category);

	FString Output;

	auto ListSubclasses = [&Output](UClass* BaseClass, const FString& Label)
	{
		Output += FString::Printf(TEXT("=== %s ===\n"), *Label);
		TArray<FString> ClassNames;

		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (Class->IsChildOf(BaseClass) && !Class->HasAnyClassFlags(CLASS_Abstract) && Class != BaseClass)
			{
				ClassNames.Add(Class->GetName());
			}
		}

		ClassNames.Sort();
		for (const FString& Name : ClassNames)
		{
			Output += FString::Printf(TEXT("  %s\n"), *Name);
		}
		Output += FString::Printf(TEXT("Total: %d\n\n"), ClassNames.Num());
	};

	if (Category.IsEmpty() || Category == TEXT("composite"))
	{
		ListSubclasses(UBTCompositeNode::StaticClass(), TEXT("Composite Nodes"));
	}
	if (Category.IsEmpty() || Category == TEXT("task"))
	{
		ListSubclasses(UBTTaskNode::StaticClass(), TEXT("Task Nodes"));
	}
	if (Category.IsEmpty() || Category == TEXT("decorator"))
	{
		ListSubclasses(UBTDecorator::StaticClass(), TEXT("Decorators"));
	}
	if (Category.IsEmpty() || Category == TEXT("service"))
	{
		ListSubclasses(UBTService::StaticClass(), TEXT("Services"));
	}

	if (Output.IsEmpty())
	{
		return MakeErrorResult(FString::Printf(TEXT("Unknown category: %s. Use 'composite', 'task', 'decorator', or 'service'."), *Category));
	}

	TSharedPtr<FJsonObject> NodeTypesData = MakeShared<FJsonObject>();
	NodeTypesData->SetStringField(TEXT("node_types"), Output);
	NodeTypesData->SetStringField(TEXT("category"), Category.IsEmpty() ? TEXT("all") : Category);
	return MakeSuccessResult(NodeTypesData, FString::Printf(TEXT("Node types for category: %s"), Category.IsEmpty() ? TEXT("all") : *Category));
}
