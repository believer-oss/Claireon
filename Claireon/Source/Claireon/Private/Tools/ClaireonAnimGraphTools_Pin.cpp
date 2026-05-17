// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimGraphTools_Pin.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "Tools/ClaireonAnimGraphEditBase.h"
#include "Tools/ClaireonAnimGraphHelpers.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonLog.h"

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNodeBinding.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "ScopedTransaction.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using FToolResult = IClaireonTool::FToolResult;

#define LOCTEXT_NAMESPACE "ClaireonAnimGraphTools_Pin"

namespace ClaireonAnimGraphTools_PinInternal
{

// ============================================================================
// Helper: Find UAnimGraphNode_Base by GUID in graph
// ============================================================================

UAnimGraphNode_Base* FindAnimNodeByGuid(UEdGraph* Graph, const FString& GuidStr, FString& OutError)
{
	FGuid ParsedGuid;
	if (!FGuid::Parse(GuidStr, ParsedGuid) || !ParsedGuid.IsValid())
	{
		OutError = FString::Printf(TEXT("Invalid GUID: %s"), *GuidStr);
		return nullptr;
	}

	UEdGraphNode* Node = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, ParsedGuid);
	if (!Node)
	{
		OutError = FString::Printf(TEXT("Node not found: %s"), *GuidStr);
		return nullptr;
	}

	UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
	if (!AnimNode)
	{
		OutError = FString::Printf(TEXT("Node '%s' is not an animation graph node (class: %s)"),
			*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(), *Node->GetClass()->GetName());
		return nullptr;
	}

	return AnimNode;
}

}  // namespace ClaireonAnimGraphTools_PinInternal

// ============================================================================
// ClaireonAnimGraphTool_ExposePin
// ============================================================================

FString ClaireonAnimGraphTool_ExposePin::GetOperation() const { return TEXT("expose_pin"); }

