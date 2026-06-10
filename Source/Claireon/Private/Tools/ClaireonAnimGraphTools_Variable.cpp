// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimGraphTools_Variable.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "Tools/ClaireonAnimGraphEditBase.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonNameResolver.h"

#include "Animation/AnimBlueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Event.h"
#include "ScopedTransaction.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using FToolResult = IClaireonTool::FToolResult;

#define LOCTEXT_NAMESPACE "ClaireonAnimGraphTools_Variable"

// ============================================================================
// ClaireonAnimGraphTool_AddVariable
// ============================================================================

FString ClaireonAnimGraphTool_AddVariable::GetOperation() const { return TEXT("add_variable"); }

FString ClaireonAnimGraphTool_AddVariable::GetDescription() const
{
    return TEXT("Add a new variable to the Animation Blueprint in the open anim_graph session. Session-mode tool: open via anim_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_AddVariable::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("name"), TEXT("Variable name"), true);
	S.AddString(TEXT("type"), TEXT("Variable type (e.g., 'float', 'int', 'Array<float>')"), true);
	S.AddString(TEXT("category"), TEXT("Variable category"));
	S.AddString(TEXT("default_value"), TEXT("Default value for the variable"));
	S.AddObject(TEXT("flags"), TEXT("Variable flags (e.g., BlueprintReadOnly, Transient)"));
	S.AddObject(TEXT("metadata"), TEXT("Variable metadata (Units, UIMin, UIMax, ClampMin, etc.)"));
	S.AddString(TEXT("function_name"), TEXT("If set, add as a local variable within this function scope"));
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_AddVariable::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();

	FString VarName, TypeStr;
	if (!Arguments->TryGetStringField(TEXT("name"), VarName))
		return MakeErrorResult(TEXT("Missing required field: name"));
	if (!Arguments->TryGetStringField(TEXT("type"), TypeStr))
		return MakeErrorResult(TEXT("Missing required field: type"));

	ClaireonBlueprintHelpers::FParseVariableTypeResult ParseResult = ClaireonBlueprintHelpers::ParseVariableTypeChecked(TypeStr);
	if (!ParseResult.bSucceeded)
	{
		FToolResult ParseErr = MakeErrorResult(FString::Printf(TEXT("Failed to parse variable type: %s"), *ParseResult.Error));
		TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> ParserErrObj = MakeShared<FJsonObject>();
		ParserErrObj->SetStringField(TEXT("input"), TypeStr);
		ParserErrObj->SetStringField(TEXT("message"), ParseResult.Error);
		DataObj->SetObjectField(TEXT("type_parser_error"), ParserErrObj);
		ParseErr.Data = DataObj;
		return ParseErr;
	}
	FEdGraphPinType PinType = ParseResult.PinType;

	// Check if it's a local variable (scoped to a function)
	FString FunctionName;
	Arguments->TryGetStringField(TEXT("function_name"), FunctionName);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Variable")));
	AnimBP->Modify();

	if (!FunctionName.IsEmpty())
	{
		// Local variable
		UEdGraph* FuncGraph = ClaireonBlueprintHelpers::FindGraphByName(AnimBP, FunctionName);
		if (!FuncGraph)
		{
			return MakeErrorResult(FString::Printf(TEXT("Function '%s' not found"), *FunctionName));
		}

		FString DefaultValue;
		Arguments->TryGetStringField(TEXT("default_value"), DefaultValue);

		bool bAdded = FBlueprintEditorUtils::AddLocalVariable(AnimBP, FuncGraph, FName(*VarName), PinType, DefaultValue);
		if (!bAdded)
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to add local variable '%s' to function '%s'"), *VarName, *FunctionName));
		}
	}
	else
	{
		// Blueprint-level variable
		FBPVariableDescription NewVar;
		NewVar.VarName = FName(*VarName);
		NewVar.VarType = PinType;
		NewVar.FriendlyName = VarName;
		NewVar.Category = FText::FromString(TEXT("Default"));
		NewVar.PropertyFlags |= CPF_Edit | CPF_BlueprintVisible;  // BlueprintReadWrite by default

		FString DefaultValue;
		if (Arguments->TryGetStringField(TEXT("default_value"), DefaultValue))
		{
			NewVar.DefaultValue = DefaultValue;
		}

		AnimBP->NewVariables.Add(NewVar);

		// Apply additional properties (category, tooltip, metadata, flags)
		TSharedPtr<FJsonObject> PropsToApply = MakeShared<FJsonObject>();

		FString Category;
		if (Arguments->TryGetStringField(TEXT("category"), Category))
			PropsToApply->SetStringField(TEXT("category"), Category);

		FString Tooltip;
		if (Arguments->TryGetStringField(TEXT("tooltip"), Tooltip))
			PropsToApply->SetStringField(TEXT("tooltip"), Tooltip);

		const TSharedPtr<FJsonObject>* MetadataObj = nullptr;
		if (Arguments->TryGetObjectField(TEXT("metadata"), MetadataObj))
			PropsToApply->SetObjectField(TEXT("metadata"), *MetadataObj);

		if (PropsToApply->Values.Num() > 0)
		{
			ClaireonBlueprintHelpers::ApplyVariableProperties(AnimBP, FName(*VarName), PropsToApply);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Added variable '%s' (%s)%s"),
		*VarName, *TypeStr, FunctionName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" in function '%s'"), *FunctionName));

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_RemoveVariable
// ============================================================================

