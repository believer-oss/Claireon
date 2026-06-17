// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCameraAssetTool_AddRig.h"

#include "ClaireonSessionManager.h"
#include "Dom/JsonObject.h"
#include "Misc/EngineVersionComparison.h"
#include "ScopedTransaction.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "Tools/ClaireonCameraAssetHelpers.h"

#if WITH_GAMEPLAY_CAMERAS

#include "Core/CameraAsset.h"
#include "Core/CameraRigAsset.h"
#include "UObject/UnrealType.h" // FArrayProperty / FScriptArrayHelper (5.6 reflection path)
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
#include "Core/CameraDirector.h"
#include "Directors/SingleCameraDirector.h"
#endif

#define LOCTEXT_NAMESPACE "ClaireonCameraAssetTool_AddRig"

FString FClaireonCameraAssetTool_AddRig::GetOperation() const { return TEXT("add_rig"); }

FString FClaireonCameraAssetTool_AddRig::GetDescription() const
{
	return TEXT("Append a new UCameraRigAsset (with null root node) to a UCameraAsset; returns the new rig index.");
}

TSharedPtr<FJsonObject> FClaireonCameraAssetTool_AddRig::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("/Game/ path of the camera asset"), true);
	S.AddString(TEXT("rig_name"), TEXT("Name for the new camera rig"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonCameraAssetTool_AddRig::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}
	FString RigName;
	if (!Arguments->TryGetStringField(TEXT("rig_name"), RigName) || RigName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: rig_name"));
	}

	const FString Canon = FClaireonSessionManager::CanonicalizePath(AssetPath);
	if (Canon.IsEmpty())
	{
		return MakeErrorResult(TEXT("Invalid asset_path (must start with /Game/)"));
	}

	UCameraAsset* Asset = LoadObject<UCameraAsset>(nullptr, *Canon);
	if (!Asset)
	{
		return MakeErrorResult(FString::Printf(TEXT("Camera asset not found: %s"), *Canon));
	}

	ClaireonCameraAssetHelpers::CloseEditorToolkitForAsset(Asset);

	FScopedTransaction Transaction(LOCTEXT("AddCameraRig", "[Claireon] Add Camera Rig"));
	Asset->Modify();

	// Note: UCameraAsset::CameraRigs is NOT marked UPROPERTY(Instanced), so the
	// CreateInstancedArrayElement helper would refuse it. The canonical engine
	// pattern (SCameraRigList::OnAddCameraRig) is to NewObject the rig with the
	// asset as Outer, then call AddCameraRig() which fires the event handler.
	UCameraRigAsset* NewRig = NewObject<UCameraRigAsset>(
		Asset,
		NAME_None,
		RF_Transactional | RF_Public);
	if (!NewRig)
	{
		return MakeErrorResult(TEXT("NewObject<UCameraRigAsset> returned null"));
	}

	// Try to give it the requested name. UObject names must be unique within the
	// outer; if a rig with that name already exists, fall back to keeping the
	// auto-assigned unique name and surface a warning via Logs.
	FString FinalName = NewRig->GetName();
	if (!RigName.IsEmpty())
	{
		const ERenameFlags RenameFlags = REN_DontCreateRedirectors | REN_DoNotDirty;
		if (NewRig->Rename(*RigName, Asset, REN_Test | RenameFlags))
		{
			NewRig->Rename(*RigName, Asset, RenameFlags);
			FinalName = NewRig->GetName();
		}
	}

#if UE_VERSION_OLDER_THAN(5, 6, 0)
	Asset->AddCameraRig(NewRig);
	Asset->PostEditChange();
#elif UE_VERSION_OLDER_THAN(5, 7, 0)
	// 5.6: AddCameraRig() is gone but rigs still live in the deprecated backing
	// array; append by reflecting on CameraRigs_DEPRECATED.
	if (FArrayProperty* Prop = FindFProperty<FArrayProperty>(UCameraAsset::StaticClass(), TEXT("CameraRigs_DEPRECATED")))
	{
		Asset->Modify();
		FScriptArrayHelper Helper(Prop, Prop->ContainerPtrToValuePtr<void>(Asset));
		const int32 Idx = Helper.AddValue();
		*reinterpret_cast<TObjectPtr<UCameraRigAsset>*>(Helper.GetRawPtr(Idx)) = NewRig;
	}
	else
	{
		Transaction.Cancel();
		return MakeErrorResult(TEXT("could not locate CameraRigs_DEPRECATED on UCameraAsset for add_rig on UE 5.6"));
	}
	Asset->PostEditChange();
#else
	// 5.7: AddCameraRig() is gone; rigs live on the camera director. Attach to a
	// USingleCameraDirector (the only single-rig slot).
	USingleCameraDirector* Single = Cast<USingleCameraDirector>(Asset->GetCameraDirector());
	if (!Single)
	{
		Transaction.Cancel();
		return MakeErrorResult(TEXT(
			"asset's camera director does not support add_rig on UE 5.7 "
			"(expected a USingleCameraDirector)"));
	}
	// Refuse rather than silently overwrite/orphan an existing rig (diverges from <=5.6 append).
	if (Single->CameraRig)
	{
		Transaction.Cancel();
		return MakeErrorResult(TEXT(
			"asset's USingleCameraDirector already references a rig; add_rig would "
			"overwrite it on UE 5.7 (single-camera directors hold exactly one rig)"));
	}
	Single->Modify();
	Single->CameraRig = NewRig;
	Asset->PostEditChange();
#endif

	const int32 NewIndex = ClaireonCameraAssetHelpers::GetCameraRigs(Asset).Num() - 1;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("rig_index"), NewIndex);
	Data->SetStringField(TEXT("rig_name"), FinalName);
	return MakeSuccessResult(Data,
		FString::Printf(TEXT("Added rig '%s' at index %d on %s"),
			*FinalName, NewIndex, *Asset->GetPathName()));
}

#undef LOCTEXT_NAMESPACE

#endif // WITH_GAMEPLAY_CAMERAS
