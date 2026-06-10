// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundCueTool_SetNodePosition.h"
#include "Tools/ClaireonAudioSessionRegistry.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "EdGraph/EdGraphNode.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

FString FClaireonSoundCueTool_SetNodePosition::GetCategory() const { return TEXT("soundcue"); }
FString FClaireonSoundCueTool_SetNodePosition::GetOperation() const { return TEXT("set_node_position"); }

FString FClaireonSoundCueTool_SetNodePosition::GetDescription() const
{
	return TEXT("Set the canvas position (NodePosX/NodePosY) of a sound node in the EdGraph "
				"within the current session. Does not affect runtime behavior; used to keep "
				"the SoundCue graph readable. Requires session_id and a valid node_index.");
}

TSharedPtr<FJsonObject> FClaireonSoundCueTool_SetNodePosition::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session id returned by soundcue_open"), true);
	S.AddInteger(TEXT("node_index"), TEXT("Index into Cue->AllNodes"), true);
	S.AddInteger(TEXT("pos_x"), TEXT("X position"), true);
	S.AddInteger(TEXT("pos_y"), TEXT("Y position"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundCueTool_SetNodePosition::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	if (!Arguments->TryGetNumberField(TEXT("node_index"), NodeIndex)) return MakeErrorResult(TEXT("Missing node_index"));
	double Dx = 0, Dy = 0;
	if (!Arguments->TryGetNumberField(TEXT("pos_x"), Dx)) return MakeErrorResult(TEXT("Missing pos_x"));
	if (!Arguments->TryGetNumberField(TEXT("pos_y"), Dy)) return MakeErrorResult(TEXT("Missing pos_y"));

#if WITH_EDITORONLY_DATA
	if (!Cue->AllNodes.IsValidIndex(NodeIndex)) return MakeErrorResult(FString::Printf(TEXT("node_index %d out of range"), NodeIndex));
	USoundNode* Node = Cue->AllNodes[NodeIndex];
	if (!Node) return MakeErrorResult(FString::Printf(TEXT("node_index %d is null"), NodeIndex));
	if (!Node->GraphNode) return MakeErrorResult(TEXT("Node has no EdGraph counterpart"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set SoundCue Node Position")));
	Node->GraphNode->Modify();
	Node->GraphNode->NodePosX = static_cast<int32>(Dx);
	Node->GraphNode->NodePosY = static_cast<int32>(Dy);
#endif

	Data->bDirty = true;
	Data->LastOperationStatus = FString::Printf(TEXT("Set node[%d] position (%d, %d)"),
		NodeIndex, (int32)Dx, (int32)Dy);

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("session_id"), SessionId);
	Out->SetNumberField(TEXT("node_index"), NodeIndex);
	Out->SetNumberField(TEXT("pos_x"), Dx);
	Out->SetNumberField(TEXT("pos_y"), Dy);
	return MakeSuccessResult(Out, Data->LastOperationStatus);
}
