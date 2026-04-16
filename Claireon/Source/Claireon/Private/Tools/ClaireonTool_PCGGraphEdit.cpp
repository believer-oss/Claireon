// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PCGGraphEdit.h"
#include "Tools/ClaireonSpecApplicator_PCGGraph.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "ClaireonLog.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSafeExec.h"
#include "ClaireonSessionManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGEdge.h"
#include "PCGSettings.h"
#include "PCGCommon.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "FileHelpers.h"
#include "Editor.h"

// Using statements
using FToolResult = IClaireonTool::FToolResult;

// Static tool data storage
TMap<FString, FPCGGraphEditToolData> ClaireonTool_PCGGraphEdit::ToolData;
bool ClaireonTool_PCGGraphEdit::bDelegateRegistered = false;

// ============================================================================
// Session Management
// ============================================================================

void ClaireonTool_PCGGraphEdit::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == TEXT("claireon.pcg_edit"))
	{
		ToolData.Remove(Info.SessionId);
	}
}

// ============================================================================
// Tool Interface Implementation
// ============================================================================

FString ClaireonTool_PCGGraphEdit::GetName() const
{
	return TEXT("claireon.pcg_edit");
}

FString ClaireonTool_PCGGraphEdit::GetDescription() const
{
	return TEXT("Interactively edit a PCG (Procedural Content Generation) graph using a session-based model. "
				"Start with 'open' to begin a session, then use operations to add/remove nodes, "
				"connect/disconnect edges, and modify node properties. Finish with 'save'.\n\n"
				"Session operations: open, close, get_state\n"
				"Node operations: add_node, remove_node, set_node_property, get_node_properties, list_node_types\n"
				"Edge operations: connect, disconnect, disconnect_all\n"
				"Navigation: focus, cursor_back\n"
				"Lifecycle: save");
}

