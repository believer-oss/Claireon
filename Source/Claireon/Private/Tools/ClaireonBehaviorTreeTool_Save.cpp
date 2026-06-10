// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBehaviorTreeTool_Save.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSafeExec.h"
#include "BehaviorTree/BehaviorTree.h"
#include "UObject/Package.h"
#include "FileHelpers.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBehaviorTreeTool_Save::GetOperation() const { return TEXT("save"); }

FString ClaireonBehaviorTreeTool_Save::GetDescription() const
{
	return TEXT("Save the Behavior Tree asset to disk within the current session and clear the dirty "
				"flag. Requires session_id from behavior_tree.open; the session stays open so further "
				"transactional edits (add/remove nodes, decorators, services) remain available after "
				"the save.");
}

TSharedPtr<FJsonObject> ClaireonBehaviorTreeTool_Save::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonBehaviorTreeTool_Save::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FBehaviorTreeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	if (!Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	UBehaviorTree* BT = Data->BehaviorTree.Get();
	UPackage* Package = BT->GetPackage();
	Package->SetDirtyFlag(true);

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		return MakeErrorResult(TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor."));
	}
	bool bSuccess = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);

	if (bSuccess)
	{
		Data->LastOperationStatus = FString::Printf(TEXT("save - Saved %s"), *BT->GetPathName());
		return BuildStateResponse(SessionId, Data);
	}
	else
	{
		Data->LastOperationStatus = TEXT("save - Failed");
		return MakeErrorResult(TEXT("Failed to save Behavior Tree package"));
	}
}
