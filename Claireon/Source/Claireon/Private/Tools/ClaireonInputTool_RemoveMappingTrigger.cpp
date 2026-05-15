// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_RemoveMappingTrigger.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "InputMappingContext.h"
#include "EnhancedActionKeyMapping.h"
#include "InputTriggers.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_RemoveMappingTrigger::GetName() const
{
	return TEXT("claireon.input_remove_mapping_trigger");
}

FString ClaireonInputTool_RemoveMappingTrigger::GetDescription() const
{
	return TEXT("Remove a per-mapping trigger at the given index from a mapping.");
}

TSharedPtr<FJsonObject> ClaireonInputTool_RemoveMappingTrigger::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("mapping_index"), TEXT("Mapping index on the IMC."), true);
	Builder.AddInteger(TEXT("trigger_index"), TEXT("Trigger index within the mapping."), true);
	return Builder.Build();
}

FToolResult ClaireonInputTool_RemoveMappingTrigger::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
		return MakeErrorResult(TEXT("'remove_mapping_trigger' requires 'mapping_index'"));
	}

	const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();
	if (MappingIndex < 0 || MappingIndex >= Mappings.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Mapping index %d out of range (0-%d)"), MappingIndex, Mappings.Num() - 1));
	}

	int32 TriggerIndex = -1;
	if (!Arguments->TryGetNumberField(TEXT("trigger_index"), TriggerIndex))
	{
		return MakeErrorResult(TEXT("'remove_mapping_trigger' requires 'trigger_index'"));
	}

	FEnhancedActionKeyMapping& Mapping = IMC->GetMapping(MappingIndex);
	if (TriggerIndex < 0 || TriggerIndex >= Mapping.Triggers.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Trigger index %d out of range (0-%d) on mapping %d"),
			TriggerIndex, Mapping.Triggers.Num() - 1, MappingIndex));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Mapping Trigger")));
	IMC->Modify();

	FString RemovedName = Mapping.Triggers[TriggerIndex] ? Mapping.Triggers[TriggerIndex]->GetClass()->GetName() : TEXT("(null)");
	Mapping.Triggers.RemoveAt(TriggerIndex);
	ClaireonEnhancedInputHelpers::NotifyMappingContextModified(IMC);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_mapping_trigger -- Removed %s from [%d].Triggers[%d]"), *RemovedName, MappingIndex, TriggerIndex);
	return BuildStateResponse(SessionId, Data);
}
