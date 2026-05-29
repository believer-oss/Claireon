// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_AddParameter.h"
#include "Tools/ClaireonNiagaraHelpers.h"
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

	FNiagaraTypeDefinition TypeDef;
	FString TypeError;
	if (!ClaireonNiagaraHelpers::ResolveUserParameterTypeDef(TypeStr, TypeDef, TypeError))
	{
		return MakeErrorResult(TypeError);
	}

	UNiagaraSystem* System = Data->System.Get();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Niagara Parameter")));

	FString NormalizedName;
	FString OpError;
	if (!ClaireonNiagaraHelpers::AddOrUpdateUserParameter(System, Name, TypeDef, NormalizedName, OpError))
	{
		Transaction.Cancel();
		return MakeErrorResult(OpError);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("add_parameter -> Added '%s' (%s)"),
		*NormalizedName, *TypeStr);
	return BuildStateResponse(SessionId, Data);
}
