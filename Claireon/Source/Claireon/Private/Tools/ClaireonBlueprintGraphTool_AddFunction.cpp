// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintGraphTool_AddFunction.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonBlueprintHelpers.h"
#include "Dom/JsonObject.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase_Internal.h"
#include "ClaireonLog.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;


FString ClaireonBlueprintGraphTool_AddFunction::GetName() const
{
	return TEXT("claireon.blueprint_graph_add_function");
}

FString ClaireonBlueprintGraphTool_AddFunction::GetDescription() const
{
	return TEXT("Create a new user-defined function graph on the Blueprint in the open editing session. Requires open session_id from claireon.blueprint_graph_open (or pass asset_path to auto-open). Transactional. Parity with claireon.animgraph_add_function for regular Blueprints. The function_name must be unique on the BP.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_AddFunction::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
	Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
	Builder.AddString(TEXT("function_name"), TEXT("Name of the function to create."), true);
	Builder.AddBoolean(TEXT("is_pure"), TEXT("If true, the function is pure (no exec pins)."));
	Builder.AddObject(TEXT("inputs"), TEXT("Array of input params: [{name, type}]. Type uses ParseVariableType format (float, bool, int, etc.)."));
	Builder.AddObject(TEXT("outputs"), TEXT("Array of output params: [{name, type}]. Use name='ReturnValue' for the return value."));
	Builder.AddString(TEXT("category"), TEXT("Optional function category (appears in My Blueprint pane)."));
	Builder.AddString(TEXT("access_specifier"), TEXT("Optional: 'Public' | 'Protected' | 'Private' (default 'Public')."));
	Builder.AddBoolean(TEXT("is_const"), TEXT("If true, the function is marked const."));
	Builder.AddBoolean(TEXT("is_static"), TEXT("If true, the function is static."));
	Builder.AddString(TEXT("is_network_call"), TEXT("Optional: 'None' | 'Server' | 'Client' | 'NetMulticast'."));
	Builder.AddString(TEXT("tooltip"), TEXT("Optional tooltip text."));
	Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
	return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_AddFunction::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	TSharedPtr<FJsonObject> Params;
	FString SessionId;
	FBlueprintEditToolData* Data = nullptr;
	FToolResult Error;
	if (!BeginSessionOp(Arguments, TEXT("add_function"), Params, SessionId, Data, Error))
	{
		return Error;
	}
	return CheckMutationAffectedNodes(TEXT("add_function"), Data, AddFunction_Impl(SessionId, Data, Params));
}

FToolResult ClaireonBlueprintGraphTool_AddFunction::AddFunction_Impl(
	const FString& SessionId,
	FBlueprintEditToolData* Data,
	const TSharedPtr<FJsonObject>& Params)
{
	// === 1. Required: function_name ===
	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field 'function_name' for add_function"));
	}

	UBlueprint* Blueprint = Data->Blueprint.Get();
	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	// === 2. Collision check -- only walk Blueprint->FunctionGraphs (matches AddFunctionOverride) ===
	const FName FuncFName(*FunctionName);
	for (UEdGraph* ExistingGraph : Blueprint->FunctionGraphs)
	{
		if (ExistingGraph && ExistingGraph->GetFName() == FuncFName)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Function '%s' already exists as function graph"), *FunctionName));
		}
	}

	// === 3. Read optional flag inputs up-front so we can validate enum strings before mutating ===
	bool bIsPure = false;          Params->TryGetBoolField(TEXT("is_pure"), bIsPure);
	bool bIsConst = false;         Params->TryGetBoolField(TEXT("is_const"), bIsConst);
	bool bIsStatic = false;        Params->TryGetBoolField(TEXT("is_static"), bIsStatic);
	FString Category;              Params->TryGetStringField(TEXT("category"), Category);
	FString Tooltip;               Params->TryGetStringField(TEXT("tooltip"), Tooltip);

	FString AccessSpec;
	int32 AccessFlags = FUNC_Public; // baseline; AddFunctionGraph already applies FUNC_Public
	if (Params->TryGetStringField(TEXT("access_specifier"), AccessSpec) && !AccessSpec.IsEmpty())
	{
		if (AccessSpec == TEXT("Public"))         { AccessFlags = FUNC_Public; }
		else if (AccessSpec == TEXT("Protected")) { AccessFlags = FUNC_Protected; }
		else if (AccessSpec == TEXT("Private"))   { AccessFlags = FUNC_Private; }
		else
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Invalid access_specifier '%s' (expected Public, Protected, Private)"), *AccessSpec));
		}
	}

	FString NetCall;
	int32 NetFlags = 0;
	if (Params->TryGetStringField(TEXT("is_network_call"), NetCall) && !NetCall.IsEmpty() && NetCall != TEXT("None"))
	{
		if (NetCall == TEXT("Server"))           { NetFlags = FUNC_Net | FUNC_NetServer   | FUNC_NetReliable; }
		else if (NetCall == TEXT("Client"))      { NetFlags = FUNC_Net | FUNC_NetClient   | FUNC_NetReliable; }
		else if (NetCall == TEXT("NetMulticast")){ NetFlags = FUNC_Net | FUNC_NetMulticast | FUNC_NetReliable; }
		else
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Invalid is_network_call value '%s' (expected None, Server, Client, NetMulticast)"), *NetCall));
		}
	}

	// === 4. Transaction + create graph ===
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Function")));
	Blueprint->Modify();

	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, FuncFName, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!NewGraph)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Failed to create function graph '%s'"), *FunctionName));
	}
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewGraph, /*bIsUserCreated=*/true, /*SignatureFromClass=*/nullptr);

	// === 5. Locate auto-created entry node ===
	UK2Node_FunctionEntry* EntryNode = nullptr;
	{
		TArray<UK2Node_FunctionEntry*> EntryNodes;
		NewGraph->GetNodesOfClass<UK2Node_FunctionEntry>(EntryNodes);
		if (EntryNodes.Num() > 0) { EntryNode = EntryNodes[0]; }
	}
	if (!EntryNode)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Failed to find entry node in new function graph '%s'"), *FunctionName));
	}

	// === 6. Compute final ExtraFlags, clear baseline access then OR everything in one assignment ===
	// AddFunctionGraph applies FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public by default.
	// We preserve FUNC_BlueprintCallable | FUNC_BlueprintEvent, clear the access bits, then OR in our
	// computed access + pure/const/static/net flags.
	int32 ExtraFlags = EntryNode->GetExtraFlags();
	ExtraFlags &= ~(FUNC_Public | FUNC_Protected | FUNC_Private);
	ExtraFlags |= AccessFlags;
	if (bIsPure)   { ExtraFlags |= FUNC_BlueprintPure; }
	if (bIsConst)  { ExtraFlags |= FUNC_Const; }
	if (bIsStatic) { ExtraFlags |= FUNC_Static; }
	ExtraFlags |= NetFlags;
	EntryNode->SetExtraFlags(ExtraFlags);

	if (!Category.IsEmpty()) { EntryNode->MetaData.Category = FText::FromString(Category); }
	if (!Tooltip.IsEmpty())  { EntryNode->MetaData.ToolTip  = FText::FromString(Tooltip); }

	// === 7. Input params (output pins on entry node) ===
	const TArray<TSharedPtr<FJsonValue>>* InputsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("inputs"), InputsArray))
	{
		for (const TSharedPtr<FJsonValue>& InputVal : *InputsArray)
		{
			const TSharedPtr<FJsonObject>* InputObj = nullptr;
			if (!InputVal.IsValid() || !InputVal->TryGetObject(InputObj)) continue;

			FString ParamName, ParamType;
			if (!(*InputObj)->TryGetStringField(TEXT("name"), ParamName)) continue;
			if (!(*InputObj)->TryGetStringField(TEXT("type"), ParamType)) continue;

			ClaireonBlueprintHelpers::FParseVariableTypeResult Parse = ClaireonBlueprintHelpers::ParseVariableTypeChecked(ParamType);
			if (!Parse.bSucceeded)
			{
				FToolResult Err = MakeErrorResult(FString::Printf(
					TEXT("Failed to parse input param '%s' type: %s"), *ParamName, *Parse.Error));
				TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
				TSharedPtr<FJsonObject> ParserErrObj = MakeShared<FJsonObject>();
				ParserErrObj->SetStringField(TEXT("input"), ParamType);
				ParserErrObj->SetStringField(TEXT("message"), Parse.Error);
				DataObj->SetObjectField(TEXT("type_parser_error"), ParserErrObj);
				Err.Data = DataObj;
				return Err;
			}
			EntryNode->CreateUserDefinedPin(FName(*ParamName), Parse.PinType, EGPD_Output);
		}
	}

	// === 8. Output params (input pins on result node) ===
	UK2Node_FunctionResult* ResultNode = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* OutputsArray = nullptr;
	if (Params->TryGetArrayField(TEXT("outputs"), OutputsArray) && OutputsArray->Num() > 0)
	{
		ResultNode = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(EntryNode);
		if (ResultNode)
		{
			for (const TSharedPtr<FJsonValue>& OutputVal : *OutputsArray)
			{
				const TSharedPtr<FJsonObject>* OutputObj = nullptr;
				if (!OutputVal.IsValid() || !OutputVal->TryGetObject(OutputObj)) continue;

				FString ParamName, ParamType;
				if (!(*OutputObj)->TryGetStringField(TEXT("name"), ParamName)) continue;
				if (!(*OutputObj)->TryGetStringField(TEXT("type"), ParamType)) continue;

				ClaireonBlueprintHelpers::FParseVariableTypeResult Parse = ClaireonBlueprintHelpers::ParseVariableTypeChecked(ParamType);
				if (!Parse.bSucceeded)
				{
					FToolResult Err = MakeErrorResult(FString::Printf(
						TEXT("Failed to parse output param '%s' type: %s"), *ParamName, *Parse.Error));
					TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
					TSharedPtr<FJsonObject> ParserErrObj = MakeShared<FJsonObject>();
					ParserErrObj->SetStringField(TEXT("input"), ParamType);
					ParserErrObj->SetStringField(TEXT("message"), Parse.Error);
					DataObj->SetObjectField(TEXT("type_parser_error"), ParserErrObj);
					Err.Data = DataObj;
					return Err;
				}
				ResultNode->CreateUserDefinedPin(FName(*ParamName), Parse.PinType, EGPD_Input);
			}
		}
	}

	// === 9. Reconstruct + mark structurally modified ===
	EntryNode->ReconstructNode();
	if (ResultNode) { ResultNode->ReconstructNode(); }
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	// === 10. Switch session cursor onto the new graph (mirrors AddFunctionOverride native path) ===
	const FString PreviousGraphName = Data->Cursor.GraphName;
	Data->Cursor.PushHistory(PreviousGraphName);
	Data->Graph = NewGraph;
	Data->Cursor.GraphName = NewGraph->GetName();
	Data->Cursor.FocusedNodeGuid = EntryNode->NodeGuid;
	for (UEdGraphPin* Pin : EntryNode->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			Data->Cursor.FocusedPinName = Pin->PinName;
			Data->Cursor.FocusedPinDirection = Pin->Direction;
			break;
		}
	}
	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Created function '%s'. Session graph switched to '%s'."),
		*FunctionName, *NewGraph->GetName());

	// === 11. Track affected nodes for response_mode="changed" ===
	Data->LastOperationAffectedNodes.Add(EntryNode->NodeGuid);
	if (ResultNode) { Data->LastOperationAffectedNodes.Add(ResultNode->NodeGuid); }

	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
