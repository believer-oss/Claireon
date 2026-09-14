// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_GetVariableProperties.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h" // kBPCategory
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonBlueprintHelpers.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_FunctionEntry.h"
#include "Dom/JsonObject.h"

FString ClaireonTool_GetVariableProperties::GetCategory() const { return kBPCategory; }
FString ClaireonTool_GetVariableProperties::GetOperation() const { return TEXT("get_variable_properties"); }

TArray<FString> ClaireonTool_GetVariableProperties::GetSearchKeywords() const
{
	return {TEXT("bp"), TEXT("blueprint"), TEXT("variable"), TEXT("properties"), TEXT("read"),
	        TEXT("category"), TEXT("flags"), TEXT("tooltip"), TEXT("replication"), TEXT("repnotify"),
	        TEXT("metadata"), TEXT("transient"), TEXT("audit"), TEXT("roundtrip")};
}

FString ClaireonTool_GetVariableProperties::GetDescription() const
{
	return TEXT("Read back every variable property bp_set_variable_properties can write: category, "
		"effective flags, tooltip, display_name, replication, rep_notify_func, replication_condition, "
		"metadata. Omit variable_name for all; function locals are reported too. Flags come from the "
		"setter's inverse, so flags and clear_flags converge -- compare normalized state, not your "
		"input tokens. Read-only; PIE-safe.");
}



TSharedPtr<FJsonObject> ClaireonTool_GetVariableProperties::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
	Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior bp_open/bp_create; reads the session's in-memory Blueprint, including unsaved edits."), false);
	Builder.AddString(TEXT("variable_name"), TEXT("Single variable to describe. Omit for all."), false);
	return Builder.Build();
}

TSharedPtr<FJsonObject> ClaireonTool_GetVariableProperties::DescribeVariable(const FBPVariableDescription& Var,
	UBlueprint* OwningBlueprint)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	ClaireonBlueprintHelpers::WriteVariableCoreJson(Var, Obj, OwningBlueprint);
	return Obj;
}

IClaireonTool::FToolResult ClaireonTool_GetVariableProperties::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	FString SessionId;
	const bool bHasAsset = Arguments.IsValid() && Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.IsEmpty();
	const bool bHasSession = Arguments.IsValid() && Arguments->TryGetStringField(TEXT("session_id"), SessionId) && !SessionId.IsEmpty();

	if (bHasAsset == bHasSession)
	{
		return MakeErrorResult(bHasAsset
			? TEXT("Supply exactly one of asset_path or session_id, not both.")
			: TEXT("Supply one of asset_path or session_id."));
	}

	// Resolve either by asset path or from the existing session without opening a new session.
	UBlueprint* Blueprint = nullptr;
	if (bHasAsset)
	{
		Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!IsValid(Blueprint))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Failed to load Blueprint at path: %s. Use find_assets to locate valid Blueprint paths."),
				*AssetPath));
		}
	}
	else
	{
		FBlueprintEditToolData* Data = ClaireonBlueprintGraphEditToolBase::FindToolData(SessionId);
		if (!Data)
		{
			return MakeErrorResult(FString::Printf(TEXT("No open session with id '%s'."), *SessionId));
		}
		Blueprint = Data->Blueprint.Get();
		if (!IsValid(Blueprint))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Session '%s' no longer references a valid Blueprint."), *SessionId));
		}
	}

	FString RequestedName;
	const bool bSingle = Arguments->TryGetStringField(TEXT("variable_name"), RequestedName) && !RequestedName.IsEmpty();
	int32 Matched = 0;

	TArray<TSharedPtr<FJsonValue>> VarValues;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (bSingle && !Var.VarName.ToString().Equals(RequestedName, ESearchCase::IgnoreCase))
		{
			continue;
		}
		VarValues.Add(MakeShared<FJsonValueObject>(DescribeVariable(Var, Blueprint)));
		++Matched;
	}

	// Local variables are stored on function entry nodes.
	TArray<TSharedPtr<FJsonValue>> FunctionValues;
	for (UEdGraph* FunctionGraph : Blueprint->FunctionGraphs)
	{
		if (!IsValid(FunctionGraph))
		{
			continue;
		}

		TArray<TSharedPtr<FJsonValue>> LocalValues;
		for (UEdGraphNode* Node : FunctionGraph->Nodes)
		{
			UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node);
			if (!IsValid(Entry))
			{
				continue;
			}
			for (const FBPVariableDescription& Local : Entry->LocalVariables)
			{
				if (bSingle && !Local.VarName.ToString().Equals(RequestedName, ESearchCase::IgnoreCase))
				{
					continue;
				}
				LocalValues.Add(MakeShared<FJsonValueObject>(DescribeVariable(Local, Blueprint)));
				++Matched;
			}
		}

		if (LocalValues.Num() > 0)
		{
			TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
			FuncObj->SetStringField(TEXT("function_name"), FunctionGraph->GetName());
			FuncObj->SetArrayField(TEXT("local_variables"), LocalValues);
			FunctionValues.Add(MakeShared<FJsonValueObject>(FuncObj));
		}
	}

	if (bSingle && Matched == 0)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("No variable named '%s' on this Blueprint (checked member variables and function locals)."),
			*RequestedName));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), Blueprint->GetPathName());
	Data->SetArrayField(TEXT("variables"), VarValues);
	Data->SetArrayField(TEXT("functions"), FunctionValues);

	const FString Summary = bSingle
		? FString::Printf(TEXT("Described variable '%s' (%d match(es))."), *RequestedName, Matched)
		: FString::Printf(TEXT("Described %d member variable(s) and locals across %d function(s)."),
			VarValues.Num(), FunctionValues.Num());

	return MakeSuccessResult(Data, Summary);
}
