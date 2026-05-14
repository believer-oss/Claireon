// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_Close.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSafeExec.h"
#include "ClaireonSessionManager.h"
#include "StateTree.h"
#include "UObject/Package.h"
#include "FileHelpers.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_Close::GetOperation() const { return TEXT("close"); }

FString ClaireonStateTreeTool_Close::GetDescription() const
{
	return TEXT("Close the open State Tree editing session and release its asset lock. Requires open session_id from statetree_open. Transactional with respect to any final save. Pass save_first=true to flush changes to disk before close; otherwise unsaved changes are discarded.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_Close::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddBoolean(TEXT("save_first"), TEXT("Save the State Tree before closing."));
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_Close::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FStateTreeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	bool bSaveFirst = false;
	Arguments->TryGetBoolField(TEXT("save_first"), bSaveFirst);

	if (bSaveFirst && Data->IsValid())
	{
		UStateTree* StateTree = Data->StateTree.Get();
		UPackage* Package = StateTree->GetPackage();
		Package->SetDirtyFlag(true);

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Package);
		if (!ClaireonSafeExec::DidLastExecutionCrash())
		{
			UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);
		}
	}

	FClaireonSessionManager::Get().CloseSession(SessionId);
	ToolData.Remove(SessionId);

	TSharedPtr<FJsonObject> CloseData = MakeShared<FJsonObject>();
	CloseData->SetStringField(TEXT("session_id"), SessionId);
	CloseData->SetStringField(TEXT("status"), TEXT("closed"));
	return MakeSuccessResult(CloseData, FString::Printf(TEXT("Session closed: %s"), *SessionId));
}
