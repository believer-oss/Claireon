// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_Save.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLog.h"
#include "ClaireonSafeExec.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditingSubsystem.h"
#include "UObject/Package.h"
#include "FileHelpers.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_Save::GetOperation() const { return TEXT("save"); }

FString ClaireonStateTreeTool_Save::GetDescription() const
{
	return TEXT("Save the State Tree asset to disk from the open editing session. Requires open session_id from statetree_open. Immediate-write to the .uasset on disk. Compiles before saving and reports compile errors. Run save periodically during long edit sessions so changes survive editor crashes.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_Save::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_Save::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FStateTreeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	UStateTree* StateTree = Data->StateTree.Get();

	// compile-before-save through the same toolkit path
	// (UStateTreeEditingSubsystem::CompileStateTree). Without this the saved blob warns
	// "could not compile. Please resave" at cold load because LastCompiledEditorDataHash and
	// the derived runtime data are out of sync with EditorData. The toolkit's Save button
	// goes through this same path; the MCP-only path was skipping it.
	{
		FStateTreeCompilerLog CompilerLog;
		const bool bCompiled = UStateTreeEditingSubsystem::CompileStateTree(StateTree, CompilerLog);
		if (!bCompiled)
		{
			CompilerLog.DumpToLog(LogClaireon);
			Data->LastOperationStatus = TEXT("save -> Pre-save compile failed");
			return MakeErrorResult(TEXT("Pre-save compile failed; aborted save. Check LogClaireon for compiler diagnostics."));
		}
	}

	UPackage* Package = StateTree->GetPackage();
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
		Data->LastOperationStatus = FString::Printf(TEXT("save -> Saved %s"), *StateTree->GetPathName());
		return BuildStateResponse(SessionId, Data);
	}
	else
	{
		Data->LastOperationStatus = TEXT("save -> Failed");
		return MakeErrorResult(TEXT("Failed to save State Tree package"));
	}
}
