// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_GetState.h"
#include "Tools/FToolSchemaBuilder.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_GetState::GetName() const
{
	return TEXT("claireon.level_sequence_get_state");
}

FString ClaireonLevelSequenceTool_GetState::GetDescription() const
{
	return TEXT("Return the current state of a Level Sequence editing session (bindings, tracks, sections, keyframes).");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_GetState::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_GetState::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FSequenceEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}
	Data->bSuppressOutput = false;
	return BuildStateResponse(SessionId, Data);
}
