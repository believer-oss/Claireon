// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_Save.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSafeExec.h"
#include "NiagaraSystem.h"
#include "UObject/Package.h"
#include "FileHelpers.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_Save::GetName() const
{
	return TEXT("claireon.niagara_save");
}

FString ClaireonNiagaraTool_Save::GetDescription() const
{
	return TEXT("Save the Niagara System being edited.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_Save::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_Save::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FNiagaraEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	UNiagaraSystem* System = Data->System.Get();
	UPackage* Package = System->GetPackage();
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
		Data->LastOperationStatus = FString::Printf(TEXT("save -> Saved %s"), *System->GetPathName());
		return BuildStateResponse(SessionId, Data);
	}
	Data->LastOperationStatus = TEXT("save -> Failed");
	return MakeErrorResult(TEXT("Failed to save Niagara System package"));
}
