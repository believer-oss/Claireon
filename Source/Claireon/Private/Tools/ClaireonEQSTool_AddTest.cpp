// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonEQSTool_AddTest.h"
#include "Tools/FToolSchemaBuilder.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonEQSTool_AddTest::GetOperation() const { return TEXT("add_test"); }

FString ClaireonEQSTool_AddTest::GetDescription() const
{
	return TEXT("Add a new scoring or filtering test to an EQS option within an open editing "
				"session. Requires session_id from eqs.open; the edit is transactional and only "
				"persists after save. Tests are evaluated in the order they appear on the option; use "
				"reorder_tests to change their priority.");
}

TSharedPtr<FJsonObject> ClaireonEQSTool_AddTest::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("option_index"), TEXT("Zero-based index of the option to add the test to."), true);
	Builder.AddString(TEXT("test_class"), TEXT("Test class name (e.g. 'Distance' or 'EnvQueryTest_Distance')."), true);
	return Builder.Build();
}

FToolResult ClaireonEQSTool_AddTest::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString TestClassName;
	if (!Arguments->TryGetStringField(TEXT("test_class"), TestClassName) || TestClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: test_class"));
	}

	UEnvQuery* Query = Data->Query.Get();

	if (OptionIndex < 0 || OptionIndex >= Query->GetOptions().Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Option index %d out of range"), OptionIndex));
	}

	UClass* TestClass = ResolveEQSClass(TestClassName, UEnvQueryTest::StaticClass(), TEXT("EnvQueryTest_"), Error);
	if (!TestClass)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add EQS Test")));
	Query->Modify();

	UEnvQueryOption* Option = GetOptionsMutable(Query)[OptionIndex];
	UEnvQueryTest* NewTest = NewObject<UEnvQueryTest>(Option, TestClass);
	Option->Tests.Add(NewTest);
	Query->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("add_test - Added %s to option %d (test index %d)"),
		*TestClassName, OptionIndex, Option->Tests.Num() - 1);
	return BuildStateResponse(SessionId, Data);
}
