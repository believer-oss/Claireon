// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeSplineTool_Save.h"
#include "Tools/ClaireonAssetUtils.h"
#include "Tools/FToolSchemaBuilder.h"
#include "LandscapeProxy.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeSplineTool_Save::GetOperation() const { return TEXT("spline_save"); }

FString ClaireonLandscapeSplineTool_Save::GetDescription() const
{
    return TEXT("Save the landscape actor's package to disk for the current spline session. Session-mode tool: open via landscape_spline_spline_open first.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeSplineTool_Save::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonLandscapeSplineTool_Save::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FLandscapeSplineEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	ALandscapeProxy* Proxy = Data->LandscapeProxy.Get();
	if (!IsValid(Proxy))
	{
		return MakeErrorResult(TEXT("Landscape proxy no longer valid"));
	}

	UPackage* Package = Proxy->GetOutermost();
	if (!IsValid(Package))
	{
		return MakeErrorResult(TEXT("Failed to get landscape package"));
	}

	// Through the shared resolver, not a hardcoded extension. This package is the LEVEL's
	// package -- GetOutermost() on a landscape proxy, foliage actor or spline outside World
	// Partition returns the world -- so writing it at .uasset creates a shadow package that
	// then wins every later lookup by name and orphans the .umap.
	FString PackageFilename;
	FString ResolveError;
	if (!ClaireonAssetUtils::ResolvePackageSaveFilename(Package, PackageFilename, ResolveError))
	{
		return MakeErrorResult(ResolveError);
	}
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	const FSavePackageResultStruct SaveResult = UPackage::Save(Package, nullptr, *PackageFilename, SaveArgs);

	if (SaveResult.Result != ESavePackageResult::Success)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to save package: %s"), *PackageFilename));
	}

	Data->LastOperationStatus = TEXT("Landscape saved");

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("save"));
	return MakeSuccessResult(ResultData, Data->LastOperationStatus);
}
