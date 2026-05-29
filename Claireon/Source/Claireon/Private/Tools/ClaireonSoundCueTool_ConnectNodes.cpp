// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundCueTool_ConnectNodes.h"
#include "Tools/ClaireonAudioSessionRegistry.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "EdGraph/EdGraph.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

FString FClaireonSoundCueTool_ConnectNodes::GetCategory() const { return TEXT("soundcue"); }
FString FClaireonSoundCueTool_ConnectNodes::GetOperation() const { return TEXT("connect_nodes"); }

FString FClaireonSoundCueTool_ConnectNodes::GetDescription() const
{
	return TEXT("Connect a child sound node into a parent node slot within the current session. "
				"Sets parent.ChildNodes[slot] on the runtime tree and mirrors the link onto the "
				"EdGraph in one FScopedTransaction. Requires session_id from soundcue.open.");
}

TSharedPtr<FJsonObject> FClaireonSoundCueTool_ConnectNodes::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session id returned by soundcue_open"), true);
	S.AddInteger(TEXT("parent_index"), TEXT("Index of the parent node"), true);
	S.AddInteger(TEXT("child_index"), TEXT("Index of the child node"), true);
	S.AddInteger(TEXT("slot"), TEXT("Optional ChildNodes slot (default 0)"));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundCueTool_ConnectNodes::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	int32 ParentIdx = INDEX_NONE, ChildIdx = INDEX_NONE, Slot = 0;
	if (!Arguments->TryGetNumberField(TEXT("parent_index"), ParentIdx)) return MakeErrorResult(TEXT("Missing parent_index"));
	if (!Arguments->TryGetNumberField(TEXT("child_index"), ChildIdx))   return MakeErrorResult(TEXT("Missing child_index"));
	Arguments->TryGetNumberField(TEXT("slot"), Slot);

#if WITH_EDITORONLY_DATA
	if (!Cue->AllNodes.IsValidIndex(ParentIdx)) return MakeErrorResult(FString::Printf(TEXT("parent_index %d out of range"), ParentIdx));
	if (!Cue->AllNodes.IsValidIndex(ChildIdx))  return MakeErrorResult(FString::Printf(TEXT("child_index %d out of range"), ChildIdx));
	USoundNode* Parent = Cue->AllNodes[ParentIdx];
	USoundNode* Child  = Cue->AllNodes[ChildIdx];
	if (!Parent || !Child) return MakeErrorResult(TEXT("Parent or child node is null"));
	if (Parent == Child) return MakeErrorResult(TEXT("Cannot connect a node to itself"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Connect SoundCue Nodes")));
	Cue->Modify();
	if (Cue->SoundCueGraph) Cue->SoundCueGraph->Modify();
	Parent->Modify();
	while (Parent->ChildNodes.Num() <= Slot)
	{
		Parent->InsertChildNode(Parent->ChildNodes.Num());
	}
	Parent->ChildNodes[Slot] = Child;
	Cue->LinkGraphNodesFromSoundNodes();
#endif

	Data->bDirty = true;
	Data->LastOperationStatus = FString::Printf(TEXT("Connected parent=%d child=%d slot=%d"),
		ParentIdx, ChildIdx, Slot);

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("session_id"), SessionId);
	Out->SetNumberField(TEXT("parent_index"), ParentIdx);
	Out->SetNumberField(TEXT("child_index"), ChildIdx);
	Out->SetNumberField(TEXT("slot"), Slot);
	return MakeSuccessResult(Out, Data->LastOperationStatus);
}
