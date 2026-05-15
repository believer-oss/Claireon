// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlackboardTool_Save.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSafeExec.h"
#include "BehaviorTree/BlackboardData.h"
#include "UObject/Package.h"
#include "FileHelpers.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlackboardTool_Save::GetName() const
{
	return TEXT("claireon.blackboard_save");
}

FString ClaireonBlackboardTool_Save::GetDescription() const
{
	return TEXT("Save the blackboard asset to disk.");
}

TSharedPtr<FJsonObject> ClaireonBlackboardTool_Save::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonBlackboardTool_Save::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FBlackboardEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	if (!Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	UBlackboardData* BB = Data->BlackboardData.Get();
	UPackage* Package = BB->GetPackage();
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
		Data->LastOperationStatus = FString::Printf(TEXT("save - Saved %s"), *BB->GetPathName());
		return BuildStateResponse(SessionId, Data);
	}
	else
	{
		Data->LastOperationStatus = TEXT("save - Failed");
		return MakeErrorResult(TEXT("Failed to save Blackboard package"));
	}
}