TSharedPtr<FJsonObject> ClaireonTool_PCGGraphEdit::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> SessionIdProp = MakeShared<FJsonObject>();
	SessionIdProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionIdProp->SetStringField(TEXT("description"), TEXT("Session identifier from a previous 'open' operation. Required for all operations except 'open'."));
	Properties->SetObjectField(TEXT("session_id"), SessionIdProp);

	TSharedPtr<FJsonObject> OperationProp = MakeShared<FJsonObject>();
	OperationProp->SetStringField(TEXT("type"), TEXT("string"));
	TArray<TSharedPtr<FJsonValue>> OperationEnum;
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("open")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("close")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("get_state")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_node")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_node")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("connect")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("disconnect")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("disconnect_all")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("set_node_property")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("get_node_properties")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("list_node_types")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("focus")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("cursor_back")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("save")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("apply_spec")));
	OperationProp->SetArrayField(TEXT("enum"), OperationEnum);
	OperationProp->SetStringField(TEXT("description"), TEXT("The editing operation to perform."));
	Properties->SetObjectField(TEXT("operation"), OperationProp);

	TSharedPtr<FJsonObject> ParamsProp = MakeShared<FJsonObject>();
	ParamsProp->SetStringField(TEXT("type"), TEXT("object"));
	ParamsProp->SetStringField(TEXT("description"),
		TEXT("Operation-specific parameters.\n"
			 "open: { asset_path }\n"
			 "add_node: { settings_class, node_title? }\n"
			 "remove_node: { node (index/name) }\n"
			 "connect: { from_node, from_pin, to_node, to_pin }\n"
			 "disconnect: { from_node, from_pin, to_node, to_pin }\n"
			 "disconnect_all: { node, pin, direction? (input/output) }\n"
			 "set_node_property: { node, property_name, value }\n"
			 "get_node_properties: { node }\n"
			 "list_node_types: { filter? }\n"
			 "focus: { node }\n"
			 "save: {}"));
	Properties->SetObjectField(TEXT("params"), ParamsProp);

	TSharedPtr<FJsonObject> SuppressOutputProp = MakeShared<FJsonObject>();
	SuppressOutputProp->SetStringField(TEXT("type"), TEXT("boolean"));
	SuppressOutputProp->SetStringField(TEXT("description"),
		TEXT("When true, returns only a brief status instead of the full graph state. "
			 "Use for intermediate operations in a batch, then omit on the final operation."));
	Properties->SetObjectField(TEXT("suppress_output"), SuppressOutputProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("operation")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonTool_PCGGraphEdit::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Operation;
	if (!Arguments->TryGetStringField(TEXT("operation"), Operation))
	{
		return MakeErrorResult(TEXT("Missing 'operation' field"));
	}

	TSharedPtr<FJsonObject> Params;
	if (Arguments->HasField(TEXT("params")))
	{
		const TSharedPtr<FJsonObject>* ParamsPtr;
		if (Arguments->TryGetObjectField(TEXT("params"), ParamsPtr))
		{
			Params = *ParamsPtr;
		}
	}
	if (!Params.IsValid())
	{
		Params = MakeShared<FJsonObject>();
	}

	bool bSuppressOutput = false;
	Arguments->TryGetBoolField(TEXT("suppress_output"), bSuppressOutput);

	// Session-less operations
	if (Operation == TEXT("open"))
	{
		return Operation_Open(Params);
	}
	if (Operation == TEXT("apply_spec"))
	{
		return Operation_ApplySpec(Params);
	}

	// Session-required operations
	FString SessionId;
	if (!Arguments->TryGetStringField(TEXT("session_id"), SessionId))
	{
		return MakeErrorResult(TEXT("Missing 'session_id' field (required for all operations except 'open')"));
	}

	FMCPSession* Session = FClaireonSessionManager::Get().FindSession(SessionId);
	if (!Session)
	{
		return MakeErrorResult(FString::Printf(TEXT("Session not found or expired: %s"), *SessionId));
	}

	FPCGGraphEditToolData* Data = ToolData.Find(SessionId);
	if (!Data)
	{
		return MakeErrorResult(TEXT("Session tool data not found"));
	}

	Data->bSuppressOutput = bSuppressOutput;

	if (Operation == TEXT("close"))
		return Operation_Close(SessionId, Data, Params);
	if (Operation == TEXT("get_state"))
		return Operation_GetState(SessionId, Data, Params);
	if (Operation == TEXT("add_node"))
		return Operation_AddNode(SessionId, Data, Params);
	if (Operation == TEXT("remove_node"))
		return Operation_RemoveNode(SessionId, Data, Params);
	if (Operation == TEXT("connect"))
		return Operation_Connect(SessionId, Data, Params);
	if (Operation == TEXT("disconnect"))
		return Operation_Disconnect(SessionId, Data, Params);
	if (Operation == TEXT("disconnect_all"))
		return Operation_DisconnectAll(SessionId, Data, Params);
	if (Operation == TEXT("set_node_property"))
		return Operation_SetNodeProperty(SessionId, Data, Params);
	if (Operation == TEXT("get_node_properties"))
		return Operation_GetNodeProperties(SessionId, Data, Params);
	if (Operation == TEXT("list_node_types"))
		return Operation_ListNodeTypes(SessionId, Data, Params);
	if (Operation == TEXT("focus"))
		return Operation_Focus(SessionId, Data, Params);
	if (Operation == TEXT("cursor_back"))
		return Operation_CursorBack(SessionId, Data, Params);
	if (Operation == TEXT("save"))
		return Operation_Save(SessionId, Data, Params);

	return MakeErrorResult(FString::Printf(TEXT("Unknown operation: %s"), *Operation));
}

// ============================================================================
// Response Building
// ============================================================================

FToolResult ClaireonTool_PCGGraphEdit::BuildStateResponse(const FString& SessionId, FPCGGraphEditToolData* Data)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	// Get asset path from the session manager
	FString AssetPath;
	if (FMCPSession* Session = FClaireonSessionManager::Get().FindSession(SessionId))
	{
		AssetPath = Session->AssetPath;
	}

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetStringField(TEXT("asset_path"), AssetPath);

	if (!Data->LastOperationStatus.IsEmpty())
	{
		ResponseData->SetStringField(TEXT("status"), Data->LastOperationStatus);
	}

	if (!Data->bSuppressOutput)
	{
		FString GraphStructure = ClaireonPCGGraphHelpers::FormatGraphStructure(Data->PCGGraph.Get(), TEXT("summary"));
		ResponseData->SetStringField(TEXT("graph_structure"), GraphStructure);

		if (Data->FocusedNodeIndex != INDEX_NONE)
		{
			const TArray<UPCGNode*>& Nodes = Data->PCGGraph->GetNodes();
			if (Data->FocusedNodeIndex >= 0 && Data->FocusedNodeIndex < Nodes.Num())
			{
				FString FocusedDetail = ClaireonPCGGraphHelpers::FormatNodeDetail(
					Data->PCGGraph.Get(), Nodes[Data->FocusedNodeIndex], Data->FocusedNodeIndex, true);
				ResponseData->SetStringField(TEXT("focused_node"), FocusedDetail);
			}
		}
	}

	FString Summary = Data->LastOperationStatus.IsEmpty()
		? FString::Printf(TEXT("Session %s: %s"), *SessionId, *AssetPath)
		: Data->LastOperationStatus;

	return MakeSuccessResult(ResponseData, Summary);
}

