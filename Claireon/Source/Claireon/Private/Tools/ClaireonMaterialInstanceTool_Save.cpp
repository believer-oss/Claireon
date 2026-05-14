// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialInstanceTool_Save.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/MaterialInstanceConstant.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialInstanceTool_Save::GetOperation() const { return TEXT("instance_save"); }

FString ClaireonMaterialInstanceTool_Save::GetDescription() const
{
	return TEXT("Save the UMaterialInstanceConstant's package to disk.");
}

TSharedPtr<FJsonObject> ClaireonMaterialInstanceTool_Save::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonMaterialInstanceTool_Save::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FMaterialInstanceEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	UMaterialInstanceConstant* Instance = Data->Instance.Get();
	FString SaveErr;
	if (!ClaireonMaterialHelpers::SaveMaterialInstanceAsset(Instance, SaveErr))
	{
		Data->LastOperationStatus = TEXT("save: failed");
		return MakeErrorResult(SaveErr);
	}
	Data->LastOperationStatus = FString::Printf(TEXT("save: %s"), *Instance->GetPathName());
	return BuildStateResponse(SessionId, Data);
}
