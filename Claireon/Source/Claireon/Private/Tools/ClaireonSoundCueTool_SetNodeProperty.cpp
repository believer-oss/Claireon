// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundCueTool_SetNodeProperty.h"
#include "Tools/ClaireonAudioHelpers.h"
#include "Tools/ClaireonAudioSessionRegistry.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString FClaireonSoundCueTool_SetNodeProperty::GetCategory() const { return TEXT("soundcue"); }
FString FClaireonSoundCueTool_SetNodeProperty::GetOperation() const { return TEXT("set_node_property"); }

FString FClaireonSoundCueTool_SetNodeProperty::GetDescription() const
{
	return TEXT("Set a USoundNode field by reflection (PreEditChange/PostEditChange fire, I7).");
}

TSharedPtr<FJsonObject> FClaireonSoundCueTool_SetNodeProperty::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session id returned by soundcue_open"), true);
	S.AddInteger(TEXT("node_index"), TEXT("Index into Cue->AllNodes"), true);
	S.AddString(TEXT("field_name"), TEXT("Name of the field on the USoundNode subclass"), true);
	S.AddString(TEXT("value"), TEXT("Value to write"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundCueTool_SetNodeProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	FString FieldName;
	if (!Arguments->TryGetStringField(TEXT("field_name"), FieldName) || FieldName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing field_name"));
	}
#if WITH_EDITORONLY_DATA
	if (!Cue->AllNodes.IsValidIndex(NodeIndex)) return MakeErrorResult(FString::Printf(TEXT("node_index %d out of range"), NodeIndex));
	USoundNode* Node = Cue->AllNodes[NodeIndex];
	if (!Node) return MakeErrorResult(FString::Printf(TEXT("node_index %d is null"), NodeIndex));
#else
	return MakeErrorResult(TEXT("SoundCue editing requires editor data"));
#endif

	const TSharedPtr<FJsonValue> ValueJson = Arguments->TryGetField(TEXT("value"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set SoundCue Node Property")));
	Cue->Modify();
#if WITH_EDITORONLY_DATA
	Node->Modify();
	FString Err;
	if (!ClaireonAudioHelpers::SetSoundNodeProperty(Node, FName(*FieldName), ValueJson, Err))
	{
		return MakeErrorResult(Err);
	}
#endif

	Data->bDirty = true;
	Data->LastOperationStatus = FString::Printf(TEXT("Set node[%d].%s"), NodeIndex, *FieldName);

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("session_id"), SessionId);
	Out->SetNumberField(TEXT("node_index"), NodeIndex);
	Out->SetStringField(TEXT("field_name"), FieldName);
	return MakeSuccessResult(Out, Data->LastOperationStatus);
}
