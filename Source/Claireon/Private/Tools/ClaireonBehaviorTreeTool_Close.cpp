// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBehaviorTreeTool_Close.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonSafeExec.h"
#include "ClaireonSessionManager.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"
#include "UObject/Package.h"
#include "FileHelpers.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBehaviorTreeTool_Close::GetOperation() const { return TEXT("close"); }

FString ClaireonBehaviorTreeTool_Close::GetDescription() const
{
	return TEXT("Close a Behavior Tree editing session opened by behavior_tree.open, releasing the "
				"'behavior_tree_edit' lock so another cohort can acquire it. Optionally runs update_asset "
				"(recompile the runtime tree from the graph) and/or save before closing; in-flight "
				"transactional edits are discarded when neither flag is set.");
}

TSharedPtr<FJsonObject> ClaireonBehaviorTreeTool_Close::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddBoolean(TEXT("update_first"), TEXT("Rebuild the runtime BT from the graph before closing. If the rebuild cannot run, the session stays OPEN and an error is returned."));
	Builder.AddBoolean(TEXT("save_first"), TEXT("Save the BT asset before closing the session. If the save fails (read-only file, source-control lock, crash-recovery state), the session stays OPEN and an error is returned so no in-session work is lost. Legacy alias: 'save' (save_first takes precedence when both are present)."));
	return Builder.Build();
}

FToolResult ClaireonBehaviorTreeTool_Close::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FBehaviorTreeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	bool bUpdateFirst = false;
	bool bSaveFirst = false;
	Arguments->TryGetBoolField(TEXT("update_first"), bUpdateFirst);
	if (!Arguments->TryGetBoolField(TEXT("save_first"), bSaveFirst))
	{
		// Legacy alias: earlier tool descriptions referred to a generic 'save'
		// flag. Honored only when 'save_first' is absent (WI-9).
		Arguments->TryGetBoolField(TEXT("save"), bSaveFirst);
	}

	// Any update_first/save_first failure below returns WITHOUT closing the
	// session so in-session edits are never silently discarded (WI-9).
	if (bUpdateFirst)
	{
		if (!Data->IsValid())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("update_first failed: the session's Behavior Tree asset is no longer loaded, so the runtime tree could not be rebuilt. Session %s is still open. Close with save_first=false and update_first=false to discard it."),
				*SessionId));
		}
		UBehaviorTreeGraph* Graph = ClaireonBehaviorTreeHelpers::GetBTGraph(Data->BehaviorTree.Get(), Error);
		if (!IsValid(Graph))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("update_first failed: %s. The runtime tree was NOT rebuilt and session %s is still open."),
				*Error, *SessionId));
		}
		Graph->UpdateAsset();
	}

	if (bSaveFirst)
	{
		if (!Data->IsValid())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("save_first failed: the session's Behavior Tree asset is no longer loaded, so nothing could be saved. The asset was NOT saved and session %s is still open. Close with save_first=false to discard it."),
				*SessionId));
		}

		UBehaviorTree* BT = Data->BehaviorTree.Get();

		if (ClaireonSafeExec::DidLastExecutionCrash())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("save_first failed: save of %s was skipped because a previous tool execution crashed (ClaireonSafeExec crash flag is set) and editor state may be corrupted. The asset was NOT saved and session %s is still open. Verify editor state (restart if needed) and retry, or close with save_first=false to discard in-session edits."),
				*BT->GetPathName(), *SessionId));
		}

		UPackage* Package = BT->GetPackage();
		Package->SetDirtyFlag(true);

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Package);
		const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);
		if (!bSaved)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("save_first failed: %s did not save (the package save was rejected; check for a read-only file or source-control lock). The asset was NOT saved and session %s is still open. Fix the save blocker and retry, or close with save_first=false to discard in-session edits."),
				*BT->GetPathName(), *SessionId));
		}
	}

	FClaireonSessionManager::Get().CloseSession(SessionId);
	ToolData.Remove(SessionId);

	TSharedPtr<FJsonObject> CloseData = MakeShared<FJsonObject>();
	CloseData->SetStringField(TEXT("session_id"), SessionId);
	CloseData->SetStringField(TEXT("status"), TEXT("closed"));
	return MakeSuccessResult(CloseData, FString::Printf(TEXT("Session closed: %s"), *SessionId));
}
