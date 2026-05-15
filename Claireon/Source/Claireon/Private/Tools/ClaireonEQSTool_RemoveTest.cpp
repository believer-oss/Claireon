// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonEQSTool_RemoveTest.h"
#include "Tools/FToolSchemaBuilder.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonEQSTool_RemoveTest::GetName() const
{
	return TEXT("claireon.eqs_remove_test");
}

FString ClaireonEQSTool_RemoveTest::GetDescription() const
{
	return TEXT("Remove a test from an EQS option by index.");
}

TSharedPtr<FJsonObject> ClaireonEQSTool_RemoveTest::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("option_index"), TEXT("Zero-based index of the option."), true);
	Builder.AddInteger(TEXT("test_index"), TEXT("Zero-based index of the test to remove within the option."), true);
	return Builder.Build();
}

FToolResult ClaireonEQSTool_RemoveTest::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	int32 TestIndex = -1;
	if (!Arguments->TryGetNumberField(TEXT("test_index"), TestIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: test_index"));
	}

	UEnvQuery* Query = Data->Query.Get();

	if (OptionIndex < 0 || OptionIndex >= Query->GetOptions().Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Option index %d out of range"), OptionIndex));
	}

	UEnvQueryOption* Option = GetOptionsMutable(Query)[OptionIndex];

	if (TestIndex < 0 || TestIndex >= Option->Tests.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Test index %d out of range for option %d (0-%d)"), TestIndex, OptionIndex, Option->Tests.Num() - 1));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove EQS Test")));
	Query->Modify();

	Option->Tests.RemoveAt(TestIndex);
	Query->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("remove_test - Removed test %d from option %d"), TestIndex, OptionIndex);
	return BuildStateResponse(SessionId, Data);
}
