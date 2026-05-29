// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonEQSTool_AddOption.h"
#include "Tools/FToolSchemaBuilder.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonEQSTool_AddOption::GetOperation() const { return TEXT("add_option"); }

FString ClaireonEQSTool_AddOption::GetDescription() const
{
	return TEXT("Add a new option to the EQS query within an open editing session, creating a "
				"generator of the specified class. Requires session_id from eqs.open; the edit is "
				"transactional and only persists after save. Options are evaluated in order at "
				"runtime and produce candidate items.");
}

TSharedPtr<FJsonObject> ClaireonEQSTool_AddOption::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("generator_class"), TEXT("Generator class name (e.g. 'SimpleGrid' or 'EnvQueryGenerator_SimpleGrid')."), true);
	return Builder.Build();
}

FToolResult ClaireonEQSTool_AddOption::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FEQSEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString GeneratorClassName;
	if (!Arguments->TryGetStringField(TEXT("generator_class"), GeneratorClassName) || GeneratorClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: generator_class"));
	}

	UClass* GeneratorClass = ResolveEQSClass(GeneratorClassName, UEnvQueryGenerator::StaticClass(), TEXT("EnvQueryGenerator_"), Error);
	if (!GeneratorClass)
	{
		return MakeErrorResult(Error);
	}

	UEnvQuery* Query = Data->Query.Get();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add EQS Option")));
	Query->Modify();

	UEnvQueryOption* NewOption = NewObject<UEnvQueryOption>(Query);
	NewOption->Generator = NewObject<UEnvQueryGenerator>(NewOption, GeneratorClass);

	GetOptionsMutable(Query).Add(NewOption);
	Query->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("add_option - Added option with generator %s (index %d)"),
		*GeneratorClassName, Query->GetOptions().Num() - 1);
	return BuildStateResponse(SessionId, Data);
}
