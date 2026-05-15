// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeTool_Save.h"
#include "Tools/FToolSchemaBuilder.h"
#include "LandscapeProxy.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeTool_Save::GetName() const
{
	return TEXT("claireon.landscape_save");
}

FString ClaireonLandscapeTool_Save::GetDescription() const
{
	return TEXT("Save the landscape actor's package to disk.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeTool_Save::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonLandscapeTool_Save::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FLandscapeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	ALandscapeProxy* Proxy = Data->LandscapeProxy.Get();
	UPackage* Package = Proxy->GetOutermost();
	if (!Package)
	{
		return MakeErrorResult(TEXT("Failed to get landscape package"));
	}

	const FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	const FSavePackageResultStruct SaveResult = UPackage::Save(Package, nullptr, *PackageFilename, SaveArgs);

	if (SaveResult.Result != ESavePackageResult::Success)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to save landscape package: %s"), *PackageFilename));
	}

	Data->LastOperationStatus = TEXT("Landscape saved");

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("save"));
	ResultData->SetStringField(TEXT("package"), Package->GetName());
	return MakeSuccessResult(ResultData, Data->LastOperationStatus);
}
