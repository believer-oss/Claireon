// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonEQSTool_SetGenerator.h"
#include "Tools/FToolSchemaBuilder.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonEQSTool_SetGenerator::GetOperation() const { return TEXT("set_generator"); }

FString ClaireonEQSTool_SetGenerator::GetDescription() const
{
	return TEXT("Set the generator of an existing EQS option within an open editing session, "
				"replacing it with a new generator of the specified class. Requires session_id from "
				"eqs.open; the edit is transactional and only persists after save. Existing tests on "
				"the option are preserved across the swap.");
}

TSharedPtr<FJsonObject> ClaireonEQSTool_SetGenerator::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("option_index"), TEXT("Zero-based index of the option whose generator will be replaced."), true);
	Builder.AddString(TEXT("generator_class"), TEXT("Generator class name (e.g. 'SimpleGrid' or 'EnvQueryGenerator_SimpleGrid')."), true);
	return Builder.Build();
}

FToolResult ClaireonEQSTool_SetGenerator::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString GeneratorClassName;
	if (!Arguments->TryGetStringField(TEXT("generator_class"), GeneratorClassName) || GeneratorClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: generator_class"));
	}

	UEnvQuery* Query = Data->Query.Get();

	if (OptionIndex < 0 || OptionIndex >= Query->GetOptions().Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Option index %d out of range"), OptionIndex));
	}

	UClass* GeneratorClass = ResolveEQSClass(GeneratorClassName, UEnvQueryGenerator::StaticClass(), TEXT("EnvQueryGenerator_"), Error);
	if (!GeneratorClass)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set EQS Generator")));
	Query->Modify();

	UEnvQueryOption* Option = GetOptionsMutable(Query)[OptionIndex];
	Option->Generator = NewObject<UEnvQueryGenerator>(Option, GeneratorClass);
	Query->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_generator - Set option %d generator to %s"),
		OptionIndex, *GeneratorClassName);
	return BuildStateResponse(SessionId, Data);
}
