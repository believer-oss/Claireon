// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonFoliageTool_Save.h"
#include "Tools/FToolSchemaBuilder.h"
#include "InstancedFoliageActor.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonFoliageTool_Save::GetName() const
{
	return TEXT("claireon.foliage_save");
}

FString ClaireonFoliageTool_Save::GetDescription() const
{
	return TEXT("Save the foliage actor's package to disk.");
}

TSharedPtr<FJsonObject> ClaireonFoliageTool_Save::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonFoliageTool_Save::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FFoliageEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	AInstancedFoliageActor* IFA = Data->FoliageActor.Get();
	UPackage* Package = IFA->GetOutermost();
	if (!Package)
	{
		return MakeErrorResult(TEXT("Failed to get foliage package"));
	}

	const FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	const FSavePackageResultStruct SaveResult = UPackage::Save(Package, nullptr, *PackageFilename, SaveArgs);

	if (SaveResult.Result != ESavePackageResult::Success)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to save foliage package: %s"), *PackageFilename));
	}

	Data->LastOperationStatus = TEXT("Foliage saved");

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("save"));
	return MakeSuccessResult(ResultData, Data->LastOperationStatus);
}
