// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundClassTool_AddChild.h"
#include "Tools/ClaireonAudioHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "ClaireonPathResolver.h"

#include "Sound/SoundClass.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

FString FClaireonSoundClassTool_AddChild::GetCategory() const { return TEXT("soundclass"); }
FString FClaireonSoundClassTool_AddChild::GetOperation() const { return TEXT("add_child"); }

FString FClaireonSoundClassTool_AddChild::GetDescription() const
{
	return TEXT("Add a child USoundClass to another USoundClass's ChildClasses array (AddUnique). "
				"Non-session, immediate operation; no session_id required. Saves the parent asset "
				"on success. Use soundclass.remove_child to undo the relationship.");
}

TSharedPtr<FJsonObject> FClaireonSoundClassTool_AddChild::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the parent USoundClass"), true);
	S.AddString(TEXT("child_path"), TEXT("Path to the child USoundClass to add"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundClassTool_AddChild::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	FString ChildPath;
	if (!Arguments->TryGetStringField(TEXT("child_path"), ChildPath) || ChildPath.IsEmpty())
	{
		// Backward-compat with bundled tool's "child_class_path" name.
		if (!Arguments->TryGetStringField(TEXT("child_class_path"), ChildPath) || ChildPath.IsEmpty())
		{
			return MakeErrorResult(TEXT("Missing required parameter: child_path"));
		}
	}

	FString Error;
	EClaireonAudioAssetKind Kind = EClaireonAudioAssetKind::Unknown;
	UObject* Loaded = ClaireonAudioHelpers::LoadAudioAsset(AssetPath, Kind, Error);
	if (!Loaded || Kind != EClaireonAudioAssetKind::SoundClass)
	{
		if (Loaded) return MakeErrorResult(FString::Printf(TEXT("Asset is not a SoundClass: %s"), *AssetPath));
		return MakeErrorResult(Error);
	}
	USoundClass* Parent = Cast<USoundClass>(Loaded);
	if (!Parent) return MakeErrorResult(FString::Printf(TEXT("Asset is not a SoundClass: %s"), *AssetPath));

	const ClaireonPathResolver::FResolveResult Resolved = ClaireonPathResolver::Resolve(ChildPath);
	USoundClass* Child = Resolved.bSuccess ? LoadObject<USoundClass>(nullptr, *Resolved.ResolvedPath.Path) : nullptr;
	if (!Child)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not load SoundClass at %s"), *ChildPath));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add SoundClass Child")));
	Parent->Modify();
	Parent->ChildClasses.AddUnique(Child);
	Parent->MarkPackageDirty();
	if (!ClaireonAssetUtils::SaveAsset(Parent, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Parent->GetPathName());
	Data->SetStringField(TEXT("child_path"), Child->GetPathName());
	Data->SetNumberField(TEXT("child_count"), Parent->ChildClasses.Num());
	return MakeSuccessResult(Data, FString::Printf(TEXT("Added child %s to %s"),
		*Child->GetPathName(), *Parent->GetPathName()));
}