FString ClaireonAnimGraphTool_ExposePin::GetDescription() const
{
	return TEXT("Expose a property as a visible pin on an animation graph node. "
		"The property must exist in the node's ShowPinForProperties array.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_ExposePin::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("node_guid"), TEXT("GUID of the target node"), true);
	S.AddString(TEXT("property_name"), TEXT("Name of the property to expose as a pin"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_ExposePin::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	UEdGraph* Graph = Data->CurrentGraph.Get();

	FString NodeGuidStr, PropertyName;
	if (!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName))
		return MakeErrorResult(TEXT("Missing required field: property_name"));

	FString FindError;
	UAnimGraphNode_Base* AnimNode = ClaireonAnimGraphTools_PinInternal::FindAnimNodeByGuid(Graph, NodeGuidStr, FindError);
	if (!AnimNode) return MakeErrorResult(FindError);

	// Find the property in ShowPinForProperties
	int32 FoundIndex = INDEX_NONE;
	for (int32 i = 0; i < AnimNode->ShowPinForProperties.Num(); ++i)
	{
		if (AnimNode->ShowPinForProperties[i].PropertyName.ToString().Equals(PropertyName, ESearchCase::IgnoreCase))
		{
			FoundIndex = i;
			break;
		}
	}

	if (FoundIndex == INDEX_NONE)
	{
		// List available properties
		TArray<FString> Available;
		for (const auto& Prop : AnimNode->ShowPinForProperties)
		{
			Available.Add(FString::Printf(TEXT("%s (%s)"),
				*Prop.PropertyName.ToString(),
				Prop.bShowPin ? TEXT("visible") : TEXT("hidden")));
		}
		return MakeErrorResult(FString::Printf(TEXT("Property '%s' not found in ShowPinForProperties. Available: %s"),
			*PropertyName, *FString::Join(Available, TEXT(", "))));
	}

	if (AnimNode->ShowPinForProperties[FoundIndex].bShowPin)
	{
		return MakeErrorResult(FString::Printf(TEXT("Property '%s' is already exposed as a pin"), *PropertyName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Expose Pin")));
	AnimNode->Modify();
	AnimNode->SetPinVisibility(true, FoundIndex);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	Graph->NotifyGraphChanged();

	Data->LastOperationAffectedNodes.Add(AnimNode->NodeGuid);
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Exposed pin '%s' on '%s'"),
		*PropertyName, *AnimNode->GetNodeTitle(ENodeTitleType::ListView).ToString());

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_HidePin
// ============================================================================

FString ClaireonAnimGraphTool_HidePin::GetOperation() const { return TEXT("hide_pin"); }

FString ClaireonAnimGraphTool_HidePin::GetDescription() const
{
	return TEXT("Hide a previously exposed pin on an animation graph node.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_HidePin::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("node_guid"), TEXT("GUID of the target node"), true);
	S.AddString(TEXT("property_name"), TEXT("Name of the property to hide"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_HidePin::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	UEdGraph* Graph = Data->CurrentGraph.Get();

	FString NodeGuidStr, PropertyName;
	if (!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName))
		return MakeErrorResult(TEXT("Missing required field: property_name"));

	FString FindError;
	UAnimGraphNode_Base* AnimNode = ClaireonAnimGraphTools_PinInternal::FindAnimNodeByGuid(Graph, NodeGuidStr, FindError);
	if (!AnimNode) return MakeErrorResult(FindError);

	int32 FoundIndex = INDEX_NONE;
	for (int32 i = 0; i < AnimNode->ShowPinForProperties.Num(); ++i)
	{
		if (AnimNode->ShowPinForProperties[i].PropertyName.ToString().Equals(PropertyName, ESearchCase::IgnoreCase))
		{
			FoundIndex = i;
			break;
		}
	}

	if (FoundIndex == INDEX_NONE)
	{
		return MakeErrorResult(FString::Printf(TEXT("Property '%s' not found in ShowPinForProperties"), *PropertyName));
	}

	if (!AnimNode->ShowPinForProperties[FoundIndex].bShowPin)
	{
		return MakeErrorResult(FString::Printf(TEXT("Property '%s' is already hidden"), *PropertyName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Hide Pin")));
	AnimNode->Modify();
	AnimNode->SetPinVisibility(false, FoundIndex);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	Graph->NotifyGraphChanged();

	Data->LastOperationAffectedNodes.Add(AnimNode->NodeGuid);
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Hidden pin '%s' on '%s'"),
		*PropertyName, *AnimNode->GetNodeTitle(ENodeTitleType::ListView).ToString());

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_SetBinding
// ============================================================================

FString ClaireonAnimGraphTool_SetBinding::GetOperation() const { return TEXT("set_binding"); }

FString ClaireonAnimGraphTool_SetBinding::GetDescription() const
{
	return TEXT("Set a property binding on an animation graph node. Binds a node property to a "
		"variable or function via the property access system. Default type is 'Property' (fast-path).");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_SetBinding::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("node_guid"), TEXT("GUID of the target node"), true);
	S.AddString(TEXT("property_name"), TEXT("Name of the property to bind (e.g., 'Alpha', 'bActiveValue')"), true);
	S.AddString(TEXT("binding_path"), TEXT("Binding source path (e.g., 'MoveSpeed' for a variable)"), true);
	S.AddString(TEXT("binding_type"), TEXT("Binding type: 'Property' (fast-path, default) or 'Function'"));
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_SetBinding::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	UEdGraph* Graph = Data->CurrentGraph.Get();

	FString NodeGuidStr, PropertyName, BindingPath;
	if (!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName))
		return MakeErrorResult(TEXT("Missing required field: property_name"));
	if (!Arguments->TryGetStringField(TEXT("binding_path"), BindingPath))
		return MakeErrorResult(TEXT("Missing required field: binding_path"));

	FString BindingTypeStr = TEXT("Property");
	Arguments->TryGetStringField(TEXT("binding_type"), BindingTypeStr);

	FString FindError;
	UAnimGraphNode_Base* AnimNode = ClaireonAnimGraphTools_PinInternal::FindAnimNodeByGuid(Graph, NodeGuidStr, FindError);
	if (!AnimNode) return MakeErrorResult(FindError);

	// Access the mutable binding object
	UAnimGraphNodeBinding* Binding = AnimNode->GetMutableBinding();
	if (!Binding)
	{
		return MakeErrorResult(TEXT("Node has no binding object"));
	}

	// Access PropertyBindings TMap via FMapProperty reflection
	FMapProperty* MapProp = CastField<FMapProperty>(
		Binding->GetClass()->FindPropertyByName(TEXT("PropertyBindings")));
	if (!MapProp)
	{
		return MakeErrorResult(TEXT("Could not find PropertyBindings on binding object"));
	}

	auto* BindingsMap = MapProp->ContainerPtrToValuePtr<TMap<FName, FAnimGraphNodePropertyBinding>>(Binding);
	if (!BindingsMap)
	{
		return MakeErrorResult(TEXT("Could not access PropertyBindings map"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Property Binding")));
	Binding->Modify();
	AnimNode->Modify();

	// Create or update the binding
	FName PropFName(*PropertyName);
	FAnimGraphNodePropertyBinding& NewBinding = BindingsMap->FindOrAdd(PropFName);
	NewBinding.PropertyPath = {BindingPath};
	NewBinding.bIsBound = true;

	if (BindingTypeStr.Equals(TEXT("Function"), ESearchCase::IgnoreCase))
	{
		NewBinding.Type = EAnimGraphNodePropertyBindingType::Function;
	}
	else
	{
		NewBinding.Type = EAnimGraphNodePropertyBindingType::Property;
	}

	// Reconstruct node to apply binding changes
	AnimNode->ReconstructNode();

	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);
	Graph->NotifyGraphChanged();

	Data->LastOperationAffectedNodes.Add(AnimNode->NodeGuid);
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Bound '%s' to '%s' (%s) on '%s'"),
		*PropertyName, *BindingPath, *BindingTypeStr,
		*AnimNode->GetNodeTitle(ENodeTitleType::ListView).ToString());

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_RemoveBinding
// ============================================================================

FString ClaireonAnimGraphTool_RemoveBinding::GetOperation() const { return TEXT("remove_binding"); }

FString ClaireonAnimGraphTool_RemoveBinding::GetDescription() const
{
	return TEXT("Remove a property binding from an animation graph node.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_RemoveBinding::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("node_guid"), TEXT("GUID of the target node"), true);
	S.AddString(TEXT("property_name"), TEXT("Name of the property to unbind"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_RemoveBinding::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	UEdGraph* Graph = Data->CurrentGraph.Get();

	FString NodeGuidStr, PropertyName;
	if (!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName))
		return MakeErrorResult(TEXT("Missing required field: property_name"));

	FString FindError;
	UAnimGraphNode_Base* AnimNode = ClaireonAnimGraphTools_PinInternal::FindAnimNodeByGuid(Graph, NodeGuidStr, FindError);
	if (!AnimNode) return MakeErrorResult(FindError);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Property Binding")));
	AnimNode->Modify();

	AnimNode->RemoveBindings(FName(*PropertyName));
	AnimNode->ReconstructNode();

	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);
	Graph->NotifyGraphChanged();

	Data->LastOperationAffectedNodes.Add(AnimNode->NodeGuid);
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Removed binding for '%s' on '%s'"),
		*PropertyName, *AnimNode->GetNodeTitle(ENodeTitleType::ListView).ToString());

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_BindFunction
// ============================================================================

FString ClaireonAnimGraphTool_BindFunction::GetOperation() const { return TEXT("bind_function"); }

FString ClaireonAnimGraphTool_BindFunction::GetDescription() const
{
	return TEXT("Bind a BlueprintThreadSafe function to a node event (OnBecomeRelevant, OnUpdate, OnInitialUpdate). "
		"The function must exist on the AnimBP and be marked BlueprintThreadSafe.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_BindFunction::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("node_guid"), TEXT("GUID of the target node"), true);
	S.AddEnum(TEXT("event_type"), TEXT("Event to bind to"),
		{TEXT("OnBecomeRelevant"), TEXT("OnUpdate"), TEXT("OnInitialUpdate")}, true);
	S.AddString(TEXT("function_name"), TEXT("Name of the BlueprintThreadSafe function to bind"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_BindFunction::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	UEdGraph* Graph = Data->CurrentGraph.Get();

	FString NodeGuidStr, EventType, FunctionName;
	if (!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	if (!Arguments->TryGetStringField(TEXT("event_type"), EventType))
		return MakeErrorResult(TEXT("Missing required field: event_type"));
	if (!Arguments->TryGetStringField(TEXT("function_name"), FunctionName))
		return MakeErrorResult(TEXT("Missing required field: function_name"));

	FString FindError;
	UAnimGraphNode_Base* AnimNode = ClaireonAnimGraphTools_PinInternal::FindAnimNodeByGuid(Graph, NodeGuidStr, FindError);
	if (!AnimNode) return MakeErrorResult(FindError);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Bind Function to Node")));
	AnimNode->Modify();

	FName FuncFName(*FunctionName);

	if (EventType == TEXT("OnBecomeRelevant"))
	{
		AnimNode->BecomeRelevantFunction.SetSelfMember(FuncFName);
	}
	else if (EventType == TEXT("OnUpdate"))
	{
		AnimNode->UpdateFunction.SetSelfMember(FuncFName);
	}
	else if (EventType == TEXT("OnInitialUpdate"))
	{
		AnimNode->InitialUpdateFunction.SetSelfMember(FuncFName);
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid event_type: '%s'. Use OnBecomeRelevant, OnUpdate, or OnInitialUpdate"), *EventType));
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);
	Graph->NotifyGraphChanged();

	Data->LastOperationAffectedNodes.Add(AnimNode->NodeGuid);
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Bound '%s' to %s on '%s'"),
		*FunctionName, *EventType,
		*AnimNode->GetNodeTitle(ENodeTitleType::ListView).ToString());

	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
