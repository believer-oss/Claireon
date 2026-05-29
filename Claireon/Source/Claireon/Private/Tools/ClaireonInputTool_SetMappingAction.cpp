// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_SetMappingAction.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "ClaireonPathResolver.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "EnhancedActionKeyMapping.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_SetMappingAction::GetOperation() const { return TEXT("set_mapping_action"); }

FString ClaireonInputTool_SetMappingAction::GetDescription() const
{
    return TEXT("Set the Action on the mapping at the given index in the Input Mapping Context. Session-mode tool: open via input_open first.");
}

TSharedPtr<FJsonObject> ClaireonInputTool_SetMappingAction::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("index"), TEXT("Mapping index."), true);
	Builder.AddString(TEXT("action_path"), TEXT("Object path to the new Input Action."), true);
	return Builder.Build();
}

FToolResult ClaireonInputTool_SetMappingAction::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
		return MakeErrorResult(TEXT("'set_mapping_action' requires 'index'"));
	}

	const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();
	if (Index < 0 || Index >= Mappings.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Mapping index %d out of range (0-%d)"), Index, Mappings.Num() - 1));
	}

	FString ActionPath;
	if (!Arguments->TryGetStringField(TEXT("action_path"), ActionPath) || ActionPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'set_mapping_action' requires 'action_path'"));
	}

	auto ResolveResult = ClaireonPathResolver::Resolve(ActionPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	UInputAction* Action = LoadObject<UInputAction>(nullptr, *ResolveResult.ResolvedPath.Path);
	if (!Action)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Input Action at: %s"), *ActionPath));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Mapping Action")));
	IMC->Modify();

	IMC->GetMapping(Index).Action = Action;
	ClaireonEnhancedInputHelpers::NotifyMappingContextModified(IMC);

	Data->LastOperationStatus = FString::Printf(TEXT("set_mapping_action -- [%d].Action = %s"), Index, *Action->GetName());
	return BuildStateResponse(SessionId, Data);
}
