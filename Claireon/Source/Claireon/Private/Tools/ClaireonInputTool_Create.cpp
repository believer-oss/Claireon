// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_Create.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSafeExec.h"
#include "ClaireonSessionManager.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "FileHelpers.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_Create::GetName() const
{
	return TEXT("claireon.input_create");
}

FString ClaireonInputTool_Create::GetDescription() const
{
	return TEXT("Create a new Input Action or Input Mapping Context asset and open an edit session.");
}

TSharedPtr<FJsonObject> ClaireonInputTool_Create::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Destination object path (e.g. /Game/Input/IA_Jump)."), true);
	Builder.AddEnum(TEXT("asset_type"), TEXT("Asset type to create."),
		{TEXT("input_action"), TEXT("ia"), TEXT("mapping_context"), TEXT("imc")}, true);
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("Return only a brief status."));
	return Builder.Build();
}

FToolResult ClaireonInputTool_Create::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'create' requires 'asset_path'"));
	}

	FString AssetTypeStr;
	if (!Arguments->TryGetStringField(TEXT("asset_type"), AssetTypeStr) || AssetTypeStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("'create' requires 'asset_type' ('input_action' or 'mapping_context')"));
	}

	const bool bIsIA = (AssetTypeStr == TEXT("input_action") || AssetTypeStr == TEXT("ia"));
	const bool bIsIMC = (AssetTypeStr == TEXT("mapping_context") || AssetTypeStr == TEXT("imc"));
	if (!bIsIA && !bIsIMC)
	{
		return MakeErrorResult(FString::Printf(TEXT("Unknown asset_type: %s (expected: 'input_action' or 'mapping_context')"), *AssetTypeStr));
	}

	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	FSoftObjectPath SoftPath(AssetPath);
	if (SoftPath.TryLoad())
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset already exists at path: %s. Use 'open' instead."), *AssetPath));
	}

	const FString& PackagePath = ResolveResult.ResolvedPath.PackagePath;
	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create package: %s"), *PackagePath));
	}

	UObject* NewAsset = nullptr;
	EInputAssetType NewAssetType;
	FString ShortName = FPackageName::GetShortName(PackagePath);

	if (bIsIA)
	{
		UInputAction* IA = NewObject<UInputAction>(Package, *ShortName, RF_Public | RF_Standalone);
		if (!IA)
		{
			return MakeErrorResult(TEXT("Failed to create Input Action"));
		}
		NewAsset = IA;
		NewAssetType = EInputAssetType::InputAction;
	}
	else
	{
		UInputMappingContext* IMC = NewObject<UInputMappingContext>(Package, *ShortName, RF_Public | RF_Standalone);
		if (!IMC)
		{
			return MakeErrorResult(TEXT("Failed to create Input Mapping Context"));
		}
		NewAsset = IMC;
		NewAssetType = EInputAssetType::MappingContext;
	}

	Package->SetDirtyFlag(true);
	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	if (!ClaireonSafeExec::DidLastExecutionCrash())
	{
		UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);
	}

	FAssetRegistryModule::AssetCreated(NewAsset);

	EnsureDelegateRegistered();

	const FString ResolvedAssetPath = NewAsset->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, InputSessionToolName);
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		return MakeErrorResult(FString::Printf(TEXT("Asset is locked by %s session %s"), *Blocker.ToolName, *Blocker.SessionId));
	}
	const FString SessionId = OpenResult.SessionId;

	FInputEditToolData NewData;
	NewData.AssetType = NewAssetType;
	NewData.LastOperationStatus = TEXT("Asset created and session opened");
	if (NewAssetType == EInputAssetType::InputAction)
	{
		NewData.InputAction = Cast<UInputAction>(NewAsset);
	}
	else
	{
		NewData.MappingContext = Cast<UInputMappingContext>(NewAsset);
	}

	if (Arguments->HasField(TEXT("suppress_output")))
	{
		NewData.bSuppressOutput = Arguments->GetBoolField(TEXT("suppress_output"));
	}

	ToolData.Add(SessionId, MoveTemp(NewData));

	FInputEditToolData* Data = ToolData.Find(SessionId);
	return BuildStateResponse(SessionId, Data);
}