FString ClaireonAnimGraphTool_RemoveVariable::GetOperation() const { return TEXT("remove_variable"); }

FString ClaireonAnimGraphTool_RemoveVariable::GetDescription() const
{
    return TEXT("Remove a variable from the Animation Blueprint in the open anim_graph session. Session-mode tool: open via anim_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_RemoveVariable::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("name"), TEXT("Name of the variable to remove"), true);
	S.AddString(TEXT("function_name"), TEXT("If set, remove a local variable from this function scope"));
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_RemoveVariable::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();

	FString VarName;
	if (!Arguments->TryGetStringField(TEXT("name"), VarName))
		return MakeErrorResult(TEXT("Missing required field: name"));

	FString FunctionName;
	Arguments->TryGetStringField(TEXT("function_name"), FunctionName);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Variable")));
	AnimBP->Modify();

	if (!FunctionName.IsEmpty())
	{
		UEdGraph* FuncGraph = ClaireonBlueprintHelpers::FindGraphByName(AnimBP, FunctionName);
		if (!FuncGraph)
			return MakeErrorResult(FString::Printf(TEXT("Function '%s' not found"), *FunctionName));

		UStruct* Scope = AnimBP->SkeletonGeneratedClass ? Cast<UStruct>(AnimBP->SkeletonGeneratedClass->FindFunctionByName(FName(*FunctionName))) : nullptr;
		if (!Scope)
			return MakeErrorResult(FString::Printf(TEXT("Could not find scope for function '%s'"), *FunctionName));

		FBlueprintEditorUtils::RemoveLocalVariable(AnimBP, Scope, FName(*VarName));
	}
	else
	{
		FBlueprintEditorUtils::RemoveMemberVariable(AnimBP, FName(*VarName));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Removed variable '%s'"), *VarName);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_SetVariableProperties
// ============================================================================

FString ClaireonAnimGraphTool_SetVariableProperties::GetOperation() const { return TEXT("set_variable_properties"); }

FString ClaireonAnimGraphTool_SetVariableProperties::GetDescription() const
{
    return TEXT("Set properties on an existing Animation Blueprint variable (category, tooltip, flags, metadata). Session-mode tool: open via anim_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_SetVariableProperties::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("name"), TEXT("Name of the variable to modify"), true);
	S.AddString(TEXT("function_name"), TEXT("If set, modify a local variable within this function scope"));
	S.AddString(TEXT("category"), TEXT("New category for the variable"));
	S.AddString(TEXT("tooltip"), TEXT("Tooltip text for the variable"));
	S.AddObject(TEXT("flags"), TEXT("Variable flags to set"));
	S.AddObject(TEXT("metadata"), TEXT("Variable metadata to set (Units, UIMin, UIMax, ClampMin, etc.)"));
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_SetVariableProperties::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();

	FString VarName;
	if (!Arguments->TryGetStringField(TEXT("name"), VarName))
		return MakeErrorResult(TEXT("Missing required field: name"));

	FString FunctionName;
	Arguments->TryGetStringField(TEXT("function_name"), FunctionName);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Variable Properties")));
	AnimBP->Modify();

	// Determine local variable scope
	const UStruct* LocalScope = nullptr;
	if (!FunctionName.IsEmpty())
	{
		LocalScope = AnimBP->SkeletonGeneratedClass ? Cast<UStruct>(AnimBP->SkeletonGeneratedClass->FindFunctionByName(FName(*FunctionName))) : nullptr;
		if (!LocalScope)
			return MakeErrorResult(FString::Printf(TEXT("Could not find scope for function '%s'"), *FunctionName));
	}

	TArray<FString> ChangedProps;
	FName VarFName(*VarName);

	// Category
	FString Category;
	if (Arguments->TryGetStringField(TEXT("category"), Category))
	{
		FBlueprintEditorUtils::SetBlueprintVariableCategory(AnimBP, VarFName, LocalScope, FText::FromString(Category));
		ChangedProps.Add(TEXT("category"));
	}

	// Tooltip
	FString Tooltip;
	if (Arguments->TryGetStringField(TEXT("tooltip"), Tooltip))
	{
		FBlueprintEditorUtils::SetBlueprintVariableMetaData(AnimBP, VarFName, LocalScope, FBlueprintMetadata::MD_Tooltip, Tooltip);
		ChangedProps.Add(TEXT("tooltip"));
	}

	// Metadata (arbitrary key-value pairs: Units, UIMin, UIMax, etc.)
	const TSharedPtr<FJsonObject>* MetadataObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("metadata"), MetadataObj) && MetadataObj && (*MetadataObj).IsValid())
	{
		for (const auto& Pair : (*MetadataObj)->Values)
		{
			FString Value;
			if (Pair.Value.IsValid() && Pair.Value->TryGetString(Value))
			{
				FBlueprintEditorUtils::SetBlueprintVariableMetaData(AnimBP, VarFName, LocalScope, FName(*Pair.Key), Value);
				ChangedProps.Add(Pair.Key);
			}
		}
	}

	// Apply flags via existing helper (only for blueprint-level variables, not local)
	if (FunctionName.IsEmpty())
	{
		TSharedPtr<FJsonObject> PropsToApply = MakeShared<FJsonObject>();
		const TArray<TSharedPtr<FJsonValue>>* FlagsArray = nullptr;
		if (Arguments->TryGetArrayField(TEXT("flags"), FlagsArray))
		{
			PropsToApply->SetArrayField(TEXT("flags"), *FlagsArray);
		}
		if (PropsToApply->Values.Num() > 0)
		{
			ClaireonBlueprintHelpers::ApplyVariableProperties(AnimBP, VarFName, PropsToApply);
			ChangedProps.Add(TEXT("flags"));
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Set properties on '%s': %s"),
		*VarName, *FString::Join(ChangedProps, TEXT(", ")));
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_AddFunction
// ============================================================================

FString ClaireonAnimGraphTool_AddFunction::GetOperation() const { return TEXT("add_function"); }

FString ClaireonAnimGraphTool_AddFunction::GetDescription() const
{
    return TEXT("Add a new function to the Animation Blueprint in the open anim_graph session. Session-mode tool: open via anim_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_AddFunction::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("function_name"), TEXT("Name of the function to create"), true);
	S.AddBoolean(TEXT("is_thread_safe"), TEXT("Whether the function should be BlueprintThreadSafe"));
	S.AddObject(TEXT("inputs"), TEXT("Array of input params: [{name, type}]. Type uses ParseVariableType format (float, bool, int, etc.)"));
	S.AddObject(TEXT("outputs"), TEXT("Array of output params: [{name, type}]. Use name='ReturnValue' for the return value."));
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_AddFunction::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();

	FString FuncName;
	if (!Arguments->TryGetStringField(TEXT("function_name"), FuncName))
		return MakeErrorResult(TEXT("Missing required field: function_name"));

	bool bThreadSafe = false;
	Arguments->TryGetBoolField(TEXT("is_thread_safe"), bThreadSafe);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Function")));
	AnimBP->Modify();

	// Create the function graph
	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(AnimBP, FName(*FuncName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!NewGraph)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create function graph '%s'"), *FuncName));
	}

	FBlueprintEditorUtils::AddFunctionGraph<UClass>(AnimBP, NewGraph, true, nullptr);

	// Find the entry node
	TArray<UK2Node_FunctionEntry*> EntryNodes;
	NewGraph->GetNodesOfClass<UK2Node_FunctionEntry>(EntryNodes);
	UK2Node_FunctionEntry* EntryNode = EntryNodes.Num() > 0 ? EntryNodes[0] : nullptr;

	// Set thread safety metadata if requested
	if (bThreadSafe && EntryNode)
	{
		EntryNode->MetaData.bThreadSafe = true;
	}

	// Add input parameters (appear as output pins on the entry node)
	const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
	if (EntryNode && Arguments->TryGetArrayField(TEXT("inputs"), InputsArray))
	{
		for (const TSharedPtr<FJsonValue>& InputVal : *InputsArray)
		{
			const TSharedPtr<FJsonObject>* InputObj = nullptr;
			if (!InputVal.IsValid() || !InputVal->TryGetObject(InputObj)) continue;

			FString ParamName, ParamType;
			if (!(*InputObj)->TryGetStringField(TEXT("name"), ParamName)) continue;
			if (!(*InputObj)->TryGetStringField(TEXT("type"), ParamType)) continue;

			ClaireonBlueprintHelpers::FParseVariableTypeResult ParamParse = ClaireonBlueprintHelpers::ParseVariableTypeChecked(ParamType);
			if (!ParamParse.bSucceeded)
			{
				FToolResult ParamErr = MakeErrorResult(FString::Printf(TEXT("Failed to parse input param '%s' type: %s"), *ParamName, *ParamParse.Error));
				TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
				TSharedPtr<FJsonObject> ParserErrObj = MakeShared<FJsonObject>();
				ParserErrObj->SetStringField(TEXT("input"), ParamType);
				ParserErrObj->SetStringField(TEXT("message"), ParamParse.Error);
				DataObj->SetObjectField(TEXT("type_parser_error"), ParserErrObj);
				ParamErr.Data = DataObj;
				return ParamErr;
			}
			EntryNode->CreateUserDefinedPin(FName(*ParamName), ParamParse.PinType, EGPD_Output);
		}
	}

	// Add output parameters (appear as input pins on a result node)
	const TArray<TSharedPtr<FJsonValue>>* OutputsArray = nullptr;
	if (Arguments->TryGetArrayField(TEXT("outputs"), OutputsArray) && OutputsArray->Num() > 0)
	{
		UK2Node_FunctionResult* ResultNode = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(EntryNode);
		if (ResultNode)
		{
			for (const TSharedPtr<FJsonValue>& OutputVal : *OutputsArray)
			{
				const TSharedPtr<FJsonObject>* OutputObj = nullptr;
				if (!OutputVal.IsValid() || !OutputVal->TryGetObject(OutputObj)) continue;

				FString ParamName, ParamType;
				if (!(*OutputObj)->TryGetStringField(TEXT("name"), ParamName)) continue;
				if (!(*OutputObj)->TryGetStringField(TEXT("type"), ParamType)) continue;

				ClaireonBlueprintHelpers::FParseVariableTypeResult OutParamParse = ClaireonBlueprintHelpers::ParseVariableTypeChecked(ParamType);
				if (!OutParamParse.bSucceeded)
				{
					FToolResult OutParamErr = MakeErrorResult(FString::Printf(TEXT("Failed to parse output param '%s' type: %s"), *ParamName, *OutParamParse.Error));
					TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
					TSharedPtr<FJsonObject> ParserErrObj = MakeShared<FJsonObject>();
					ParserErrObj->SetStringField(TEXT("input"), ParamType);
					ParserErrObj->SetStringField(TEXT("message"), OutParamParse.Error);
					DataObj->SetObjectField(TEXT("type_parser_error"), ParserErrObj);
					OutParamErr.Data = DataObj;
					return OutParamErr;
				}
				ResultNode->CreateUserDefinedPin(FName(*ParamName), OutParamParse.PinType, EGPD_Input);
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Added function '%s'%s"),
		*FuncName, bThreadSafe ? TEXT(" (BlueprintThreadSafe)") : TEXT(""));

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_AddFunctionOverride
// ============================================================================

FString ClaireonAnimGraphTool_AddFunctionOverride::GetOperation() const { return TEXT("add_function_override"); }

FString ClaireonAnimGraphTool_AddFunctionOverride::GetDescription() const
{
	return TEXT("Override a parent class function (BlueprintNativeEvent or BlueprintImplementableEvent). "
		"Creates an event override node in the EventGraph. Use for BlueprintThreadSafeUpdateAnimation, "
		"BlueprintUpdateAnimation, etc.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_AddFunctionOverride::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("function_name"), TEXT("Name of the parent function to override (e.g., BlueprintThreadSafeUpdateAnimation)"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_AddFunctionOverride::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();

	FString FunctionName;
	if (!Arguments->TryGetStringField(TEXT("function_name"), FunctionName))
		return MakeErrorResult(TEXT("Missing required field: function_name"));

	UClass* ParentClass = AnimBP->ParentClass;
	if (!ParentClass)
		return MakeErrorResult(TEXT("Blueprint has no parent class"));

	// Resolve the function on the parent class
	ClaireonNameResolver::FNameResolveResult FuncResult;
	UFunction* TargetFunc = ClaireonNameResolver::ResolveFunctionName(ParentClass, FunctionName, FuncResult);
	if (!TargetFunc)
	{
		return MakeErrorResult(FuncResult.Error.IsEmpty()
			? FString::Printf(TEXT("Function '%s' not found on parent class '%s'"), *FunctionName, *ParentClass->GetName())
			: FuncResult.Error);
	}

	TArray<FString> Warnings;
	if (!FuncResult.ResolutionNote.IsEmpty())
		Warnings.Add(FuncResult.ResolutionNote);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Function Override")));
	AnimBP->Modify();

	const bool bIsBlueprintEvent = TargetFunc->HasAnyFunctionFlags(FUNC_BlueprintEvent);
	const bool bIsNativeEvent = TargetFunc->HasAnyFunctionFlags(FUNC_Native) && bIsBlueprintEvent;

	// BlueprintNativeEvent → event override node in EventGraph
	if (bIsNativeEvent)
	{
		// Check for existing event override
		UK2Node_Event* ExistingOverride = FBlueprintEditorUtils::FindOverrideForFunction(
			AnimBP, ParentClass, TargetFunc->GetFName());
		if (ExistingOverride)
		{
			return MakeErrorResult(FString::Printf(TEXT("Event override for '%s' already exists (GUID: %s)"),
				*TargetFunc->GetName(), *ExistingOverride->NodeGuid.ToString()));
		}

		UEdGraph* EventGraph = nullptr;
		if (AnimBP->UbergraphPages.Num() > 0)
		{
			EventGraph = AnimBP->UbergraphPages[0];
		}
		if (!EventGraph)
		{
			return MakeErrorResult(TEXT("No EventGraph found"));
		}

		EventGraph->Modify();
		UK2Node_Event* EventNode = NewObject<UK2Node_Event>(EventGraph);
		EventNode->EventReference.SetExternalMember(TargetFunc->GetFName(), ParentClass);
		EventNode->bOverrideFunction = true;
		EventGraph->AddNode(EventNode, true, false);
		EventNode->CreateNewGuid();
		EventNode->PostPlacedNewNode();
		EventNode->AllocateDefaultPins();
		EventNode->SetFlags(RF_Transactional);

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

		Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Event override added for '%s' in EventGraph (GUID: %s)"),
			*TargetFunc->GetName(), *EventNode->NodeGuid.ToString());
	}
	// BlueprintImplementableEvent or regular overridable → function graph override
	else
	{
		// Check for existing function graph with same name
		UEdGraph* ExistingGraph = ClaireonBlueprintHelpers::FindGraphByName(AnimBP, TargetFunc->GetName());
		if (ExistingGraph)
		{
			return MakeErrorResult(FString::Printf(TEXT("Function override for '%s' already exists as a graph"), *TargetFunc->GetName()));
		}

		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			AnimBP,
			TargetFunc->GetFName(),
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass());

		if (!NewGraph)
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to create function graph for '%s'"), *FunctionName));
		}

		FBlueprintEditorUtils::AddFunctionGraph<UClass>(AnimBP, NewGraph, true, TargetFunc->GetOuterUClass());
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

		Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Function override added for '%s' (graph: %s)"),
			*TargetFunc->GetName(), *NewGraph->GetName());
	}

	FToolResult Result = BuildStateResponse(SessionId, Data);
	Result.Warnings = Warnings;
	return Result;
}

// ============================================================================
// ClaireonAnimGraphTool_RemoveFunction
// ============================================================================

FString ClaireonAnimGraphTool_RemoveFunction::GetOperation() const { return TEXT("remove_function"); }

FString ClaireonAnimGraphTool_RemoveFunction::GetDescription() const
{
    return TEXT("Remove a function from the Animation Blueprint in the open anim_graph session. Session-mode tool: open via anim_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_RemoveFunction::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("function_name"), TEXT("Name of the function to remove"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_RemoveFunction::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();

	FString FuncName;
	if (!Arguments->TryGetStringField(TEXT("function_name"), FuncName))
		return MakeErrorResult(TEXT("Missing required field: function_name"));

	UEdGraph* FuncGraph = ClaireonBlueprintHelpers::FindGraphByName(AnimBP, FuncName);
	if (!FuncGraph)
		return MakeErrorResult(FString::Printf(TEXT("Function '%s' not found"), *FuncName));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Function")));
	AnimBP->Modify();

	FBlueprintEditorUtils::RemoveGraph(AnimBP, FuncGraph);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Removed function '%s'"), *FuncName);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_AddInterface
// ============================================================================

FString ClaireonAnimGraphTool_AddInterface::GetOperation() const { return TEXT("add_interface"); }

FString ClaireonAnimGraphTool_AddInterface::GetDescription() const
{
    return TEXT("Add an interface implementation to the Animation Blueprint in the open anim_graph session. Session-mode tool: open via anim_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_AddInterface::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("interface_class"), TEXT("Interface class name to implement"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_AddInterface::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();

	FString InterfaceClassName;
	if (!Arguments->TryGetStringField(TEXT("interface_class"), InterfaceClassName))
		return MakeErrorResult(TEXT("Missing required field: interface_class"));

	ClaireonNameResolver::FNameResolveResult ResolveResult;
	UClass* InterfaceClass = ClaireonNameResolver::ResolveClassName(InterfaceClassName, nullptr, ResolveResult);
	if (!InterfaceClass)
		return MakeErrorResult(FString::Printf(TEXT("Failed to resolve interface class '%s': %s"), *InterfaceClassName, *ResolveResult.Error));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Interface")));
	AnimBP->Modify();

	bool bAdded = FBlueprintEditorUtils::ImplementNewInterface(AnimBP, InterfaceClass->GetClassPathName());
	if (!bAdded)
		return MakeErrorResult(FString::Printf(TEXT("Failed to implement interface '%s'"), *InterfaceClassName));

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

	TArray<FString> Warnings;
	if (!ResolveResult.ResolutionNote.IsEmpty())
		Warnings.Add(ResolveResult.ResolutionNote);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Implemented interface '%s'"), *InterfaceClass->GetName());
	FToolResult Result = BuildStateResponse(SessionId, Data);
	Result.Warnings = Warnings;
	return Result;
}

// ============================================================================
// ClaireonAnimGraphTool_RemoveInterface
// ============================================================================

FString ClaireonAnimGraphTool_RemoveInterface::GetOperation() const { return TEXT("remove_interface"); }

FString ClaireonAnimGraphTool_RemoveInterface::GetDescription() const
{
    return TEXT("Remove an interface implementation from the Animation Blueprint in the open anim_graph session. Session-mode tool: open via anim_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_RemoveInterface::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("interface_class"), TEXT("Interface class name to remove"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_RemoveInterface::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();

	FString InterfaceClassName;
	if (!Arguments->TryGetStringField(TEXT("interface_class"), InterfaceClassName))
		return MakeErrorResult(TEXT("Missing required field: interface_class"));

	ClaireonNameResolver::FNameResolveResult ResolveResult;
	UClass* InterfaceClass = ClaireonNameResolver::ResolveClassName(InterfaceClassName, nullptr, ResolveResult);
	if (!InterfaceClass)
		return MakeErrorResult(FString::Printf(TEXT("Failed to resolve interface class '%s': %s"), *InterfaceClassName, *ResolveResult.Error));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Interface")));
	AnimBP->Modify();

	FBlueprintEditorUtils::RemoveInterface(AnimBP, InterfaceClass->GetClassPathName());
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Removed interface '%s'"), *InterfaceClass->GetName());
	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
