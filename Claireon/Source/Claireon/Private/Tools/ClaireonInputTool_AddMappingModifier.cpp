// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_AddMappingModifier.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "InputMappingContext.h"
#include "EnhancedActionKeyMapping.h"
#include "InputModifiers.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_AddMappingModifier::GetOperation() const { return TEXT("add_mapping_modifier"); }

FString ClaireonInputTool_AddMappingModifier::GetDescription() const
{
	return TEXT("Add a per-mapping modifier to the mapping at the given index.");
}

TSharedPtr<FJsonObject> ClaireonInputTool_AddMappingModifier::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("mapping_index"), TEXT("Mapping index on the IMC."), true);
	Builder.AddString(TEXT("modifier_class"), TEXT("Modifier class name."), true);
	return Builder.Build();
}

FToolResult ClaireonInputTool_AddMappingModifier::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
		return MakeErrorResult(TEXT("'add_mapping_modifier' requires 'mapping_index'"));
	}

	const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();
	if (MappingIndex < 0 || MappingIndex >= Mappings.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Mapping index %d out of range (0-%d)"), MappingIndex, Mappings.Num() - 1));
	}

	FString ModifierClassName;
	if (!Arguments->TryGetStringField(TEXT("modifier_class"), ModifierClassName) || ModifierClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'add_mapping_modifier' requires 'modifier_class'"));
	}

	UClass* ModifierClass = ClaireonEnhancedInputHelpers::ResolveModifierClass(ModifierClassName, Error);
	if (!ModifierClass)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Mapping Modifier")));
	IMC->Modify();

	UInputModifier* NewModifier = ClaireonEnhancedInputHelpers::CreateModifier(IMC, ModifierClass);
	if (!NewModifier)
	{
		return MakeErrorResult(TEXT("Failed to create modifier instance"));
	}

	const int32 NewIndex = IMC->GetMapping(MappingIndex).Modifiers.Add(NewModifier);
	ClaireonEnhancedInputHelpers::NotifyMappingContextModified(IMC);

	Data->LastOperationStatus = FString::Printf(TEXT("add_mapping_modifier -- [%d].Modifiers[%d] = %s"), MappingIndex, NewIndex, *ModifierClass->GetName());
	return BuildStateResponse(SessionId, Data);
}
