// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_Save.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSafeExec.h"
#include "LevelSequence.h"
#include "UObject/Package.h"
#include "FileHelpers.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_Save::GetOperation() const { return TEXT("save"); }

FString ClaireonLevelSequenceTool_Save::GetDescription() const
{
	return TEXT("Save the Level Sequence asset associated with a session without closing the session.");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_Save::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("If true, returns brief status instead of full state."));
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_Save::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FSequenceEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}
	if (!Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}
	UPackage* Package = Data->Sequence->GetOutermost();
	if (!Package)
	{
		return MakeErrorResult(TEXT("Level Sequence has no package"));
	}
	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		return MakeErrorResult(TEXT("Save blocked: editor state may be corrupted after a previous crash."));
	}
	bool bSaved = UEditorLoadingAndSavingUtils::SavePackages({ Package }, false);
	Data->LastOperationStatus = bSaved ? TEXT("Saved successfully") : TEXT("Save failed");
	return BuildStateResponse(SessionId, Data);
}
