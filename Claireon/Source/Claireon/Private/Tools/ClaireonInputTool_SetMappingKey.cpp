// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_SetMappingKey.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "InputMappingContext.h"
#include "EnhancedActionKeyMapping.h"
#include "InputCoreTypes.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_SetMappingKey::GetOperation() const { return TEXT("set_mapping_key"); }

FString ClaireonInputTool_SetMappingKey::GetDescription() const
{
	return TEXT("Change the Key on the mapping at the given index.");
}

TSharedPtr<FJsonObject> ClaireonInputTool_SetMappingKey::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("index"), TEXT("Mapping index."), true);
	Builder.AddString(TEXT("key"), TEXT("New key name."), true);
	return Builder.Build();
}

FToolResult ClaireonInputTool_SetMappingKey::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FInputEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	UInputMappingContext* IMC = RequireMappingContext(Data, Error);
	if (!IMC)
	{
		return MakeErrorResult(Error);
	}

	int32 Index = -1;
	if (!Arguments->TryGetNumberField(TEXT("index"), Index))
	{
		return MakeErrorResult(TEXT("'set_mapping_key' requires 'index'"));
	}

	const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();
	if (Index < 0 || Index >= Mappings.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Mapping index %d out of range (0-%d)"), Index, Mappings.Num() - 1));
	}

	FString KeyName;
	if (!Arguments->TryGetStringField(TEXT("key"), KeyName) || KeyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'set_mapping_key' requires 'key'"));
	}

	FKey NewKey;
	if (!ClaireonEnhancedInputHelpers::ResolveKey(KeyName, NewKey, Error))
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Mapping Key")));
	IMC->Modify();

	IMC->GetMapping(Index).Key = NewKey;
	ClaireonEnhancedInputHelpers::NotifyMappingContextModified(IMC);

	Data->LastOperationStatus = FString::Printf(TEXT("set_mapping_key -- [%d].Key = %s"), Index, *KeyName);
	return BuildStateResponse(SessionId, Data);
}
