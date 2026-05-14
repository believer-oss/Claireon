// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_Close.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSafeExec.h"
#include "ClaireonSessionManager.h"
#include "LevelSequence.h"
#include "UObject/Package.h"
#include "ScopedTransaction.h"
#include "FileHelpers.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_Close::GetOperation() const { return TEXT("sequence_close"); }

FString ClaireonLevelSequenceTool_Close::GetDescription() const
{
	return TEXT("Close a Level Sequence editing session. Optionally save the asset before closing.");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_Close::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	Builder.AddBoolean(TEXT("save"), TEXT("If true, save the sequence before closing."));
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_Close::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FSequenceEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	bool bSave = false;
	Arguments->TryGetBoolField(TEXT("save"), bSave);

	if (bSave && Data->IsValid())
	{
		UPackage* Package = Data->Sequence->GetOutermost();
		if (Package && !ClaireonSafeExec::DidLastExecutionCrash())
		{
			FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Save Level Sequence")));
			UEditorLoadingAndSavingUtils::SavePackages({ Package }, false);
		}
	}

	FClaireonSessionManager::Get().CloseSession(SessionId);
	ToolData.Remove(SessionId);

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("session_id"), SessionId);
	Response->SetBoolField(TEXT("saved"), bSave);
	return MakeSuccessResult(Response, FString::Printf(TEXT("Session %s closed%s"),
		*SessionId, bSave ? TEXT(" (saved)") : TEXT("")));
}
