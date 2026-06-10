// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonEQSTool_SetNodeProperty.h"
#include "Tools/FToolSchemaBuilder.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonEQSTool_SetNodeProperty::GetOperation() const { return TEXT("set_node_property"); }

FString ClaireonEQSTool_SetNodeProperty::GetDescription() const
{
	return TEXT("Set a property on an EQS generator or test node within an open editing session via "
				"reflection (ImportText). target='generator' edits the option's generator; "
				"target='test' requires test_index. Requires session_id from eqs.open; the edit is "
				"transactional and only persists after save.");
}

TSharedPtr<FJsonObject> ClaireonEQSTool_SetNodeProperty::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("option_index"), TEXT("Zero-based index of the option whose node property will be set."), true);
	Builder.AddString(TEXT("target"), TEXT("Which node to modify: 'generator' or 'test'."), true);
	Builder.AddString(TEXT("property_name"), TEXT("Name of the property to set."), true);
	Builder.AddString(TEXT("property_value"), TEXT("New property value (ImportText-compatible string)."), true);
	Builder.AddInteger(TEXT("test_index"), TEXT("Zero-based index of the test within the option (required when target='test')."));
	return Builder.Build();
}

FToolResult ClaireonEQSTool_SetNodeProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString Target;
	if (!Arguments->TryGetStringField(TEXT("target"), Target) || Target.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: target ('generator' or 'test')"));
	}

	FString PropertyName;
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_name"));
	}

	FString PropertyValue;
	if (!Arguments->TryGetStringField(TEXT("property_value"), PropertyValue))
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_value"));
	}

	UEnvQuery* Query = Data->Query.Get();

	if (OptionIndex < 0 || OptionIndex >= Query->GetOptions().Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Option index %d out of range"), OptionIndex));
	}

	UEnvQueryOption* Option = GetOptionsMutable(Query)[OptionIndex];

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set EQS Node Property")));
	Query->Modify();

	if (Target == TEXT("generator"))
	{
		if (!Option->Generator)
		{
			return MakeErrorResult(TEXT("Option has no generator"));
		}
		if (!SetEQSNodeProperty(Option->Generator, PropertyName, PropertyValue, Error))
		{
			return MakeErrorResult(Error);
		}
	}
	else if (Target == TEXT("test"))
	{
		int32 TestIndex = -1;
		if (!Arguments->TryGetNumberField(TEXT("test_index"), TestIndex))
		{
			return MakeErrorResult(TEXT("Missing required parameter: test_index (required when target='test')"));
		}

		if (TestIndex < 0 || TestIndex >= Option->Tests.Num())
		{
			return MakeErrorResult(FString::Printf(TEXT("Test index %d out of range for option %d"), TestIndex, OptionIndex));
		}

		if (!SetEQSNodeProperty(Option->Tests[TestIndex], PropertyName, PropertyValue, Error))
		{
			return MakeErrorResult(Error);
		}
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid target: '%s'. Use 'generator' or 'test'."), *Target));
	}

	Query->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_node_property - Set '%s' = '%s' on %s (option %d)"),
		*PropertyName, *PropertyValue, *Target, OptionIndex);
	return BuildStateResponse(SessionId, Data);
}
