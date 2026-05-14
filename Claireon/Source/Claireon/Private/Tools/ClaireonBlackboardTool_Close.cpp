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
	return TEXT("Close a Blackboard editing session. Optionally save before closing.");
}

TSharedPtr<FJsonObject> ClaireonBlackboardTool_Close::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddBoolean(TEXT("save_first"), TEXT("Save the blackboard before closing the session."));
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
	Arguments->TryGetBoolField(TEXT("save_first"), bSaveFirst);

	if (bSaveFirst && Data->IsValid())
	{
		UBlackboardData* BB = Data->BlackboardData.Get();
		UPackage* Package = BB->GetPackage();
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