// ============================================================================
// Operations
// ============================================================================

FToolResult ClaireonTool_PCGGraphEdit::Operation_Open(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: params.asset_path"));
	}

	// Resolve path to canonical form
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	FString Error;
	UPCGGraph* Graph = ClaireonPCGGraphHelpers::LoadPCGGraphAsset(AssetPath, Error);
	if (!Graph)
	{
		return MakeErrorResult(Error);
	}

	// Register delegate if not done yet
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonTool_PCGGraphEdit::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	const FString ResolvedAssetPath = Graph->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, TEXT("claireon.pcg_edit"));
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
	FPCGGraphEditToolData NewData;
	NewData.PCGGraph = Graph;
	NewData.LastOperationStatus = TEXT("Session opened");
	NewData.bSuppressOutput = false; // Always show full output on open
	ToolData.Add(SessionId, MoveTemp(NewData));

	FPCGGraphEditToolData* DataPtr = ToolData.Find(SessionId);
	return BuildStateResponse(SessionId, DataPtr);
}

FToolResult ClaireonTool_PCGGraphEdit::Operation_Close(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);

	if (bSave && Data->IsValid())
	{
		UPackage* Package = Data->PCGGraph->GetOutermost();
		if (Package)
		{
			FString PackageFilename;
			if (FPackageName::DoesPackageExist(Package->GetName(), &PackageFilename))
			{
				if (!ClaireonSafeExec::DidLastExecutionCrash())
				{
					FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Save PCG Graph")));
					UEditorLoadingAndSavingUtils::SavePackages({ Package }, false);
				}
			}
		}
	}

	FClaireonSessionManager::Get().CloseSession(SessionId);
	ToolData.Remove(SessionId);

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetBoolField(TEXT("saved"), bSave);

	return MakeSuccessResult(ResponseData, FString::Printf(TEXT("Session %s closed%s"), *SessionId, bSave ? TEXT(" (saved)") : TEXT("")));
}

