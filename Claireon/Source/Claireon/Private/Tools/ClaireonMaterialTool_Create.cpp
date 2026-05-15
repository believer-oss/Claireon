// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialTool_Create.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "ClaireonSessionManager.h"
#include "Materials/Material.h"
#include "Factories/MaterialFactoryNew.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"
#include "UObject/SoftObjectPath.h"

using FToolResult = IClaireonTool::FToolResult;

namespace
{
	static bool ParseShadingModel_Create(const FString& Str, EMaterialShadingModel& OutModel)
	{
		const UEnum* Enum = StaticEnum<EMaterialShadingModel>();
		if (!Enum) return false;
		const int64 Val = Enum->GetValueByNameString(Str);
		if (Val == INDEX_NONE) return false;
		OutModel = static_cast<EMaterialShadingModel>(Val);
		return true;
	}

	static bool ParseMaterialDomain_Create(const FString& Str, EMaterialDomain& OutDomain)
	{
		const UEnum* Enum = StaticEnum<EMaterialDomain>();
		if (!Enum) return false;
		const int64 Val = Enum->GetValueByNameString(Str);
		if (Val == INDEX_NONE) return false;
		OutDomain = static_cast<EMaterialDomain>(Val);
		return true;
	}
}

FString ClaireonMaterialTool_Create::GetName() const
{
	return TEXT("claireon.material_create");
}

FString ClaireonMaterialTool_Create::GetDescription() const
{
	return TEXT("Create a new UMaterial asset and open an edit session. Optionally sets initial material_domain and shading_model.");
}

TSharedPtr<FJsonObject> ClaireonMaterialTool_Create::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("package_path"), TEXT("Content-browser folder path (e.g. /Game/Materials)."), true);
	Builder.AddString(TEXT("asset_name"), TEXT("Asset name (without path)."), true);
	Builder.AddString(TEXT("material_domain"), TEXT("Optional initial material domain (e.g. MD_Surface)."));
	Builder.AddString(TEXT("shading_model"), TEXT("Optional initial shading model (e.g. MSM_DefaultLit)."));
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("Return only a brief status."));
	return Builder.Build();
}

FToolResult ClaireonMaterialTool_Create::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString PackagePath, AssetName;
	if (!Arguments->TryGetStringField(TEXT("package_path"), PackagePath) || PackagePath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'create' requires 'package_path'"));
	}
	if (!Arguments->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'create' requires 'asset_name'"));
	}

	const FString FullObjectPath = FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *AssetName, *AssetName);
	FSoftObjectPath SoftPath(FullObjectPath);
	if (SoftPath.TryLoad())
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset already exists at path: %s. Use 'open' instead."), *FullObjectPath));
	}

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
	UObject* NewAsset = AssetTools.CreateAsset(AssetName, PackagePath, UMaterial::StaticClass(), Factory);
	UMaterial* NewMaterial = Cast<UMaterial>(NewAsset);
	if (!NewMaterial)
	{
		return MakeErrorResult(TEXT("Failed to create UMaterial via AssetTools.CreateAsset"));
	}

	FString DomainStr;
	if (Arguments->TryGetStringField(TEXT("material_domain"), DomainStr) && !DomainStr.IsEmpty())
	{
		EMaterialDomain Domain;
		if (ParseMaterialDomain_Create(DomainStr, Domain))
		{
			NewMaterial->Modify();
			NewMaterial->MaterialDomain = Domain;
			NewMaterial->PostEditChange();
		}
	}

	FString ShadingStr;
	if (Arguments->TryGetStringField(TEXT("shading_model"), ShadingStr) && !ShadingStr.IsEmpty())
	{
		EMaterialShadingModel SM;
		if (ParseShadingModel_Create(ShadingStr, SM))
		{
			FString SmErr;
			ClaireonMaterialHelpers::SetShadingModel(NewMaterial, SM, SmErr);
		}
	}

	EnsureDelegateRegistered();

	const FString ResolvedPath = NewMaterial->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedPath, MaterialSessionToolName);
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		return MakeErrorResult(FString::Printf(TEXT("Asset is locked by %s session %s"), *Blocker.ToolName, *Blocker.SessionId));
	}
	const FString SessionId = OpenResult.SessionId;

	FMaterialEditToolData NewData;
	NewData.Material = NewMaterial;
	NewData.LastOperationStatus = TEXT("Material created and session opened");
	if (Arguments->HasField(TEXT("suppress_output")))
	{
		NewData.bSuppressOutput = Arguments->GetBoolField(TEXT("suppress_output"));
	}
	ToolData.Add(SessionId, MoveTemp(NewData));

	FMaterialEditToolData* Data = ToolData.Find(SessionId);
	return BuildStateResponse(SessionId, Data);
}
