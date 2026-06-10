// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundCueTool_Open.h"
#include "Tools/ClaireonAudioHelpers.h"
#include "Tools/ClaireonAudioSessionRegistry.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonSessionManager.h"

#include "Sound/SoundCue.h"
#include "Dom/JsonObject.h"

FString FClaireonSoundCueTool_Open::GetCategory() const { return TEXT("soundcue"); }
FString FClaireonSoundCueTool_Open::GetOperation() const { return TEXT("open"); }

FString FClaireonSoundCueTool_Open::GetDescription() const
{
	return TEXT("Open a USoundCue editing session and lock the asset under tool name 'audio_edit' "
				"(I1) so only one cohort holds it at a time. Returns a session_id used by all "
				"subsequent SoundCue session ops (add_node, connect_nodes, save, close, etc). "
				"Also surfaces the asset editor when running headless.");
}

TSharedPtr<FJsonObject> FClaireonSoundCueTool_Open::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the USoundCue asset"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundCueTool_Open::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString LoadError;
	const EClaireonAudioAssetKind ResolvedKind = ResolveAudioAssetKindFromPath(AssetPath, LoadError);
	EClaireonAudioAssetKind Kind = EClaireonAudioAssetKind::Unknown;
	UObject* Asset = ClaireonAudioHelpers::LoadAudioAsset(AssetPath, Kind, LoadError);
	if (!Asset) return MakeErrorResult(LoadError);
	if (Kind != EClaireonAudioAssetKind::SoundCue)
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset is not a SoundCue: %s"), *AssetPath));
	}

	ClaireonAudioSessionRegistry::EnsureDelegateRegistered();

	const FString ResolvedPath = Asset->GetPathName();
	// lock string is the literal "audio_edit" (preserved across cohorts per D3=B).
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedPath, TEXT("audio_edit"));
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		return MakeErrorResult(FString::Printf(TEXT("Asset is locked by %s session %s"), *Blocker.ToolName, *Blocker.SessionId));
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *ResolvedPath));
	}
	if (OpenResult.Result == EOpenSessionResult::ReusedExistingSession)
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset is locked by existing audio_edit session %s"), *OpenResult.SessionId));
	}
	const FString SessionId = OpenResult.SessionId;

	FAudioEditToolData* Entry = ClaireonAudioSessionRegistry::CreateSession(SessionId, ESoundCohort::SoundCue);
	if (!Entry)
	{
		return MakeErrorResult(TEXT("Failed to create session entry"));
	}
	Entry->Asset = Asset;
	Entry->AssetPath = ResolvedPath;
	Entry->LastOperationStatus = TEXT("Session opened");
	Entry->FocusedNodeIndex = INDEX_NONE;

#if WITH_EDITOR
	if (USoundCue* Cue = Cast<USoundCue>(Asset))
	{
		if (!Cue->SoundCueGraph)
		{
			Cue->CreateGraph();
		}
	}
#endif

	(void)ResolvedKind; // currently unused; reserved for parity with bundled validation messages.

	ClaireonAssetUtils::OpenAssetEditorIfHeadless(Asset);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("session_id"), SessionId);
	Data->SetStringField(TEXT("asset_path"), ResolvedPath);
	Data->SetStringField(TEXT("kind"), TEXT("sound_cue"));
	return MakeSuccessResult(Data, FString::Printf(TEXT("Opened SoundCue session %s on %s"), *SessionId, *ResolvedPath));
}
