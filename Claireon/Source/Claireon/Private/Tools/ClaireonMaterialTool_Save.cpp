// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialTool_Save.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/Material.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialTool_Save::GetOperation() const { return TEXT("save"); }

FString ClaireonMaterialTool_Save::GetDescription() const
{
    return TEXT("Save the material asset of the current session to disk so in-session edits are persisted.");
}

TSharedPtr<FJsonObject> ClaireonMaterialTool_Save::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonMaterialTool_Save::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FMaterialEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	UMaterial* Material = Data->Material.Get();
	FString SaveErr;
	if (!ClaireonMaterialHelpers::SaveMaterialAsset(Material, SaveErr))
	{
		Data->LastOperationStatus = TEXT("save: failed");
		return MakeErrorResult(SaveErr);
	}
	Data->LastOperationStatus = FString::Printf(TEXT("save: %s"), *Material->GetPathName());
	return BuildStateResponse(SessionId, Data);
}
