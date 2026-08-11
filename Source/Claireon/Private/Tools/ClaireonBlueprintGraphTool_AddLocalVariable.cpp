// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintGraphTool_AddLocalVariable.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase_Internal.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonLog.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlueprintGraphTool_AddLocalVariable::GetOperation() const { return TEXT("add_local_variable"); }

TArray<FString> ClaireonBlueprintGraphTool_AddLocalVariable::GetSearchKeywords() const
{
    return {TEXT("bp"), TEXT("variable"), TEXT("local"), TEXT("add"), TEXT("function"), TEXT("declare"), TEXT("scope")};
}

FString ClaireonBlueprintGraphTool_AddLocalVariable::GetDescription() const
{
    return TEXT("Add a function-local variable to a Blueprint function graph. Targets the session's current graph when it is a function graph, or pass function_name explicitly. Pair with bp_add_node VariableGet/Set member_scope to reference it. Accepts session_id or asset_path; auto-opens when asset_path is supplied.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_AddLocalVariable::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("function_name"), TEXT("Function graph to declare the local in. Defaults to the session's current graph (which must be a function graph)."));
    Builder.AddString(TEXT("variable_name"), TEXT("Name of the new local variable."), true);
    Builder.AddString(TEXT("variable_type"), TEXT("Variable type (primitive name like 'bool'/'int'/'float', struct/class name, or full path). Required unless variable_type_spec is provided."));
    Builder.AddObject(TEXT("variable_type_spec"), TEXT("Structured type spec (same grammar as bp_add_variable). Takes precedence over variable_type."));
    Builder.AddString(TEXT("default_value"), TEXT("Optional default value (string form)."));
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_AddLocalVariable::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("add_local_variable"), Params, SessionId, Data, Error))
    {
        return Error;
    }

	UBlueprint* Blueprint = Data->Blueprint.Get();
	if (!IsValid(Blueprint))
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	FString VarName;
	if (!Params->TryGetStringField(TEXT("variable_name"), VarName))
	{
		return MakeErrorResult(TEXT("Missing required field: variable_name"));
	}

	// Resolve the target function graph: explicit function_name, else the
	// session's current graph.
	UEdGraph* TargetGraph = nullptr;
	FString FunctionName;
	if (Params->TryGetStringField(TEXT("function_name"), FunctionName) && !FunctionName.IsEmpty())
	{
		for (UEdGraph* G : Blueprint->FunctionGraphs)
		{
			if (IsValid(G) && G->GetName() == FunctionName)
			{
				TargetGraph = G;
				break;
			}
		}
		if (!IsValid(TargetGraph))
		{
			TArray<FString> Avail;
			for (UEdGraph* G : Blueprint->FunctionGraphs) { if (IsValid(G)) Avail.Add(G->GetName()); }
			return MakeErrorResult(FString::Printf(
				TEXT("Function graph '%s' not found. Available: %s"),
				*FunctionName, *FString::Join(Avail, TEXT(", "))));
		}
	}
	else
	{
		TargetGraph = Data->Graph.Get();
		if (!IsValid(TargetGraph) || !Blueprint->FunctionGraphs.Contains(TargetGraph))
		{
			return MakeErrorResult(TEXT("The session's current graph is not a function graph; pass function_name to target one (locals can only be declared on function graphs)."));
		}
	}

	// Parse the variable type via the shared helpers (same grammar as bp_add_variable).
	FString VarType;
	const TSharedPtr<FJsonObject>* TypeSpecObj = nullptr;
	const bool bHasTypeSpec = Params->TryGetObjectField(TEXT("variable_type_spec"), TypeSpecObj) && TypeSpecObj && (*TypeSpecObj).IsValid();
	const bool bHasTypeString = Params->TryGetStringField(TEXT("variable_type"), VarType);
	if (!bHasTypeSpec && !bHasTypeString)
	{
		return MakeErrorResult(TEXT("Missing required field: variable_type (or variable_type_spec)"));
	}

	ClaireonBlueprintHelpers::FParseVariableTypeResult ParseResult = bHasTypeSpec
		? ClaireonBlueprintHelpers::ParseVariableTypeSpec(*TypeSpecObj)
		: ClaireonBlueprintHelpers::ParseVariableTypeChecked(VarType);
	if (!ParseResult.bSucceeded)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to parse variable type: %s"), *ParseResult.Error));
	}

	// Duplicate check against the entry node's existing locals.
	for (UEdGraphNode* Node : TargetGraph->Nodes)
	{
		if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node); IsValid(Entry))
		{
			for (const FBPVariableDescription& Local : Entry->LocalVariables)
			{
				if (Local.VarName == FName(*VarName))
				{
					return MakeErrorResult(FString::Printf(
						TEXT("Local variable '%s' already exists in function '%s'"),
						*VarName, *TargetGraph->GetName()));
				}
			}
			break;
		}
	}

	FString DefaultValue;
	Params->TryGetStringField(TEXT("default_value"), DefaultValue);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Local Variable")));
	Blueprint->Modify();

	if (!FBlueprintEditorUtils::AddLocalVariable(Blueprint, TargetGraph, FName(*VarName), ParseResult.PinType, DefaultValue))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("FBlueprintEditorUtils::AddLocalVariable failed for '%s' in function '%s'"),
			*VarName, *TargetGraph->GetName()));
	}

	TArray<FString> ResolutionWarnings;
	if (!ParseResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(ParseResult.ResolutionNote);
	}

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Added local variable: %s (%s) in function %s"),
		*VarName, bHasTypeSpec ? TEXT("spec") : *VarType, *TargetGraph->GetName());

	FToolResult Result = BuildStateResponse(SessionId, Data);
	Result.Warnings.Append(ResolutionWarnings);
	return Result;
}

#undef LOCTEXT_NAMESPACE
