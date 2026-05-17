// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_SetVariableProperties.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonBlueprintHelpers.h"
#include "Dom/JsonObject.h"
#include "Tools/ClaireonSpecApplicator_Blueprint.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase_Internal.h"
#include "ClaireonLog.h"
#include "ClaireonSafeExec.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallDataTableFunction.h"
#include "K2Node_CallMaterialParameterCollectionFunction.h"
#include "K2Node_CommutativeAssociativeBinaryOperator.h"
#include "K2Node_Event.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Timeline.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_Select.h"
#include "K2Node_MacroInstance.h"
#include "Engine/MemberReference.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_Knot.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_Literal.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeMap.h"
#include "K2Node_MakeSet.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_AddPinInterface.h"
#include "K2Node_Switch.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_SwitchName.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_ForEachElementInEnum.h"
#include "K2Node_DoOnceMultiInput.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_AssignDelegate.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphUtilities.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ScopedTransaction.h"
#include "Animation/AnimBlueprint.h"
#include "AnimationGraph.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Root.h"
#include "K2Node_Tunnel.h"
#include "ClaireonBlueprintNodeSerializer.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonBPInterfaceAuthor.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;


FString ClaireonBlueprintGraphTool_SetVariableProperties::GetOperation() const { return TEXT("set_variable_properties"); }

FString ClaireonBlueprintGraphTool_SetVariableProperties::GetDescription() const
{
    return TEXT("Set properties on an existing Blueprint variable (flags, category, tooltip, replication, metadata) in the open editing session. Requires open session_id from blueprint_graph_open (or pass asset_path to auto-open). Transactional. Common pitfall: switching replication to RepNotify auto-creates the OnRep handler function graph if absent. Session-mode tool: open via blueprint_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_SetVariableProperties::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("variable_name"), TEXT("Name of the variable to modify."), true);
    Builder.AddString(TEXT("category"), TEXT("My Blueprint category."));
    Builder.AddString(TEXT("tooltip"), TEXT("Tooltip text."));
    Builder.AddString(TEXT("display_name"), TEXT("Friendly display name."));
    Builder.AddString(TEXT("replication"), TEXT("Replication mode: 'None' | 'Replicated' | 'RepNotify'."));
    Builder.AddString(TEXT("rep_notify_func"), TEXT("OnRep function name (used when replication='RepNotify')."));
    Builder.AddString(TEXT("replication_condition"), TEXT("ELifetimeCondition name (used when replication='RepNotify')."));
    Builder.AddObject(TEXT("metadata"), TEXT("Map of metadata key -> string value."));

    // flags[] and clear_flags[] are string arrays; inject directly.
    {
        TSharedPtr<FJsonObject> FlagsProp = MakeShared<FJsonObject>();
        FlagsProp->SetStringField(TEXT("type"), TEXT("array"));
        TSharedPtr<FJsonObject> StrItems = MakeShared<FJsonObject>();
        StrItems->SetStringField(TEXT("type"), TEXT("string"));
        FlagsProp->SetObjectField(TEXT("items"), StrItems);
        FlagsProp->SetStringField(TEXT("description"), TEXT("Flags to set (e.g. 'Interp', 'BlueprintReadOnly', 'BlueprintReadWrite', or UProperty flag names)."));
        Builder.Properties->SetObjectField(TEXT("flags"), FlagsProp);
    }
    {
        TSharedPtr<FJsonObject> ClearProp = MakeShared<FJsonObject>();
        ClearProp->SetStringField(TEXT("type"), TEXT("array"));
        TSharedPtr<FJsonObject> StrItems = MakeShared<FJsonObject>();
        StrItems->SetStringField(TEXT("type"), TEXT("string"));
        ClearProp->SetObjectField(TEXT("items"), StrItems);
        ClearProp->SetStringField(TEXT("description"), TEXT("Flags to clear."));
        Builder.Properties->SetObjectField(TEXT("clear_flags"), ClearProp);
    }

    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_SetVariableProperties::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("set_variable_properties"), Params, SessionId, Data, Error))
    {
        return Error;
    }
	UBlueprint* Blueprint = Data->Blueprint.Get();

	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	// Get variable name
	FString VarName;
	if (!Params->TryGetStringField(TEXT("variable_name"), VarName))
	{
		return MakeErrorResult(TEXT("Missing required field: variable_name"));
	}

	// Find the variable
	FName VarFName(*VarName);
	bool bFound = false;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == VarFName)
		{
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		return MakeErrorResult(FString::Printf(TEXT("Variable '%s' not found in Blueprint"), *VarName));
	}

	// Apply properties within a transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Variable Properties")));
	Blueprint->Modify();

	ClaireonBlueprintHelpers::FApplyVariableResult ApplyResult;
	ClaireonBlueprintHelpers::ApplyVariableProperties(Blueprint, VarFName, Params, &ApplyResult);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Set properties on variable: %s"),
		*VarName);

	FToolResult SetVarResult = BuildStateResponse(SessionId, Data);

	// Surface the RepNotify handler graph name on the tool response so callers
	// can immediately target it with blueprint_graph_add_node.
	if (!ApplyResult.RepNotifyHandlerGraph.IsNone())
	{
		if (!SetVarResult.Data.IsValid())
		{
			SetVarResult.Data = MakeShared<FJsonObject>();
		}
		SetVarResult.Data->SetStringField(
			TEXT("rep_notify_graph"),
			ApplyResult.RepNotifyHandlerGraph.ToString());
	}

	return SetVarResult;
}

#undef LOCTEXT_NAMESPACE
