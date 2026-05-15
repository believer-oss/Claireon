// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonEQSTool_ReorderTests.h"
#include "Tools/FToolSchemaBuilder.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonEQSTool_ReorderTests::GetName() const
{
	return TEXT("claireon.eqs_reorder_tests");
}

FString ClaireonEQSTool_ReorderTests::GetDescription() const
{
	return TEXT("Move a test within an EQS option's test array to a new index.");
}

TSharedPtr<FJsonObject> ClaireonEQSTool_ReorderTests::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("option_index"), TEXT("Zero-based index of the option."), true);
	Builder.AddInteger(TEXT("test_index"), TEXT("Zero-based current index of the test to move."), true);
	Builder.AddInteger(TEXT("new_index"), TEXT("Zero-based destination index for the test."), true);
	return Builder.Build();
}

FToolResult ClaireonEQSTool_ReorderTests::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	int32 NewIndex = -1;
	if (!Arguments->TryGetNumberField(TEXT("new_index"), NewIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: new_index"));
	}

	UEnvQuery* Query = Data->Query.Get();

	if (OptionIndex < 0 || OptionIndex >= Query->GetOptions().Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Option index %d out of range"), OptionIndex));
	}

	UEnvQueryOption* Option = GetOptionsMutable(Query)[OptionIndex];

	if (TestIndex < 0 || TestIndex >= Option->Tests.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Test index %d out of range"), TestIndex));
	}

	if (NewIndex < 0 || NewIndex >= Option->Tests.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("New index %d out of range"), NewIndex));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Reorder EQS Tests")));
	Query->Modify();

	UEnvQueryTest* TestToMove = Option->Tests[TestIndex];
	Option->Tests.RemoveAt(TestIndex);
	Option->Tests.Insert(TestToMove, NewIndex);
	Query->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("reorder_tests - Moved test %d to index %d in option %d"),
		TestIndex, NewIndex, OptionIndex);
	return BuildStateResponse(SessionId, Data);
}