FToolResult ClaireonTool_PCGGraphEdit::Operation_GetState(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	Data->bSuppressOutput = false; // get_state always returns full output
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_PCGGraphEdit::Operation_AddNode(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString SettingsClassName;
	if (!Params->TryGetStringField(TEXT("settings_class"), SettingsClassName) || SettingsClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: params.settings_class"));
	}

	FString Error;
	UClass* SettingsClass = ClaireonPCGGraphHelpers::ResolveSettingsClass(SettingsClassName, Error);
	if (!SettingsClass)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add PCG Node")));

	UPCGSettings* DefaultSettings = nullptr;
	UPCGNode* NewNode = Data->PCGGraph->AddNodeOfType(TSubclassOf<UPCGSettings>(SettingsClass), DefaultSettings);
	if (!NewNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to add node of type: %s"), *SettingsClassName));
	}

	// Set optional title
	FString NodeTitle;
	if (Params->TryGetStringField(TEXT("node_title"), NodeTitle) && !NodeTitle.IsEmpty())
	{
		NewNode->NodeTitle = *NodeTitle;
	}

	ClaireonPCGGraphHelpers::NotifyGraphChanged(Data->PCGGraph.Get());

	int32 NewIndex = Data->PCGGraph->GetNodes().IndexOfByKey(NewNode);
	Data->LastOperationStatus = FString::Printf(TEXT("Added node [%d] %s (%s)"),
		NewIndex, *ClaireonPCGGraphHelpers::GetNodeDisplayName(NewNode), *SettingsClass->GetName());

	// Focus on the new node
	Data->PushHistory();
	Data->FocusedNodeIndex = NewIndex;

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_PCGGraphEdit::Operation_RemoveNode(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString NodeIdentifier;
	if (!Params->TryGetStringField(TEXT("node"), NodeIdentifier) || NodeIdentifier.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: params.node"));
	}

	int32 NodeIndex;
	UPCGNode* Node = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Data->PCGGraph.Get(), NodeIdentifier, NodeIndex);
	if (!Node)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeIdentifier));
	}

	// Don't allow removing input/output nodes
	if (Node == Data->PCGGraph->GetInputNode() || Node == Data->PCGGraph->GetOutputNode())
	{
		return MakeErrorResult(TEXT("Cannot remove the graph's built-in Input or Output node"));
	}

	FString RemovedName = ClaireonPCGGraphHelpers::GetNodeDisplayName(Node);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove PCG Node")));
	Data->PCGGraph->RemoveNode(Node);
	ClaireonPCGGraphHelpers::NotifyGraphChanged(Data->PCGGraph.Get());

	Data->LastOperationStatus = FString::Printf(TEXT("Removed node: %s"), *RemovedName);

	// Reset cursor if it pointed to the removed node
	if (Data->FocusedNodeIndex == NodeIndex)
	{
		Data->FocusedNodeIndex = INDEX_NONE;
	}
	else if (Data->FocusedNodeIndex > NodeIndex && Data->FocusedNodeIndex != INDEX_NONE)
	{
		Data->FocusedNodeIndex--;
	}

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_PCGGraphEdit::Operation_Connect(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString FromNodeId, FromPinLabel, ToNodeId, ToPinLabel;
	if (!Params->TryGetStringField(TEXT("from_node"), FromNodeId) || FromNodeId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: params.from_node"));
	}
	if (!Params->TryGetStringField(TEXT("from_pin"), FromPinLabel) || FromPinLabel.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: params.from_pin"));
	}
	if (!Params->TryGetStringField(TEXT("to_node"), ToNodeId) || ToNodeId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: params.to_node"));
	}
	if (!Params->TryGetStringField(TEXT("to_pin"), ToPinLabel) || ToPinLabel.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: params.to_pin"));
	}

	int32 FromIndex, ToIndex;
	UPCGNode* FromNode = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Data->PCGGraph.Get(), FromNodeId, FromIndex);
	UPCGNode* ToNode = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Data->PCGGraph.Get(), ToNodeId, ToIndex);

	if (!FromNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Source node not found: %s"), *FromNodeId));
	}
	if (!ToNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Target node not found: %s"), *ToNodeId));
	}

	// Verify pins exist
	UPCGPin* FromPin = FromNode->GetOutputPin(FName(*FromPinLabel));
	if (!FromPin)
	{
		// List available output pins
		FString Available;
		for (const TObjectPtr<UPCGPin>& Pin : FromNode->GetOutputPins())
		{
			if (Pin)
			{
				if (!Available.IsEmpty())
					Available += TEXT(", ");
				Available += Pin->Properties.Label.ToString();
			}
		}
		return MakeErrorResult(FString::Printf(TEXT("Output pin '%s' not found on node %s. Available: %s"),
			*FromPinLabel, *ClaireonPCGGraphHelpers::GetNodeDisplayName(FromNode), *Available));
	}

	UPCGPin* ToPin = ToNode->GetInputPin(FName(*ToPinLabel));
	if (!ToPin)
	{
		FString Available;
		for (const TObjectPtr<UPCGPin>& Pin : ToNode->GetInputPins())
		{
			if (Pin)
			{
				if (!Available.IsEmpty())
					Available += TEXT(", ");
				Available += Pin->Properties.Label.ToString();
			}
		}
		return MakeErrorResult(FString::Printf(TEXT("Input pin '%s' not found on node %s. Available: %s"),
			*ToPinLabel, *ClaireonPCGGraphHelpers::GetNodeDisplayName(ToNode), *Available));
	}

	// Check compatibility
	if (!FromPin->CanConnect(ToPin))
	{
		return MakeErrorResult(FString::Printf(TEXT("Pins are not compatible: %s.%s -> %s.%s"),
			*ClaireonPCGGraphHelpers::GetNodeDisplayName(FromNode), *FromPinLabel,
			*ClaireonPCGGraphHelpers::GetNodeDisplayName(ToNode), *ToPinLabel));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Connect PCG Pins")));
	Data->PCGGraph->AddEdge(FromNode, FName(*FromPinLabel), ToNode, FName(*ToPinLabel));
	ClaireonPCGGraphHelpers::NotifyGraphChanged(Data->PCGGraph.Get());

	Data->LastOperationStatus = FString::Printf(TEXT("Connected %s.\"%s\" -> %s.\"%s\""),
		*ClaireonPCGGraphHelpers::GetNodeDisplayName(FromNode), *FromPinLabel,
		*ClaireonPCGGraphHelpers::GetNodeDisplayName(ToNode), *ToPinLabel);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_PCGGraphEdit::Operation_Disconnect(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString FromNodeId, FromPinLabel, ToNodeId, ToPinLabel;
	if (!Params->TryGetStringField(TEXT("from_node"), FromNodeId) || !Params->TryGetStringField(TEXT("from_pin"), FromPinLabel) || !Params->TryGetStringField(TEXT("to_node"), ToNodeId) || !Params->TryGetStringField(TEXT("to_pin"), ToPinLabel))
	{
		return MakeErrorResult(TEXT("Missing required parameters: from_node, from_pin, to_node, to_pin"));
	}

	int32 FromIndex, ToIndex;
	UPCGNode* FromNode = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Data->PCGGraph.Get(), FromNodeId, FromIndex);
	UPCGNode* ToNode = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Data->PCGGraph.Get(), ToNodeId, ToIndex);

	if (!FromNode || !ToNode)
	{
		return MakeErrorResult(TEXT("Source or target node not found"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Disconnect PCG Pins")));
	bool bRemoved = Data->PCGGraph->RemoveEdge(FromNode, FName(*FromPinLabel), ToNode, FName(*ToPinLabel));

	if (!bRemoved)
	{
		return MakeErrorResult(TEXT("No matching edge found to remove"));
	}

	ClaireonPCGGraphHelpers::NotifyGraphChanged(Data->PCGGraph.Get());

	Data->LastOperationStatus = FString::Printf(TEXT("Disconnected %s.\"%s\" -> %s.\"%s\""),
		*ClaireonPCGGraphHelpers::GetNodeDisplayName(FromNode), *FromPinLabel,
		*ClaireonPCGGraphHelpers::GetNodeDisplayName(ToNode), *ToPinLabel);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_PCGGraphEdit::Operation_DisconnectAll(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString NodeIdentifier, PinLabel;
	if (!Params->TryGetStringField(TEXT("node"), NodeIdentifier) || NodeIdentifier.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: params.node"));
	}
	if (!Params->TryGetStringField(TEXT("pin"), PinLabel) || PinLabel.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: params.pin"));
	}

	int32 NodeIndex;
	UPCGNode* Node = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Data->PCGGraph.Get(), NodeIdentifier, NodeIndex);
	if (!Node)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeIdentifier));
	}

	FString Direction;
	Params->TryGetStringField(TEXT("direction"), Direction);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Disconnect All PCG Pins")));

	bool bRemoved = false;
	FName PinName(*PinLabel);

	if (Direction.IsEmpty() || Direction.Equals(TEXT("output"), ESearchCase::IgnoreCase))
	{
		bRemoved |= Data->PCGGraph->RemoveOutboundEdges(Node, PinName);
	}
	if (Direction.IsEmpty() || Direction.Equals(TEXT("input"), ESearchCase::IgnoreCase))
	{
		bRemoved |= Data->PCGGraph->RemoveInboundEdges(Node, PinName);
	}

	if (bRemoved)
	{
		ClaireonPCGGraphHelpers::NotifyGraphChanged(Data->PCGGraph.Get());
	}

	Data->LastOperationStatus = FString::Printf(TEXT("Disconnected all edges on %s.\"%s\"%s"),
		*ClaireonPCGGraphHelpers::GetNodeDisplayName(Node), *PinLabel,
		bRemoved ? TEXT("") : TEXT(" (no edges found)"));

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_PCGGraphEdit::Operation_SetNodeProperty(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString NodeIdentifier, PropertyName, Value;
	if (!Params->TryGetStringField(TEXT("node"), NodeIdentifier) || NodeIdentifier.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: params.node"));
	}
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: params.property_name"));
	}
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required parameter: params.value"));
	}

	int32 NodeIndex;
	UPCGNode* Node = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Data->PCGGraph.Get(), NodeIdentifier, NodeIndex);
	if (!Node)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeIdentifier));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set PCG Node Property")));

	FString Error;
	if (!ClaireonPCGGraphHelpers::SetNodeProperty(Node, PropertyName, Value, Error))
	{
		return MakeErrorResult(Error);
	}

	ClaireonPCGGraphHelpers::NotifyGraphChanged(Data->PCGGraph.Get());

	Data->LastOperationStatus = FString::Printf(TEXT("Set %s.%s = %s"),
		*ClaireonPCGGraphHelpers::GetNodeDisplayName(Node), *PropertyName, *Value);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_PCGGraphEdit::Operation_GetNodeProperties(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString NodeIdentifier;
	if (!Params->TryGetStringField(TEXT("node"), NodeIdentifier) || NodeIdentifier.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: params.node"));
	}

	int32 NodeIndex;
	UPCGNode* Node = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Data->PCGGraph.Get(), NodeIdentifier, NodeIndex);
	if (!Node)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeIdentifier));
	}

	FString PropertiesText = ClaireonPCGGraphHelpers::ReadNodeProperties(Node);
	FString NodeName = ClaireonPCGGraphHelpers::GetNodeDisplayName(Node);

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("node"), NodeName);
	ResultJson->SetStringField(TEXT("properties"), PropertiesText.IsEmpty() ? TEXT("(no editable properties)") : PropertiesText);

	return MakeSuccessResult(ResultJson, FString::Printf(TEXT("Properties for %s"), *NodeName));
}

