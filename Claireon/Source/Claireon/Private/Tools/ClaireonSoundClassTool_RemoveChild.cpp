// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundClassTool_RemoveChild.h"
#include "Tools/ClaireonAudioHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Sound/SoundClass.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

FString FClaireonSoundClassTool_RemoveChild::GetCategory() const { return TEXT("soundclass"); }
FString FClaireonSoundClassTool_RemoveChild::GetOperation() const { return TEXT("remove_child"); }

FString FClaireonSoundClassTool_RemoveChild::GetDescription() const
{
	return TEXT("Remove a child USoundClass from another USoundClass's ChildClasses array by path. "
				"Non-session, immediate operation; no session_id is needed. Returns removed_count "
				"(0 if not present, matching bundled tool semantics). Wraps the change in "
				"FScopedTransaction and saves the parent asset when something was actually removed.");
}

TSharedPtr<FJsonObject> FClaireonSoundClassTool_RemoveChild::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the parent USoundClass"), true);
	S.AddString(TEXT("child_path"), TEXT("Path of the child USoundClass to remove"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundClassTool_RemoveChild::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
		if (!Arguments->TryGetStringField(TEXT("child_class_path"), ChildPath) || ChildPath.IsEmpty())
		{
			return MakeErrorResult(TEXT("Missing required parameter: child_path"));
		}
	}

	FString Error;
	EClaireonAudioAssetKind Kind = EClaireonAudioAssetKind::Unknown;
	UObject* Loaded = ClaireonAudioHelpers::LoadAudioAsset(AssetPath, Kind, Error);
	if (!Loaded)
	{
		return MakeErrorResult(Error);
	}
	USoundClass* Parent = Cast<USoundClass>(Loaded);
	if (!Parent || Kind != EClaireonAudioAssetKind::SoundClass)
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset is not a SoundClass: %s"), *AssetPath));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove SoundClass Child")));
	Parent->Modify();
	const int32 Removed = Parent->ChildClasses.RemoveAll([&ChildPath](const TObjectPtr<USoundClass>& C)
	{
		return C && (C->GetPathName() == ChildPath);
	});
	Parent->MarkPackageDirty();
	if (Removed > 0)
	{
		if (!ClaireonAssetUtils::SaveAsset(Parent, Error))
		{
			return MakeErrorResult(Error);
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Parent->GetPathName());
	Data->SetStringField(TEXT("child_path"), ChildPath);
	Data->SetNumberField(TEXT("removed_count"), Removed);
	Data->SetNumberField(TEXT("child_count"), Parent->ChildClasses.Num());
	return MakeSuccessResult(Data, FString::Printf(TEXT("Removed %d child(ren) from %s"),
		Removed, *Parent->GetPathName()));
}
