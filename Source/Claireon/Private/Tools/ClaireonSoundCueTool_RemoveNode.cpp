// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundCueTool_RemoveNode.h"
#include "Tools/ClaireonAudioSessionRegistry.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

FString FClaireonSoundCueTool_RemoveNode::GetCategory() const { return TEXT("soundcue"); }
FString FClaireonSoundCueTool_RemoveNode::GetOperation() const { return TEXT("remove_node"); }

FString FClaireonSoundCueTool_RemoveNode::GetDescription() const
{
	return TEXT("Remove a node from the SoundCue within the current session. Clears any ChildNodes "
				"references that pointed to it, breaks EdGraph pin links, removes the EdGraph node, "
				"and re-links the graph in a single FScopedTransaction (I5). Requires session_id "
				"from soundcue.open. Clears the focused-node index if it referenced the removed node.");
}

TSharedPtr<FJsonObject> FClaireonSoundCueTool_RemoveNode::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session id returned by soundcue_open"), true);
	S.AddInteger(TEXT("node_index"), TEXT("Index into Cue->AllNodes of the node to remove"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundCueTool_RemoveNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}
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
		return MakeErrorResult(TEXT("Missing node_index"));
	}
#if WITH_EDITORONLY_DATA
	if (!Cue->AllNodes.IsValidIndex(NodeIndex))
	{
		return MakeErrorResult(FString::Printf(TEXT("node_index %d out of range"), NodeIndex));
	}
	USoundNode* Victim = Cue->AllNodes[NodeIndex];
	if (!Victim)
	{
		return MakeErrorResult(FString::Printf(TEXT("node_index %d is null"), NodeIndex));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove SoundCue Node")));
	Cue->Modify();
	if (Cue->SoundCueGraph) Cue->SoundCueGraph->Modify();
	for (USoundNode* N : Cue->AllNodes)
	{
		if (!N || N == Victim) continue;
		N->Modify();
		for (int32 i = 0; i < N->ChildNodes.Num(); ++i)
		{
			if (N->ChildNodes[i] == Victim) N->ChildNodes[i] = nullptr;
		}
	}
	if (Cue->FirstNode == Victim) Cue->FirstNode = nullptr;
	if (UEdGraphNode* GN = Victim->GraphNode)
	{
		GN->Modify();
		GN->BreakAllNodeLinks();
		if (Cue->SoundCueGraph)
		{
			Cue->SoundCueGraph->RemoveNode(GN);
		}
	}
	Cue->AllNodes.RemoveSingle(Victim);
	Cue->LinkGraphNodesFromSoundNodes();
#endif

	if (Data->FocusedNodeIndex == NodeIndex) Data->FocusedNodeIndex = INDEX_NONE;
	Data->bDirty = true;
	Data->LastOperationStatus = FString::Printf(TEXT("Removed node %d"), NodeIndex);

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("session_id"), SessionId);
	Out->SetNumberField(TEXT("removed_index"), NodeIndex);
	return MakeSuccessResult(Out, Data->LastOperationStatus);
}
