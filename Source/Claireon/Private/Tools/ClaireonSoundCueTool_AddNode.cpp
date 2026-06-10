// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundCueTool_AddNode.h"
#include "Tools/ClaireonAudioHelpers.h"
#include "Tools/ClaireonAudioSessionRegistry.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

namespace
{
	USoundNode* AddNode_NodeAt(USoundCue* Cue, int32 Index)
	{
#if WITH_EDITORONLY_DATA
		if (!Cue || !Cue->AllNodes.IsValidIndex(Index)) return nullptr;
		return Cue->AllNodes[Index];
#else
		return nullptr;
#endif
	}
	int32 AddNode_IndexOfNode(USoundCue* Cue, const USoundNode* Node)
	{
#if WITH_EDITORONLY_DATA
		if (!Cue || !Node) return INDEX_NONE;
		for (int32 i = 0; i < Cue->AllNodes.Num(); ++i)
		{
			if (Cue->AllNodes[i] == Node) return i;
		}
#endif
		return INDEX_NONE;
	}
}

FString FClaireonSoundCueTool_AddNode::GetCategory() const { return TEXT("soundcue"); }
FString FClaireonSoundCueTool_AddNode::GetOperation() const { return TEXT("add_node"); }

FString FClaireonSoundCueTool_AddNode::GetDescription() const
{
	return TEXT("Add a USoundNode subclass to the SoundCue within the current session. Syncs the "
				"runtime tree, EdGraph node, and graph position in a single FScopedTransaction (I5); "
				"default position is +300 from the focused node. Requires session_id from "
				"soundcue.open. Updates the session's focused node to the newly created one.");
}

TSharedPtr<FJsonObject> FClaireonSoundCueTool_AddNode::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session id returned by soundcue_open"), true);
	S.AddString(TEXT("node_class"), TEXT("Short name of the USoundNode subclass (e.g. 'wave_player', 'mixer')"), true);
	S.AddInteger(TEXT("pos_x"), TEXT("Optional explicit X position"));
	S.AddInteger(TEXT("pos_y"), TEXT("Optional explicit Y position"));
	S.AddBoolean(TEXT("make_root"), TEXT("If true, set this node as Cue->FirstNode"));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundCueTool_AddNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString ClassShort;
	if (!Arguments->TryGetStringField(TEXT("node_class"), ClassShort) || ClassShort.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing node_class (short name, e.g. 'wave_player', 'mixer')"));
	}
	UClass* NodeClass = ClaireonAudioHelpers::ResolveSoundNodeClass(FName(*ClassShort));
	if (!NodeClass)
	{
		return MakeErrorResult(FString::Printf(TEXT("Unknown sound node class '%s' (use list_node_types)"), *ClassShort));
	}

	int32 PosX = 0;
	int32 PosY = 0;
	bool bHasExplicitPos = false;
	{
		double Dx = 0, Dy = 0;
		if (Arguments->TryGetNumberField(TEXT("pos_x"), Dx)) { PosX = static_cast<int32>(Dx); bHasExplicitPos = true; }
		if (Arguments->TryGetNumberField(TEXT("pos_y"), Dy)) { PosY = static_cast<int32>(Dy); bHasExplicitPos = true; }
	}
	if (!bHasExplicitPos)
	{
		if (USoundNode* Focus = AddNode_NodeAt(Cue, Data->FocusedNodeIndex))
		{
			if (Focus->GraphNode)
			{
				PosX = Focus->GraphNode->NodePosX + 300;
				PosY = Focus->GraphNode->NodePosY;
			}
		}
	}

	bool bMakeRoot = false;
	Arguments->TryGetBoolField(TEXT("make_root"), bMakeRoot);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add SoundCue Node")));
	Cue->Modify();
#if WITH_EDITORONLY_DATA
	if (Cue->SoundCueGraph) Cue->SoundCueGraph->Modify();
#endif

	USoundNode* NewNode = Cue->ConstructSoundNode<USoundNode>(NodeClass, /*bSelectNewNode=*/false);
	if (!NewNode)
	{
		Transaction.Cancel();
		return MakeErrorResult(TEXT("ConstructSoundNode returned null"));
	}
	if (NewNode->GraphNode)
	{
		NewNode->GraphNode->Modify();
		NewNode->GraphNode->NodePosX = PosX;
		NewNode->GraphNode->NodePosY = PosY;
	}
	if (bMakeRoot) Cue->FirstNode = NewNode;
	Cue->LinkGraphNodesFromSoundNodes();

	const int32 NodeIndex = AddNode_IndexOfNode(Cue, NewNode);
	Data->FocusedNodeIndex = NodeIndex;
	Data->bDirty = true;
	Data->LastOperationStatus = FString::Printf(TEXT("Added %s at index %d (pos %d, %d)"),
		*ClassShort, NodeIndex, PosX, PosY);

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("session_id"), SessionId);
	Out->SetNumberField(TEXT("node_index"), NodeIndex);
	Out->SetStringField(TEXT("node_class"), ClassShort);
	Out->SetNumberField(TEXT("pos_x"), PosX);
	Out->SetNumberField(TEXT("pos_y"), PosY);
	return MakeSuccessResult(Out, Data->LastOperationStatus);
}
