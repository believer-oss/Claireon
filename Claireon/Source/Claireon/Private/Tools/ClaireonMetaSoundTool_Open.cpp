// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMetaSoundTool_Open.h"
#include "Tools/ClaireonAudioHelpers.h"
#include "Tools/ClaireonAudioSessionRegistry.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonSessionManager.h"

#include "Dom/JsonObject.h"

#undef CLAIREON_HAS_METASOUND_BUILDER_API
#define CLAIREON_HAS_METASOUND_BUILDER_API 0
#if __has_include("MetasoundDocumentBuilderRegistry.h") && __has_include("MetasoundBuilderSubsystem.h") && __has_include("MetasoundFrontendLiteral.h")
#include "MetasoundDocumentBuilderRegistry.h"
#include "MetasoundBuilderSubsystem.h"
#include "MetasoundFrontendLiteral.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundSource.h"
#undef CLAIREON_HAS_METASOUND_BUILDER_API
#define CLAIREON_HAS_METASOUND_BUILDER_API 1
#endif

FString FClaireonMetaSoundTool_Open::GetCategory() const { return TEXT("metasound"); }
FString FClaireonMetaSoundTool_Open::GetOperation() const { return TEXT("open"); }

FString FClaireonMetaSoundTool_Open::GetDescription() const
{
	return TEXT("Open a UMetaSoundSource editing session and lock the asset under tool name "
				"'audio_edit' (I1) so SoundCue and MetaSound mutually exclude on the same asset path. "
				"Eagerly attaches the FMetaSoundFrontendDocumentBuilder so subsequent session ops "
				"(add_node, connect_pins, set_default, save) never have to first-touch the registry.");
}

TSharedPtr<FJsonObject> FClaireonMetaSoundTool_Open::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the UMetaSoundSource asset"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonMetaSoundTool_Open::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
#if !CLAIREON_HAS_METASOUND_BUILDER_API
	return MakeErrorResult(TEXT("MetaSound builder API not available on this engine branch"));
#else
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	EClaireonAudioAssetKind Kind = EClaireonAudioAssetKind::Unknown;
	FString LoadError;
	UObject* Asset = ClaireonAudioHelpers::LoadAudioAsset(AssetPath, Kind, LoadError);
	if (!Asset) return MakeErrorResult(LoadError);
	if (Kind != EClaireonAudioAssetKind::MetaSoundSource)
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset is not a MetaSoundSource: %s"), *AssetPath));
	}

	ClaireonAudioSessionRegistry::EnsureDelegateRegistered();

	const FString ResolvedPath = Asset->GetPathName();
	// I1: lock string is the literal "audio_edit" (preserved across cohorts per D3=B).
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

	// Eagerly attach the document builder so subsequent ops never have to "first-touch" the registry.
	FMetaSoundFrontendDocumentBuilder* Builder = nullptr;
#if WITH_EDITORONLY_DATA
	if (Asset->Implements<UMetaSoundDocumentInterface>() || Cast<UMetaSoundSource>(Asset))
	{
		using namespace Metasound::Engine;
		FDocumentBuilderRegistry& Registry = FDocumentBuilderRegistry::GetChecked();
		UMetaSoundBuilderBase& BuilderObj = Registry.FindOrBeginBuilding<UMetaSoundBuilderBase>(*Asset);
		Builder = &BuilderObj.GetBuilder();
	}
#endif
	if (!Builder)
	{
		FClaireonSessionManager::Get().CloseSession(SessionId);
		return MakeErrorResult(TEXT("Could not attach MetaSound document builder"));
	}

	FAudioEditToolData* Entry = ClaireonAudioSessionRegistry::CreateSession(SessionId, ESoundCohort::MetaSound);
	if (!Entry)
	{
		FClaireonSessionManager::Get().CloseSession(SessionId);
		return MakeErrorResult(TEXT("Failed to create session entry"));
	}
	Entry->Asset = Asset;
	Entry->AssetPath = ResolvedPath;
	Entry->LastOperationStatus = TEXT("Session opened");
	Entry->FocusedNodeIndex = INDEX_NONE;
	Entry->MetaSoundBuilderHandle = static_cast<void*>(Builder);

	ClaireonAssetUtils::OpenAssetEditorIfHeadless(Asset);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("session_id"), SessionId);
	Data->SetStringField(TEXT("asset_path"), ResolvedPath);
	Data->SetStringField(TEXT("kind"), TEXT("metasound_source"));
	return MakeSuccessResult(Data, FString::Printf(TEXT("Opened MetaSound session %s on %s"), *SessionId, *ResolvedPath));
#endif
}
