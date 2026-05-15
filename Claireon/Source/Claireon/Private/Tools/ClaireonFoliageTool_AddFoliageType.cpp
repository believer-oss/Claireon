// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonFoliageTool_AddFoliageType.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonPathResolver.h"
#include "InstancedFoliageActor.h"
#include "FoliageType.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonFoliageTool_AddFoliageType::GetName() const
{
	return TEXT("claireon.foliage_add_foliage_type");
}

FString ClaireonFoliageTool_AddFoliageType::GetDescription() const
{
	return TEXT("Register a UFoliageType asset on the session's foliage actor.");
}

TSharedPtr<FJsonObject> ClaireonFoliageTool_AddFoliageType::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("asset_path"), TEXT("Path to the UFoliageType asset to register."), true);
	return Builder.Build();
}

FToolResult ClaireonFoliageTool_AddFoliageType::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
#if WITH_EDITOR
	FString SessionId;
	FFoliageEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	// Normalize caller-provided path through the central resolver before LoadObject.
	const auto AssetPathResolve = ClaireonPathResolver::Resolve(AssetPath);
	if (!AssetPathResolve.bSuccess)
	{
		return MakeErrorResult(AssetPathResolve.Error);
	}
	AssetPath = AssetPathResolve.ResolvedPath.Path;

	UFoliageType* FoliageType = LoadObject<UFoliageType>(nullptr, *AssetPath);
	if (!FoliageType)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Failed to load UFoliageType at '%s'. Verify the path points to a FoliageType asset."), *AssetPath));
	}

	AInstancedFoliageActor* IFA = Data->FoliageActor.Get();
	IFA->AddFoliageInfo(FoliageType);

	Data->LastOperationStatus = FString::Printf(TEXT("Added foliage type: %s"), *FoliageType->GetName());
	return BuildStateResponse(SessionId, Data);
#else
	return MakeErrorResult(TEXT("Foliage editing requires WITH_EDITOR"));
#endif
}
