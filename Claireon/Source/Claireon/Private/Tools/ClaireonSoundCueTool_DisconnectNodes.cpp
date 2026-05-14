// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundCueTool_DisconnectNodes.h"
#include "Tools/ClaireonAudioSessionRegistry.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "EdGraph/EdGraph.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

FString FClaireonSoundCueTool_DisconnectNodes::GetCategory() const { return TEXT("soundcue"); }
FString FClaireonSoundCueTool_DisconnectNodes::GetOperation() const { return TEXT("disconnect_nodes"); }

FString FClaireonSoundCueTool_DisconnectNodes::GetDescription() const
{
	return TEXT("Clear parent.ChildNodes[slot] on the SoundCue and mirror onto the EdGraph (I5).");
}

TSharedPtr<FJsonObject> FClaireonSoundCueTool_DisconnectNodes::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session id returned by soundcue_open"), true);
	S.AddInteger(TEXT("parent_index"), TEXT("Index of the parent node"), true);
	S.AddInteger(TEXT("slot"), TEXT("Optional ChildNodes slot (default 0)"));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundCueTool_DisconnectNodes::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	int32 ParentIdx = INDEX_NONE, Slot = 0;
	if (!Arguments->TryGetNumberField(TEXT("parent_index"), ParentIdx)) return MakeErrorResult(TEXT("Missing parent_index"));
	Arguments->TryGetNumberField(TEXT("slot"), Slot);

#if WITH_EDITORONLY_DATA
	if (!Cue->AllNodes.IsValidIndex(ParentIdx)) return MakeErrorResult(FString::Printf(TEXT("parent_index %d out of range"), ParentIdx));
	USoundNode* Parent = Cue->AllNodes[ParentIdx];
	if (!Parent) return MakeErrorResult(TEXT("Parent node is null"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Disconnect SoundCue Nodes")));
	Cue->Modify();
	if (Cue->SoundCueGraph) Cue->SoundCueGraph->Modify();
	Parent->Modify();
	if (!Parent->ChildNodes.IsValidIndex(Slot))
	{
		Transaction.Cancel();
		return MakeErrorResult(FString::Printf(TEXT("slot %d out of range for node %d (has %d children)"),
			Slot, ParentIdx, Parent->ChildNodes.Num()));
	}
	Parent->ChildNodes[Slot] = nullptr;
	Cue->LinkGraphNodesFromSoundNodes();
#endif

	Data->bDirty = true;
	Data->LastOperationStatus = FString::Printf(TEXT("Disconnected parent=%d slot=%d"), ParentIdx, Slot);

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("session_id"), SessionId);
	Out->SetNumberField(TEXT("parent_index"), ParentIdx);
	Out->SetNumberField(TEXT("slot"), Slot);
	return MakeSuccessResult(Out, Data->LastOperationStatus);
}
