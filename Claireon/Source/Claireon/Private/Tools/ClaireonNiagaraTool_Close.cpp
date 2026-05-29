// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_Close.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSafeExec.h"
#include "ClaireonSessionManager.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"
#include "FileHelpers.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_Close::GetOperation() const { return TEXT("close"); }

FString ClaireonNiagaraTool_Close::GetDescription() const
{
    return TEXT("Close a Niagara edit session, optionally saving the asset first before releasing the session lock.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_Close::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddBoolean(TEXT("save_first"), TEXT("Save the Niagara System before closing."));
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_Close::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FNiagaraEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	bool bSaveFirst = false;
	Arguments->TryGetBoolField(TEXT("save_first"), bSaveFirst);

	if (bSaveFirst && Data->IsValid())
	{
		UNiagaraSystem* System = Data->System.Get();
		UPackage* Package = System->GetPackage();
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
