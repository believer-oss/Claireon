// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_AddMapping.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "ClaireonPathResolver.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "EnhancedActionKeyMapping.h"
#include "InputCoreTypes.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_AddMapping::GetOperation() const { return TEXT("add_mapping"); }

FString ClaireonInputTool_AddMapping::GetDescription() const
{
    return TEXT("Add a new key-to-action mapping to the Input Mapping Context. Session-mode tool: open via input_open first.");
}

TSharedPtr<FJsonObject> ClaireonInputTool_AddMapping::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("action_path"), TEXT("Object path to the Input Action."), true);
	Builder.AddString(TEXT("key"), TEXT("Key name (e.g. SpaceBar, Gamepad_FaceButton_Bottom)."), true);
	return Builder.Build();
}

FToolResult ClaireonInputTool_AddMapping::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString ActionPath;
	if (!Arguments->TryGetStringField(TEXT("action_path"), ActionPath) || ActionPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'add_mapping' requires 'action_path'"));
	}

	FString KeyName;
	if (!Arguments->TryGetStringField(TEXT("key"), KeyName) || KeyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'add_mapping' requires 'key'"));
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

	FKey Key;
	if (!ClaireonEnhancedInputHelpers::ResolveKey(KeyName, Key, Error))
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Input Mapping")));
	IMC->Modify();

	IMC->MapKey(Action, Key);
	const int32 NewIndex = IMC->GetMappings().Num() - 1;

	ClaireonEnhancedInputHelpers::NotifyMappingContextModified(IMC);

	Data->LastOperationStatus = FString::Printf(TEXT("add_mapping -- Added %s -> %s at index %d"), *Action->GetName(), *KeyName, NewIndex);
	return BuildStateResponse(SessionId, Data);
}
