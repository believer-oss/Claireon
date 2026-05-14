// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_RemoveParameter.h"
#include "Tools/FToolSchemaBuilder.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_RemoveParameter::GetOperation() const { return TEXT("remove_parameter"); }

FString ClaireonNiagaraTool_RemoveParameter::GetDescription() const
{
	return TEXT("Remove an exposed User.* parameter from the Niagara System.");
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

	FString FullName = Name;
	if (!FullName.StartsWith(TEXT("User.")))
	{
		FullName = TEXT("User.") + FullName;
	}

	UNiagaraSystem* System = Data->System.Get();

	TArray<FNiagaraVariable> UserParams;
	System->GetExposedParameters().GetUserParameters(UserParams);

	const FNiagaraVariable* FoundVar = nullptr;
	for (const FNiagaraVariable& Param : UserParams)
	{
		if (Param.GetName().ToString().Equals(FullName, ESearchCase::IgnoreCase))
		{
			FoundVar = &Param;
			break;
		}
	}

	if (!FoundVar)
	{
		return MakeErrorResult(FString::Printf(TEXT("Parameter '%s' not found in exposed parameters"), *FullName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Niagara Parameter")));
	System->Modify();

	System->GetExposedParameters().RemoveParameter(*FoundVar);

	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("remove_parameter -> Removed '%s'"), *FullName);
	return BuildStateResponse(SessionId, Data);
}
