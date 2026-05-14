// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_FocusBinding.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLevelSequenceEditInternal.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_FocusBinding::GetOperation() const { return TEXT("sequence_focus_binding"); }

FString ClaireonLevelSequenceTool_FocusBinding::GetDescription() const
{
	return TEXT("Focus a binding within the sequence (by label or guid). Subsequent track/section/keyframe "
				"operations target this binding.");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_FocusBinding::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	Builder.AddString(TEXT("label"), TEXT("Binding label to focus (provide this OR guid)."));
	Builder.AddString(TEXT("guid"), TEXT("Binding GUID to focus (provide this OR label)."));
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("If true, returns brief status instead of full state."));
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_FocusBinding::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	UMovieScene* MovieScene = Data->Sequence->GetMovieScene();
	FString Label, GuidStr;
	Arguments->TryGetStringField(TEXT("label"), Label);
	Arguments->TryGetStringField(TEXT("guid"), GuidStr);

	int32 Index = INDEX_NONE;
	FGuid Guid;
	if (!ClaireonLevelSequenceInternal::FindBindingByLabelOrGuid(MovieScene, Label, GuidStr, Index, Guid, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->PushHistory();
	Data->FocusedBindingIndex = Index;
	Data->FocusedTrackIndex = INDEX_NONE;
	Data->LastOperationStatus = FString::Printf(TEXT("Focused binding [%d] %s"),
		Index, *MovieScene->GetBindings()[Index].GetName());
	return BuildStateResponse(SessionId, Data);
}
