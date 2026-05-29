// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundCueTool_Save.h"
#include "Tools/ClaireonAudioSessionRegistry.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Sound/SoundCue.h"
#include "FileHelpers.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"

FString FClaireonSoundCueTool_Save::GetCategory() const { return TEXT("soundcue"); }
FString FClaireonSoundCueTool_Save::GetOperation() const { return TEXT("save"); }

FString FClaireonSoundCueTool_Save::GetDescription() const
{
	return TEXT("Save the SoundCue's package to disk within the current session. "
				"Runs CompileSoundNodesFromGraphNodes (I4) before SavePackages, then clears the "
				"dirty flag on the session. Requires session_id from soundcue.open; the session "
				"stays open so further edits remain transactional after the save.");
}

TSharedPtr<FJsonObject> FClaireonSoundCueTool_Save::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session id returned by soundcue_open"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundCueTool_Save::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}
	FString SessionId;
	if (!Arguments->TryGetStringField(TEXT("session_id"), SessionId) || SessionId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: session_id"));
	}
	FAudioEditToolData* Data = ClaireonAudioSessionRegistry::FindSession(SessionId, ESoundCohort::SoundCue);
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("SoundCue session not found: %s"), *SessionId));
	}
	UObject* Asset = Data->Asset.Get();
	if (!Asset) return MakeErrorResult(TEXT("Asset is no longer valid"));
	UPackage* Package = Asset->GetOutermost();
	if (!Package) return MakeErrorResult(TEXT("Asset has no package"));

	if (USoundCue* Cue = Cast<USoundCue>(Asset))
	{
#if WITH_EDITOR
		if (Cue->SoundCueGraph)
		{
			Cue->CompileSoundNodesFromGraphNodes();
		}
#endif
	}
	Package->MarkPackageDirty();

	TArray<UPackage*> Packages;
	Packages.Add(Package);
	if (!UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty=*/false))
	{
		return MakeErrorResult(TEXT("SavePackages failed"));
	}
	Data->bDirty = false;
	Data->LastOperationStatus = TEXT("Saved");

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("session_id"), SessionId);
	Out->SetStringField(TEXT("asset_path"), Asset->GetPathName());
	return MakeSuccessResult(Out, FString::Printf(TEXT("Saved %s"), *Asset->GetPathName()));
}
