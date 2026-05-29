// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_AddParameter.h"
#include "Tools/FToolSchemaBuilder.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_AddParameter::GetOperation() const { return TEXT("add_parameter"); }

FString ClaireonNiagaraTool_AddParameter::GetDescription() const
{
    return TEXT("Add an exposed User.* parameter to the Niagara System. Session-mode tool: open via niagara_open first.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_AddParameter::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("name"), TEXT("Parameter name (User. prefix added automatically)."), true);
	Builder.AddString(TEXT("type"), TEXT("Parameter type: Float, Vector, Color/LinearColor, Bool, Int."), true);
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_AddParameter::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FNiagaraEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString Name;
	if (!Arguments->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: name"));
	}

	FString TypeStr;
	if (!Arguments->TryGetStringField(TEXT("type"), TypeStr) || TypeStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: type"));
	}

	FString FullName = Name;
	if (!FullName.StartsWith(TEXT("User.")))
	{
		FullName = TEXT("User.") + FullName;
	}

	FNiagaraTypeDefinition TypeDef;
	if (TypeStr.Equals(TEXT("Float"), ESearchCase::IgnoreCase))
	{
		TypeDef = FNiagaraTypeDefinition::GetFloatDef();
	}
	else if (TypeStr.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
	{
		TypeDef = FNiagaraTypeDefinition::GetVec3Def();
	}
	else if (TypeStr.Equals(TEXT("Color"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("LinearColor"), ESearchCase::IgnoreCase))
	{
		TypeDef = FNiagaraTypeDefinition::GetColorDef();
	}
	else if (TypeStr.Equals(TEXT("Bool"), ESearchCase::IgnoreCase))
	{
		TypeDef = FNiagaraTypeDefinition::GetBoolDef();
	}
	else if (TypeStr.Equals(TEXT("Int"), ESearchCase::IgnoreCase))
	{
		TypeDef = FNiagaraTypeDefinition::GetIntDef();
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Unsupported parameter type: '%s'. Valid types: Float, Vector, Color, LinearColor, Bool, Int"), *TypeStr));
	}

	UNiagaraSystem* System = Data->System.Get();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Niagara Parameter")));
	System->Modify();

	FNiagaraVariable Variable(TypeDef, FName(*FullName));
	System->GetExposedParameters().AddParameter(Variable, true);

	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("add_parameter -> Added '%s' (%s)"),
		*FullName, *TypeStr);
	return BuildStateResponse(SessionId, Data);
}
