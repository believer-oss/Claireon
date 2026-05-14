// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimTools_Session.h"
#include "Tools/ClaireonAnimHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonLog.h"
#include "Animation/AnimSequenceBase.h"
#include "FileHelpers.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"

// ============================================================================
// claireon.anim_open
// ============================================================================

FString ClaireonAnimTool_Open::GetName() const { return TEXT("claireon.anim_open"); }

FString ClaireonAnimTool_Open::GetDescription() const
{
	return TEXT("Open an animation asset (AnimSequence, AnimMontage, or AnimComposite) for editing and return a session_id used by all subsequent claireon.anim_* operations. The session reuses an existing one for the same asset path; close it with claireon.anim_close to release the lock.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_Open::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Unreal asset path (e.g. /Game/Char/STELLA/Anim/AM_Combo)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_Open::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	FString AssetType, Error;
	UAnimSequenceBase* Anim = ClaireonAnimHelpers::LoadAnimAsset(AssetPath, AssetType, Error);
	if (!Anim)
	{
		return MakeErrorResult(Error);
	}

	EnsureDelegateRegistered();

	const FString ResolvedPath = Anim->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedPath, AnimSessionToolName);
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		const FTimespan Elapsed = FDateTime::UtcNow() - Blocker.LastAccessTime;
		return MakeErrorResult(FString::Printf(
			TEXT("Asset is locked by %s session %s (last activity %dm %ds ago). Close that session first."),
			*Blocker.ToolName, *Blocker.SessionId,
			static_cast<int32>(Elapsed.GetTotalMinutes()),
			static_cast<int32>(Elapsed.GetTotalSeconds()) % 60));
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *ResolvedPath));
	}

	const FString SessionId = OpenResult.SessionId;

	FAnimEditToolData NewData;
	NewData.Animation = Anim;
	NewData.AssetType = AssetType;
	NewData.LastOperationStatus = TEXT("Opened session");
	ToolData.Add(SessionId, MoveTemp(NewData));

	FString StructureText = ClaireonAnimHelpers::FormatAnimStructure(Anim, AssetType, true);

	TSharedPtr<FJsonObject> OpenData = MakeShared<FJsonObject>();
	OpenData->SetStringField(TEXT("session_id"), SessionId);
	OpenData->SetStringField(TEXT("asset_path"), ResolvedPath);
	OpenData->SetStringField(TEXT("asset_type"), AssetType);
	OpenData->SetStringField(TEXT("status"), TEXT("Opened session"));
	OpenData->SetStringField(TEXT("structure"), StructureText);

	return MakeSuccessResult(OpenData, FString::Printf(TEXT("Opened session for %s (%s)"), *FPaths::GetBaseFilename(ResolvedPath), *AssetType));
}

// ============================================================================
// claireon.anim_close
// ============================================================================

FString ClaireonAnimTool_Close::GetName() const { return TEXT("claireon.anim_close"); }

FString ClaireonAnimTool_Close::GetDescription() const
{
	return TEXT("Close an animation editing session and release the asset lock. Pass save=true to persist outstanding changes before closing. Common pitfall: closing without save discards in-flight edits silently; call claireon.anim_save first when changes must survive.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_Close::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session identifier from open"), true);
	S.AddBoolean(TEXT("save_first"), TEXT("Save the animation before closing"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_Close::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	bool bSaveFirst = false;
	Arguments->TryGetBoolField(TEXT("save_first"), bSaveFirst);

	if (bSaveFirst)
	{
		// Inline save
		UAnimSequenceBase* Anim = Data->Animation.Get();
		UPackage* Package = Anim->GetOutermost();
		Package->SetDirtyFlag(true);
		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Package);
		UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false);
	}

	FClaireonSessionManager::Get().CloseSession(SessionId);
	ToolData.Remove(SessionId);

	TSharedPtr<FJsonObject> CloseData = MakeShared<FJsonObject>();
	CloseData->SetStringField(TEXT("session_id"), SessionId);
	CloseData->SetStringField(TEXT("status"), TEXT("closed"));
	return MakeSuccessResult(CloseData, FString::Printf(TEXT("Session %s closed"), *SessionId));
}

// ============================================================================
// claireon.anim_get_state
// ============================================================================

FString ClaireonAnimTool_GetState::GetName() const { return TEXT("claireon.anim_get_state"); }

FString ClaireonAnimTool_GetState::GetDescription() const
{
	return TEXT("Get the current state of an animation editing session. Requires open session_id from claireon.anim_open. Read-only. Returns notifies, curves, sections, segments, slots, and asset metadata so the caller can re-anchor after a series of edits without round-tripping the asset.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_GetState::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddEnum(TEXT("focus"), TEXT("Show only a specific aspect of the animation"),
		{TEXT("notifies"), TEXT("curves"), TEXT("sections"), TEXT("slots"), TEXT("sync_markers"), TEXT("properties"), TEXT("modifiers"), TEXT("metadata")});
	S.AddInteger(TEXT("notify_index"), TEXT("Set the focused notify by index (0-based)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_GetState::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	FString FocusSection;
	Arguments->TryGetStringField(TEXT("focus"), FocusSection);

	double NotifyIndexD = -1.0;
	if (Arguments->TryGetNumberField(TEXT("notify_index"), NotifyIndexD))
	{
		Data->FocusedNotifyIndex = static_cast<int32>(NotifyIndexD);
	}

	if (!FocusSection.IsEmpty())
	{
		FString SectionOutput = ClaireonAnimHelpers::FormatAnimStructure(Data->Animation.Get(), Data->AssetType, true, FocusSection);
		TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
		ResponseData->SetStringField(TEXT("session_id"), SessionId);
		ResponseData->SetStringField(TEXT("asset_path"), Data->Animation->GetPathName());
		ResponseData->SetStringField(TEXT("asset_type"), Data->AssetType);
		ResponseData->SetStringField(TEXT("focus"), FocusSection);
		ResponseData->SetStringField(TEXT("state_view"), SectionOutput);
		return MakeSuccessResult(ResponseData, FString::Printf(TEXT("Session %s: focused on %s"), *SessionId.Left(8), *FocusSection));
	}

	Data->LastOperationStatus = TEXT("get_state");
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// claireon.anim_save
// ============================================================================

FString ClaireonAnimTool_Save::GetName() const { return TEXT("claireon.anim_save"); }

FString ClaireonAnimTool_Save::GetDescription() const
{
	return TEXT("Save the animation asset to disk in the open editing session. Requires open session_id from claireon.anim_open. Immediate-write to the package; the session itself remains open for further edits. Common pitfall: save does not validate the asset; chain claireon.asset_validate when stricter checks are required.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_Save::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_Save::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	UAnimSequenceBase* Anim = Data->Animation.Get();
	UPackage* Package = Anim->GetOutermost();
	Package->SetDirtyFlag(true);

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimSave", "MCP: Save Animation"));

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false);

	if (bSaved)
	{
		Data->LastOperationStatus = FString::Printf(TEXT("save -> Saved %s"), *Anim->GetPathName());
		return BuildStateResponse(SessionId, Data);
	}
	else
	{
		Data->LastOperationStatus = TEXT("save -> Failed");
		return MakeErrorResult(TEXT("Failed to save animation package"));
	}
}
