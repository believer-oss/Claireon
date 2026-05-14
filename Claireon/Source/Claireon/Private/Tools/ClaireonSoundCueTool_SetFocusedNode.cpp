// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundCueTool_SetFocusedNode.h"
#include "Tools/ClaireonAudioSessionRegistry.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Sound/SoundCue.h"
#include "Dom/JsonObject.h"

FString FClaireonSoundCueTool_SetFocusedNode::GetCategory() const { return TEXT("soundcue"); }
FString FClaireonSoundCueTool_SetFocusedNode::GetOperation() const { return TEXT("set_focused_node"); }

FString FClaireonSoundCueTool_SetFocusedNode::GetDescription() const
{
	return TEXT("Set the session's focused-node index. Used by add_node default placement (offset +300 from focused).");
}

TSharedPtr<FJsonObject> FClaireonSoundCueTool_SetFocusedNode::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session id returned by soundcue_open"), true);
	S.AddInteger(TEXT("node_index"), TEXT("Optional: omit to clear focus. -1 also clears."));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundCueTool_SetFocusedNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid()) return MakeErrorResult(TEXT("Arguments object missing"));
	FString SessionId;
	if (!Arguments->TryGetStringField(TEXT("session_id"), SessionId) || SessionId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: session_id"));
	}
	FAudioEditToolData* Data = ClaireonAudioSessionRegistry::FindSession(SessionId, ESoundCohort::SoundCue);
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("SoundCue session not found: %s"), *SessionId));
	}
	USoundCue* Cue = Cast<USoundCue>(Data->Asset.Get());
	if (!Cue) return MakeErrorResult(TEXT("Session asset is not a SoundCue"));

	int32 NodeIndex = INDEX_NONE;
	if (!Arguments->TryGetNumberField(TEXT("node_index"), NodeIndex))
	{
		Data->FocusedNodeIndex = INDEX_NONE;
		Data->LastOperationStatus = TEXT("Cleared focused node");
	}
	else
	{
#if WITH_EDITORONLY_DATA
		if (NodeIndex != INDEX_NONE && !Cue->AllNodes.IsValidIndex(NodeIndex))
		{
			return MakeErrorResult(FString::Printf(TEXT("node_index %d out of range"), NodeIndex));
		}
#endif
		Data->FocusedNodeIndex = NodeIndex;
		Data->LastOperationStatus = FString::Printf(TEXT("Focused node %d"), NodeIndex);
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("session_id"), SessionId);
	Out->SetNumberField(TEXT("focused_node_index"), Data->FocusedNodeIndex);
	return MakeSuccessResult(Out, Data->LastOperationStatus);
}
