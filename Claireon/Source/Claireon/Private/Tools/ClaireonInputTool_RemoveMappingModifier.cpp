// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_RemoveMappingModifier.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "InputMappingContext.h"
#include "EnhancedActionKeyMapping.h"
#include "InputModifiers.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_RemoveMappingModifier::GetOperation() const { return TEXT("remove_mapping_modifier"); }

FString ClaireonInputTool_RemoveMappingModifier::GetDescription() const
{
    return TEXT("Remove a per-mapping modifier at the given index from a mapping. Session-mode tool: open via input_open first.");
}

TSharedPtr<FJsonObject> ClaireonInputTool_RemoveMappingModifier::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("mapping_index"), TEXT("Mapping index on the IMC."), true);
	Builder.AddInteger(TEXT("modifier_index"), TEXT("Modifier index within the mapping."), true);
	return Builder.Build();
}

FToolResult ClaireonInputTool_RemoveMappingModifier::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
		return MakeErrorResult(TEXT("'remove_mapping_modifier' requires 'mapping_index'"));
	}

	const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();
	if (MappingIndex < 0 || MappingIndex >= Mappings.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Mapping index %d out of range (0-%d)"), MappingIndex, Mappings.Num() - 1));
	}

	int32 ModifierIndex = -1;
	if (!Arguments->TryGetNumberField(TEXT("modifier_index"), ModifierIndex))
	{
		return MakeErrorResult(TEXT("'remove_mapping_modifier' requires 'modifier_index'"));
	}

	FEnhancedActionKeyMapping& Mapping = IMC->GetMapping(MappingIndex);
	if (ModifierIndex < 0 || ModifierIndex >= Mapping.Modifiers.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Modifier index %d out of range (0-%d) on mapping %d"),
			ModifierIndex, Mapping.Modifiers.Num() - 1, MappingIndex));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Mapping Modifier")));
	IMC->Modify();

	FString RemovedName = Mapping.Modifiers[ModifierIndex] ? Mapping.Modifiers[ModifierIndex]->GetClass()->GetName() : TEXT("(null)");
	Mapping.Modifiers.RemoveAt(ModifierIndex);
	ClaireonEnhancedInputHelpers::NotifyMappingContextModified(IMC);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_mapping_modifier -- Removed %s from [%d].Modifiers[%d]"), *RemovedName, MappingIndex, ModifierIndex);
	return BuildStateResponse(SessionId, Data);
}
