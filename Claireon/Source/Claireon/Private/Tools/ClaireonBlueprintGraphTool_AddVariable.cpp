// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_AddVariable.h"
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


FString ClaireonBlueprintGraphTool_AddVariable::GetName() const
{
    return TEXT("claireon.blueprint_graph_add_variable");
}

FString ClaireonBlueprintGraphTool_AddVariable::GetDescription() const
{
    return TEXT("Add a member variable to the Blueprint with type, default value, category, tooltip, replication, and metadata.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_AddVariable::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("variable_name"), TEXT("Name of the new variable."), true);
    Builder.AddString(TEXT("variable_type"), TEXT("Variable type (primitive name like 'bool'/'int'/'float' or full object/struct/class path)."), true);
    Builder.AddString(TEXT("container_type"), TEXT("Optional container: 'none' | 'array' | 'set' | 'map' (default 'none')."));
    Builder.AddString(TEXT("default_value"), TEXT("Optional default value for the variable (string form)."));
    Builder.AddString(TEXT("category"), TEXT("Optional My Blueprint category."));
    Builder.AddString(TEXT("tooltip"), TEXT("Optional tooltip text."));
    Builder.AddBoolean(TEXT("instance_editable"), TEXT("Whether the variable is editable on instances."));
    Builder.AddBoolean(TEXT("blueprint_read_only"), TEXT("Whether the variable is read-only in Blueprints."));
    Builder.AddString(TEXT("replication"), TEXT("Replication mode: 'none' | 'replicated' | 'rep_notify' (default 'none')."));
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_AddVariable::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("add_variable"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return Operation_AddVariable(SessionId, Data, Params);
}

FToolResult ClaireonBlueprintGraphEditToolBase::Operation_AddVariable(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();

	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	// Get variable name (always required).
	FString VarName;
	if (!Params->TryGetStringField(TEXT("variable_name"), VarName))
	{
		return MakeErrorResult(TEXT("Missing required field: variable_name"));
	}

	// Resolve variable type via either variable_type_spec (long-form) or variable_type (short-form).
	// variable_type_spec takes precedence when both are provided.
	FString VarType;
	const TSharedPtr<FJsonObject>* TypeSpecObj = nullptr;
	const bool bHasTypeSpec = Params->TryGetObjectField(TEXT("variable_type_spec"), TypeSpecObj) && TypeSpecObj && (*TypeSpecObj).IsValid();
	const bool bHasTypeString = Params->TryGetStringField(TEXT("variable_type"), VarType);
	if (!bHasTypeSpec && !bHasTypeString)
	{
		return MakeErrorResult(TEXT("Missing required field: variable_type (or variable_type_spec)"));
	}

	// Get optional default value
	FString DefaultValue;
	Params->TryGetStringField(TEXT("default_value"), DefaultValue);

	// Parse variable type; surface structured parser errors through data.type_parser_error.
	ClaireonBlueprintHelpers::FParseVariableTypeResult ParseResult = bHasTypeSpec
		? ClaireonBlueprintHelpers::ParseVariableTypeSpec(*TypeSpecObj)
		: ClaireonBlueprintHelpers::ParseVariableTypeChecked(VarType);
	if (!ParseResult.bSucceeded)
	{
		FToolResult ErrResult = MakeErrorResult(FString::Printf(TEXT("Failed to parse variable type: %s"), *ParseResult.Error));
		TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> ParserErr = MakeShared<FJsonObject>();
		FString InputDesc;
		if (bHasTypeSpec)
		{
			// Serialize the spec verbatim for the caller's debugging.
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&InputDesc);
			FJsonSerializer::Serialize((*TypeSpecObj).ToSharedRef(), Writer);
		}
		else
		{
			InputDesc = VarType;
		}
		ParserErr->SetStringField(TEXT("input"), InputDesc);
		ParserErr->SetStringField(TEXT("message"), ParseResult.Error);
		DataObj->SetObjectField(TEXT("type_parser_error"), ParserErr);
		ErrResult.Data = DataObj;
		return ErrResult;
	}
	FEdGraphPinType PinType = ParseResult.PinType;
	// Resolution note (e.g. fuzzy class match) surfaces as a non-fatal warning.
	TArray<FString> ResolutionWarnings;
	if (!ParseResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(ParseResult.ResolutionNote);
	}
	// If only variable_type was provided but variable_type_spec is what we needed, VarType will
	// be empty for the downstream status line; fall back to the base name when known.
	if (VarType.IsEmpty() && bHasTypeSpec)
	{
		(*TypeSpecObj)->TryGetStringField(TEXT("base"), VarType);
	}

	// Check if variable already exists
	for (const FBPVariableDescription& ExistingVar : Blueprint->NewVariables)
	{
		if (ExistingVar.VarName == FName(*VarName))
		{
			return MakeErrorResult(FString::Printf(TEXT("Variable '%s' already exists"), *VarName));
		}
	}

	// Create variable using transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Blueprint Variable")));
	Blueprint->Modify();

	// Create new variable
	FBPVariableDescription NewVar;
	NewVar.VarName = FName(*VarName);
	NewVar.VarType = PinType;
	NewVar.FriendlyName = VarName;
	NewVar.Category = FText::FromString(TEXT("Default"));

	// Set default value if provided
	if (!DefaultValue.IsEmpty())
	{
		NewVar.DefaultValue = DefaultValue;
	}

	// Add to Blueprint
	int32 VarIndex = Blueprint->NewVariables.Add(NewVar);

	// Apply optional properties (flags, category, replication, metadata, etc.)
	// Must be called after the variable is added to NewVariables since the
	// FBlueprintEditorUtils setter functions look up the variable by name.
	ClaireonBlueprintHelpers::ApplyVariableProperties(Blueprint, FName(*VarName), Params);

	// Mark Blueprint as structurally modified
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Added variable: %s (%s)"),
		*VarName, *VarType);

	FToolResult AddVariableResult = BuildStateResponse(SessionId, Data);
	AddVariableResult.Warnings.Append(ResolutionWarnings);
	return AddVariableResult;
}

#undef LOCTEXT_NAMESPACE
