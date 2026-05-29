// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_RemoveParameter.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_RemoveParameter::GetOperation() const { return TEXT("remove_parameter"); }

FString ClaireonNiagaraTool_RemoveParameter::GetDescription() const
{
    return TEXT("Remove an exposed User.* parameter from the Niagara System. Session-mode tool: open via niagara_open first.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_RemoveParameter::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("name"), TEXT("Parameter name (User. prefix added automatically)."), true);
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_RemoveParameter::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	UNiagaraSystem* System = Data->System.Get();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Niagara Parameter")));

	FString NormalizedName;
	FString OpError;
	if (!ClaireonNiagaraHelpers::RemoveUserParameter(System, Name, NormalizedName, OpError))
	{
		Transaction.Cancel();
		return MakeErrorResult(OpError);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("remove_parameter -> Removed '%s'"), *NormalizedName);
	return BuildStateResponse(SessionId, Data);
}
