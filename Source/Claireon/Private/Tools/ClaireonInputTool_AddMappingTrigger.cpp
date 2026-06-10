// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_AddMappingTrigger.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "InputMappingContext.h"
#include "EnhancedActionKeyMapping.h"
#include "InputTriggers.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_AddMappingTrigger::GetOperation() const { return TEXT("add_mapping_trigger"); }

FString ClaireonInputTool_AddMappingTrigger::GetDescription() const
{
    return TEXT("Add a per-mapping trigger to the mapping at the given index. Session-mode tool: open via input_open first.");
}

TSharedPtr<FJsonObject> ClaireonInputTool_AddMappingTrigger::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("mapping_index"), TEXT("Mapping index on the IMC."), true);
	Builder.AddString(TEXT("trigger_class"), TEXT("Trigger class name."), true);
	return Builder.Build();
}

FToolResult ClaireonInputTool_AddMappingTrigger::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	int32 MappingIndex = -1;
	if (!Arguments->TryGetNumberField(TEXT("mapping_index"), MappingIndex))
	{
		return MakeErrorResult(TEXT("'add_mapping_trigger' requires 'mapping_index'"));
	}

	const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();
	if (MappingIndex < 0 || MappingIndex >= Mappings.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Mapping index %d out of range (0-%d)"), MappingIndex, Mappings.Num() - 1));
	}

	FString TriggerClassName;
	if (!Arguments->TryGetStringField(TEXT("trigger_class"), TriggerClassName) || TriggerClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'add_mapping_trigger' requires 'trigger_class'"));
	}

	UClass* TriggerClass = ClaireonEnhancedInputHelpers::ResolveTriggerClass(TriggerClassName, Error);
	if (!TriggerClass)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Mapping Trigger")));
	IMC->Modify();

	UInputTrigger* NewTrigger = ClaireonEnhancedInputHelpers::CreateTrigger(IMC, TriggerClass);
	if (!NewTrigger)
	{
		return MakeErrorResult(TEXT("Failed to create trigger instance"));
	}

	const int32 NewIndex = IMC->GetMapping(MappingIndex).Triggers.Add(NewTrigger);
	ClaireonEnhancedInputHelpers::NotifyMappingContextModified(IMC);

	Data->LastOperationStatus = FString::Printf(TEXT("add_mapping_trigger -- [%d].Triggers[%d] = %s"), MappingIndex, NewIndex, *TriggerClass->GetName());
	return BuildStateResponse(SessionId, Data);
}
