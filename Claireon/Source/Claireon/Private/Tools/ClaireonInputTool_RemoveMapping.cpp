// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_RemoveMapping.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "InputMappingContext.h"
#include "EnhancedActionKeyMapping.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_RemoveMapping::GetOperation() const { return TEXT("remove_mapping"); }

FString ClaireonInputTool_RemoveMapping::GetDescription() const
{
    return TEXT("Remove a mapping at the given index from the Input Mapping Context. Session-mode tool: open via input_open first.");
}

TSharedPtr<FJsonObject> ClaireonInputTool_RemoveMapping::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("index"), TEXT("Mapping index to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonInputTool_RemoveMapping::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
		return MakeErrorResult(TEXT("'remove_mapping' requires 'index'"));
	}

	TArray<FEnhancedActionKeyMapping>& Mappings = ClaireonEnhancedInputHelpers::GetMappingsMutable(IMC);
	if (Index < 0 || Index >= Mappings.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Mapping index %d out of range (0-%d)"), Index, Mappings.Num() - 1));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Input Mapping")));
	IMC->Modify();

	Mappings.RemoveAt(Index);
	ClaireonEnhancedInputHelpers::NotifyMappingContextModified(IMC);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_mapping -- Removed mapping at index %d"), Index);
	return BuildStateResponse(SessionId, Data);
}
