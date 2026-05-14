// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialTool_Compile.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/Material.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialTool_Compile::GetOperation() const { return TEXT("compile"); }

FString ClaireonMaterialTool_Compile::GetDescription() const
{
	return TEXT("Recompile the material shader map. Optionally wait for compilation to finish.");
}

TSharedPtr<FJsonObject> ClaireonMaterialTool_Compile::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddBoolean(TEXT("wait_for_compile"), TEXT("If true, block until shader compilation finishes."));
	return Builder.Build();
}

FToolResult ClaireonMaterialTool_Compile::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FMaterialEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	bool bWaitForCompile = false;
	Arguments->TryGetBoolField(TEXT("wait_for_compile"), bWaitForCompile);

	UMaterial* Material = Data->Material.Get();
	FString CompileErr;
	const bool bOk = ClaireonMaterialHelpers::CompileMaterial(Material, bWaitForCompile, CompileErr);
	if (!bOk)
	{
		Data->LastOperationStatus = FString::Printf(TEXT("compile: errors (%s)"), *CompileErr);
		return MakeErrorResult(CompileErr);
	}

	Data->LastOperationStatus = TEXT("compile: ok");
	return BuildStateResponse(SessionId, Data);
}
