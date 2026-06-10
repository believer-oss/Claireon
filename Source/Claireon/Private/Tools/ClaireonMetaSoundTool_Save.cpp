// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMetaSoundTool_Save.h"
#include "Tools/ClaireonAudioSessionRegistry.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "FileHelpers.h"
#include "UObject/Package.h"
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

FString FClaireonMetaSoundTool_Save::GetCategory() const { return TEXT("metasound"); }
FString FClaireonMetaSoundTool_Save::GetOperation() const { return TEXT("save"); }

FString FClaireonMetaSoundTool_Save::GetDescription() const
{
	return TEXT("Save the MetaSound builder document to disk within the current session. The builder "
				"commit (UpdateAndRegisterForExecution) auto-compiles the MetaSound graph (I6), so "
				"there is no separate compile op. Requires session_id from metasound.open; the session "
				"stays open so further edits remain transactional after the save.");
}

TSharedPtr<FJsonObject> FClaireonMetaSoundTool_Save::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session id returned by metasound_open"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonMetaSoundTool_Save::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
#if !CLAIREON_HAS_METASOUND_BUILDER_API
	return MakeErrorResult(TEXT("MetaSound builder API not available on this engine branch"));
#else
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}
	FString SessionId;
	if (!Arguments->TryGetStringField(TEXT("session_id"), SessionId) || SessionId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: session_id"));
	}
	FAudioEditToolData* Data = ClaireonAudioSessionRegistry::FindSession(SessionId, ESoundCohort::MetaSound);
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("MetaSound session not found: %s"), *SessionId));
	}
	UObject* Asset = Data->Asset.Get();
	if (!Asset) return MakeErrorResult(TEXT("Asset is no longer valid"));
	UPackage* Package = Asset->GetOutermost();
	if (!Package) return MakeErrorResult(TEXT("Asset has no package"));

	bool bCompiled = false;
	if (UMetaSoundSource* Source = Cast<UMetaSoundSource>(Asset))
	{
		// / D2: builder commit auto-compiles. UpdateAndRegisterForExecution rebuilds the
		// MetaSound from the live document; no separate compile op is needed.
		Source->UpdateAndRegisterForExecution();
		bCompiled = true;
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
	Out->SetBoolField(TEXT("saved"), true);
	Out->SetBoolField(TEXT("compiled"), bCompiled);
	return MakeSuccessResult(Out, FString::Printf(TEXT("Saved %s"), *Asset->GetPathName()));
#endif
}
