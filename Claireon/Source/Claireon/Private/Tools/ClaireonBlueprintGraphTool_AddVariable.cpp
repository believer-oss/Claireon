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


FString ClaireonBlueprintGraphTool_AddVariable::GetOperation() const { return TEXT("add_variable"); }

TArray<FString> ClaireonBlueprintGraphTool_AddVariable::GetSearchKeywords() const
{
    return {TEXT("bp"), TEXT("var"), TEXT("variable"), TEXT("add"), TEXT("create"), TEXT("property"), TEXT("graph")};
}

FString ClaireonBlueprintGraphTool_AddVariable::GetDescription() const
{
    return TEXT("Add a member variable to the Blueprint in the open editing session. Transactional. Pass variable_type for primitives/classes/structs, or variable_type_spec (base + signature_function/subtype) for delegate/multicast/soft-class/soft-object/instanced-struct. Common pitfall: plain delegate names in variable_type silently fail. Accepts either session_id or asset_path; auto-opens a session when asset_path is supplied.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_AddVariable::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("variable_name"), TEXT("Name of the new variable."), true);
    Builder.AddString(TEXT("variable_type"), TEXT("Variable type (primitive name like 'bool'/'int'/'float' or full object/struct/class path). Required unless variable_type_spec is provided."));
    Builder.AddObject(TEXT("variable_type_spec"), TEXT("Structured type spec. Preferred for delegate / multicast-delegate / soft-ref / instanced-struct variables. Keys: 'base' (string; e.g. 'MulticastDelegate', 'Delegate', 'SoftClass', 'SoftObject', 'InstancedStruct'); 'signature_function' (string; required for delegate bases; UFunction object path, e.g. '/Script/FSTargeting.LockedTargetChanged__DelegateSignature'); 'subtype' (string; required for soft* bases, optional for InstancedStruct). Takes precedence over variable_type when both are provided."));
    Builder.AddString(TEXT("container_type"), TEXT("Optional container: 'none' | 'array' | 'set' | 'map' (default 'none')."));
    Builder.AddString(TEXT("default_value"), TEXT("Optional default value for the variable (string form)."));
    Builder.AddString(TEXT("category"), TEXT("Optional My Blueprint category."));
    Builder.AddString(TEXT("tooltip"), TEXT("Optional tooltip text."));
    Builder.AddBoolean(TEXT("instance_editable"), TEXT("Whether the variable is editable on instances."));
    Builder.AddBoolean(TEXT("blueprint_read_only"), TEXT("Whether the variable is read-only in Blueprints."));
    Builder.AddString(TEXT("replication"),
        TEXT("Replication mode: 'None' | 'Replicated' | 'RepNotify' (PascalCase preferred; 'none'/'replicated'/'rep_notify' also accepted). Default 'None'."));
    Builder.AddString(TEXT("rep_notify_func"),
        TEXT("Optional OnRep handler function name. Defaults to OnRep_<VariableName> when replication='RepNotify'. Ignored for other replication modes."));
    Builder.AddString(TEXT("replication_condition"),
        TEXT("Optional ELifetimeCondition name (used when replication='RepNotify')."));
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

	// Default flags mirror the editor's "+" button: CPF_Edit (visible in Class Defaults)
	// + CPF_BlueprintVisible (visible in the My Blueprint Variables panel and Get/Set
	// available in graphs). Callers can override via instance_editable / blueprint_read_only
	// / clear_flags.
	FBPVariableDescription NewVar;
	NewVar.VarName = FName(*VarName);
	NewVar.VarType = PinType;
	NewVar.FriendlyName = VarName;
	NewVar.Category = FText::FromString(TEXT("Default"));
	NewVar.PropertyFlags = CPF_Edit | CPF_BlueprintVisible;

	bool bInstanceEditable = true;
	if (Params->TryGetBoolField(TEXT("instance_editable"), bInstanceEditable) && !bInstanceEditable)
	{
		NewVar.PropertyFlags |= CPF_DisableEditOnInstance;
	}

	bool bBlueprintReadOnly = false;
	if (Params->TryGetBoolField(TEXT("blueprint_read_only"), bBlueprintReadOnly) && bBlueprintReadOnly)
	{
		NewVar.PropertyFlags |= CPF_BlueprintReadOnly;
	}

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
	ClaireonBlueprintHelpers::FApplyVariableResult ApplyResult;
	ClaireonBlueprintHelpers::ApplyVariableProperties(Blueprint, FName(*VarName), Params, &ApplyResult);

	// Mark Blueprint as structurally modified
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Added variable: %s (%s)"),
		*VarName, *VarType);

	FToolResult AddVariableResult = BuildStateResponse(SessionId, Data);
	AddVariableResult.Warnings.Append(ResolutionWarnings);

	// Surface the RepNotify handler graph name on the tool response so callers
	// can immediately target it with bp_add_node.
	if (!ApplyResult.RepNotifyHandlerGraph.IsNone())
	{
		if (!AddVariableResult.Data.IsValid())
		{
			AddVariableResult.Data = MakeShared<FJsonObject>();
		}
		AddVariableResult.Data->SetStringField(
			TEXT("rep_notify_graph"),
			ApplyResult.RepNotifyHandlerGraph.ToString());
	}

	return AddVariableResult;
}

// ----------------------------------------------------------------------------
// P1: hot-path metadata enrichment
// ----------------------------------------------------------------------------

FString ClaireonBlueprintGraphTool_AddVariable::GetFullDescription() const
{
    return TEXT(
        "Adds a member variable to the Blueprint via the current session. The "
        "variable type is specified via one of two parameters: variable_type "
        "(simple form: pass a primitive name like 'float', a class path like "
        "'/Game/BP/MyActor', or a struct path) OR variable_type_spec (structured "
        "form: an object with base + signature_function or subtype, used for "
        "delegate, multicast-delegate, soft-class, soft-object, and "
        "instanced-struct variables). When both are provided, variable_type_spec "
        "takes precedence -- this is a deliberate ergonomics choice so you can "
        "pass a fallback type alongside the structured form during refactors. "
        "Note: plain delegate names in "
        "variable_type silently fail; always use variable_type_spec with "
        "base='Delegate' for delegate variables. Setting replication='RepNotify' "
        "auto-creates the OnRep_<Name> handler function graph; pass "
        "rep_notify_func to customize the function name.");
}

FString ClaireonBlueprintGraphTool_AddVariable::GetExampleUsage() const
{
    return TEXT(
        "bp_add_variable session_id=\"...\" "
        "name=\"MaxHealth\" variable_type=\"float\" replication=\"RepNotify\"");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_AddVariable::GetParameterTooltips() const
{
    TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
    T->SetStringField(TEXT("session_id"), TEXT("Session ID returned by bp_open or _create."));
    T->SetStringField(TEXT("name"), TEXT("Variable name (also member name on the generated CDO)."));
    T->SetStringField(TEXT("variable_type"), TEXT("Simple type form: primitive name, class path, or struct path. Use variable_type_spec for delegate/soft-class/soft-object/instanced-struct types."));
    T->SetStringField(TEXT("variable_type_spec"), TEXT("Structured type form: { base: 'Delegate'|'MulticastDelegate'|'SoftClass'|'SoftObject'|'InstancedStruct', signature_function: '...', subtype: '...' }. Takes precedence over variable_type when both are provided."));
    T->SetStringField(TEXT("replication"), TEXT("'None' | 'Replicated' | 'RepNotify'. RepNotify auto-creates the OnRep_<Name> handler function graph (customizable via rep_notify_func)."));
    return T;
}

#undef LOCTEXT_NAMESPACE