FToolResult ClaireonTool_PCGGraphEdit::Operation_ListNodeTypes(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Filter;
	Params->TryGetStringField(TEXT("filter"), Filter);

	TArray<FString> Classes = ClaireonPCGGraphHelpers::GetAvailableSettingsClasses();

	TArray<TSharedPtr<FJsonValue>> TypesArray;
	for (const FString& ClassName : Classes)
	{
		if (!Filter.IsEmpty() && !ClassName.Contains(Filter, ESearchCase::IgnoreCase))
		{
			continue;
		}
		TypesArray.Add(MakeShared<FJsonValueString>(ClassName));
	}

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetArrayField(TEXT("node_types"), TypesArray);

	return MakeSuccessResult(ResultJson, FString::Printf(TEXT("%d available PCG node type(s)"), TypesArray.Num()));
}

FToolResult ClaireonTool_PCGGraphEdit::Operation_Focus(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString NodeIdentifier;
	if (!Params->TryGetStringField(TEXT("node"), NodeIdentifier) || NodeIdentifier.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: params.node"));
	}

	int32 NodeIndex;
	UPCGNode* Node = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Data->PCGGraph.Get(), NodeIdentifier, NodeIndex);
	if (!Node)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeIdentifier));
	}

	Data->PushHistory();
	Data->FocusedNodeIndex = NodeIndex;
	Data->LastOperationStatus = FString::Printf(TEXT("Focused on [%d] %s"), NodeIndex, *ClaireonPCGGraphHelpers::GetNodeDisplayName(Node));

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_PCGGraphEdit::Operation_CursorBack(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (Data->CursorHistory.Num() == 0)
	{
		Data->LastOperationStatus = TEXT("No cursor history");
		return BuildStateResponse(SessionId, Data);
	}

	Data->FocusedNodeIndex = Data->CursorHistory.Pop();
	Data->LastOperationStatus = FString::Printf(TEXT("Cursor back to [%d]"), Data->FocusedNodeIndex);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_PCGGraphEdit::Operation_Save(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (!Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid — PCG Graph is no longer loaded"));
	}

	UPackage* Package = Data->PCGGraph->GetOutermost();
	if (!Package)
	{
		return MakeErrorResult(TEXT("Could not find package for PCG Graph"));
	}

	FString PackageFilename;
	if (!FPackageName::DoesPackageExist(Package->GetName(), &PackageFilename))
	{
		return MakeErrorResult(FString::Printf(TEXT("Package file not found for: %s"), *Package->GetName()));
	}

	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		return MakeErrorResult(TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor."));
	}
	bool bSaved = UEditorLoadingAndSavingUtils::SavePackages({ Package }, false);

	Data->LastOperationStatus = bSaved ? TEXT("Saved successfully") : TEXT("Save failed");
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// apply_spec
// ============================================================================

FToolResult ClaireonTool_PCGGraphEdit::Operation_ApplySpec(const TSharedPtr<FJsonObject>& Params)
{
	// Extract asset_path -- required
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("apply_spec requires 'asset_path' parameter"));
	}

	// Extract spec -- required JSON object
	const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("spec"), SpecPtr) || !SpecPtr || !SpecPtr->IsValid())
	{
		return MakeErrorResult(TEXT("apply_spec requires 'spec' parameter (JSON object)"));
	}

	// Optional: reuse an existing session
	FString SessionId;
	Params->TryGetStringField(TEXT("session_id"), SessionId);

	FClaireonSpecApplicator_PCGGraph Applicator;
	return Applicator.ApplySpec(*SpecPtr, AssetPath, SessionId);
}
