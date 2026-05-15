// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialInstanceTool_Create.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSessionManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"
#include "UObject/SoftObjectPath.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialInstanceTool_Create::GetName() const
{
	return TEXT("claireon.material_instance_create");
}

FString ClaireonMaterialInstanceTool_Create::GetDescription() const
{
	return TEXT("Create a new UMaterialInstanceConstant asset and open a session on it.");
}

TSharedPtr<FJsonObject> ClaireonMaterialInstanceTool_Create::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("package_path"), TEXT("Package path where the new asset will be created (e.g. /Game/Materials)."), true);
	Builder.AddString(TEXT("asset_name"), TEXT("Name for the new asset."), true);
	Builder.AddString(TEXT("parent_path"), TEXT("Optional parent material path. If omitted, instance is created with no parent."));
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("When true, response is brief status only."));
	return Builder.Build();
}

FToolResult ClaireonMaterialInstanceTool_Create::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString PackagePath, AssetName;
	if (!Arguments->TryGetStringField(TEXT("package_path"), PackagePath) || PackagePath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: package_path"));
	}
	if (!Arguments->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_name"));
	}

	const FString FullObjectPath = FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *AssetName, *AssetName);
	FSoftObjectPath SoftPath(FullObjectPath);
	if (SoftPath.TryLoad())
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset already exists at path: %s. Use 'open' instead."), *FullObjectPath));
	}

	FString ParentPath;
	UMaterialInterface* InitialParent = nullptr;
	if (Arguments->TryGetStringField(TEXT("parent_path"), ParentPath) && !ParentPath.IsEmpty())
	{
		FSoftObjectPath ParentSoft(ParentPath);
		InitialParent = Cast<UMaterialInterface>(ParentSoft.TryLoad());
		if (!InitialParent)
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to load parent material '%s'"), *ParentPath));
		}
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
	if (InitialParent)
	{
		Factory->InitialParent = InitialParent;
	}

	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UMaterialInstanceConstant::StaticClass(), Factory);
	UMaterialInstanceConstant* NewInstance = Cast<UMaterialInstanceConstant>(NewAsset);
	if (!NewInstance)
	{
		return MakeErrorResult(TEXT("Failed to create UMaterialInstanceConstant via AssetTools.CreateAsset"));
	}

	EnsureDelegateRegistered();

	const FString ResolvedPath = NewInstance->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedPath, MaterialInstanceSessionToolName);
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		return MakeErrorResult(FString::Printf(TEXT("Asset is locked by %s session %s"), *Blocker.ToolName, *Blocker.SessionId));
	}
	const FString SessionId = OpenResult.SessionId;

	FMaterialInstanceEditToolData NewData;
	NewData.Instance = NewInstance;
	NewData.LastOperationStatus = TEXT("MaterialInstance created and session opened");

	bool bSuppressOutput = false;
	Arguments->TryGetBoolField(TEXT("suppress_output"), bSuppressOutput);
	NewData.bSuppressOutput = bSuppressOutput;

	ToolData.Add(SessionId, MoveTemp(NewData));

	FMaterialInstanceEditToolData* Data = ToolData.Find(SessionId);
	return BuildStateResponse(SessionId, Data);
}
