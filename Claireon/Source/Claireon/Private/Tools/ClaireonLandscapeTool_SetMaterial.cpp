// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeTool_SetMaterial.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonPathResolver.h"
#include "LandscapeProxy.h"
#include "Materials/MaterialInterface.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeTool_SetMaterial::GetOperation() const { return TEXT("set_material"); }

FString ClaireonLandscapeTool_SetMaterial::GetDescription() const
{
	return TEXT("Assign a UMaterialInterface to the session's landscape.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeTool_SetMaterial::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("material_path"), TEXT("Path to a UMaterialInterface asset."), true);
	return Builder.Build();
}

FToolResult ClaireonLandscapeTool_SetMaterial::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FLandscapeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString MaterialPath;
	if (!Arguments->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: material_path"));
	}

	// Normalize caller-provided path through the central resolver before LoadObject.
	const auto MaterialPathResolve = ClaireonPathResolver::Resolve(MaterialPath);
	if (!MaterialPathResolve.bSuccess)
	{
		return MakeErrorResult(MaterialPathResolve.Error);
	}
	MaterialPath = MaterialPathResolve.ResolvedPath.Path;

	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Material)
	{
		return MakeErrorResult(FString::Printf(TEXT("Material '%s' not found"), *MaterialPath));
	}

	ALandscapeProxy* Proxy = Data->LandscapeProxy.Get();
	Proxy->LandscapeMaterial = Material;
	Proxy->UpdateAllComponentMaterialInstances();

	Data->LastOperationStatus = FString::Printf(TEXT("Set material to %s"), *MaterialPath);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("set_material"));
	ResultData->SetStringField(TEXT("material_path"), MaterialPath);
	return MakeSuccessResult(ResultData, Data->LastOperationStatus);
}
