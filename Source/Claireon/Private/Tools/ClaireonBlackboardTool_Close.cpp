// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlackboardTool_Close.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSessionManager.h"
#include "ClaireonSafeExec.h"
#include "BehaviorTree/BlackboardData.h"
#include "UObject/Package.h"
#include "FileHelpers.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlackboardTool_Close::GetOperation() const { return TEXT("close"); }

FString ClaireonBlackboardTool_Close::GetDescription() const
{
	return TEXT("Close a Blackboard editing session opened by blackboard.open, releasing the "
				"'blackboard_edit' lock so another cohort can acquire it. Optionally saves the asset "
				"before closing; in-flight transactional key edits are discarded when save is not "
				"requested.");
}

TSharedPtr<FJsonObject> ClaireonBlackboardTool_Close::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddBoolean(TEXT("save_first"), TEXT("Save the blackboard before closing the session. If the save fails (read-only file, source-control lock, crash-recovery state), the session stays OPEN and an error is returned so no in-session work is lost. Legacy alias: 'save' (save_first takes precedence when both are present)."));
	return Builder.Build();
}

FToolResult ClaireonBlackboardTool_Close::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FBlackboardEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	bool bSaveFirst = false;
	if (!Arguments->TryGetBoolField(TEXT("save_first"), bSaveFirst))
	{
		// Legacy alias: earlier tool descriptions referred to a generic 'save'
		// flag. Honored only when 'save_first' is absent (WI-9).
		Arguments->TryGetBoolField(TEXT("save"), bSaveFirst);
	}

	// Any save_first failure below returns WITHOUT closing the session so
	// in-session edits are never silently discarded (WI-9).
	if (bSaveFirst)
	{
		if (!Data->IsValid())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("save_first failed: the session's Blackboard asset is no longer loaded, so nothing could be saved. The asset was NOT saved and session %s is still open. Close with save_first=false to discard it."),
				*SessionId));
		}

		UBlackboardData* BB = Data->BlackboardData.Get();

		if (ClaireonSafeExec::DidLastExecutionCrash())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("save_first failed: save of %s was skipped because a previous tool execution crashed (ClaireonSafeExec crash flag is set) and editor state may be corrupted. The asset was NOT saved and session %s is still open. Verify editor state (restart if needed) and retry, or close with save_first=false to discard in-session edits."),
				*BB->GetPathName(), *SessionId));
		}

		UPackage* Package = BB->GetPackage();
		Package->SetDirtyFlag(true);

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Package);
		const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);
		if (!bSaved)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("save_first failed: %s did not save (the package save was rejected; check for a read-only file or source-control lock). The asset was NOT saved and session %s is still open. Fix the save blocker and retry, or close with save_first=false to discard in-session edits."),
				*BB->GetPathName(), *SessionId));
		}
	}

	FClaireonSessionManager::Get().CloseSession(SessionId);
	ToolData.Remove(SessionId);

	TSharedPtr<FJsonObject> CloseData = MakeShared<FJsonObject>();
	CloseData->SetStringField(TEXT("session_id"), SessionId);
	CloseData->SetStringField(TEXT("status"), TEXT("closed"));
	return MakeSuccessResult(CloseData, FString::Printf(TEXT("Session closed: %s"), *SessionId));
}
