// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonEQSTool_RemoveOption.h"
#include "Tools/FToolSchemaBuilder.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonEQSTool_RemoveOption::GetOperation() const { return TEXT("remove_option"); }

FString ClaireonEQSTool_RemoveOption::GetDescription() const
{
	return TEXT("Remove an option from the EQS query by index within an open editing session. "
				"Requires session_id from eqs.open; the edit is transactional and only persists after "
				"save. The option's generator and all its tests are removed together as a single "
				"unit.");
}

TSharedPtr<FJsonObject> ClaireonEQSTool_RemoveOption::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("option_index"), TEXT("Zero-based index of the option to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonEQSTool_RemoveOption::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FEQSEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	int32 OptionIndex = -1;
	if (!Arguments->TryGetNumberField(TEXT("option_index"), OptionIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: option_index"));
	}

	UEnvQuery* Query = Data->Query.Get();

	if (OptionIndex < 0 || OptionIndex >= Query->GetOptions().Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Option index %d out of range (0-%d)"), OptionIndex, Query->GetOptions().Num() - 1));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove EQS Option")));
	Query->Modify();

	GetOptionsMutable(Query).RemoveAt(OptionIndex);
	Query->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("remove_option - Removed option %d"), OptionIndex);
	return BuildStateResponse(SessionId, Data);
}
