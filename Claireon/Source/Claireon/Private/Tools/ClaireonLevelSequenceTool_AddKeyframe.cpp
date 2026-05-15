// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_AddKeyframe.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLevelSequenceEditInternal.h"
#include "ClaireonSequenceEditHandlers.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_AddKeyframe::GetName() const
{
	return TEXT("claireon.level_sequence_add_keyframe");
}

FString ClaireonLevelSequenceTool_AddKeyframe::GetDescription() const
{
	return TEXT("Insert a keyframe on the focused section. Payload shape depends on channel type "
				"(scalar, bool, or transform object).");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_AddKeyframe::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	Builder.AddInteger(TEXT("frame"), TEXT("Frame number for the keyframe."), true);
	Builder.AddInteger(TEXT("section_index"), TEXT("Optional explicit section index (defaults to first section on focused track)."));
	Builder.AddObject(TEXT("value"), TEXT("Keyframe payload. Float: number. Bool: true/false. Transform: {location:[x,y,z],rotation:[p,y,r]}."), true);
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("If true, returns brief status instead of full state."));
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_AddKeyframe::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	int32 Frame = 0;
	if (!Arguments->TryGetNumberField(TEXT("frame"), Frame))
	{
		return MakeErrorResult(TEXT("Missing required parameter: frame"));
	}
	int32 SectionIndex = -1;
	int32 SecVal = 0;
	if (Arguments->TryGetNumberField(TEXT("section_index"), SecVal))
	{
		SectionIndex = SecVal;
	}

	// The value payload may be a JSON object, string, or number. Serialize it
	// back to a string for ApplyAddKeyframe's coercion path.
	FString ValueJson;
	{
		const TSharedPtr<FJsonObject>* ValueObj = nullptr;
		if (Arguments->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && ValueObj->IsValid())
		{
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ValueJson);
			FJsonSerializer::Serialize(ValueObj->ToSharedRef(), Writer);
		}
		else
		{
			const TSharedPtr<FJsonValue> Val = Arguments->TryGetField(TEXT("value"));
			if (Val.IsValid())
			{
				double NumVal = 0.0;
				FString StrVal;
				bool BoolVal = false;
				if (Val->TryGetNumber(NumVal))
				{
					ValueJson = FString::SanitizeFloat(NumVal);
				}
				else if (Val->TryGetBool(BoolVal))
				{
					ValueJson = BoolVal ? TEXT("true") : TEXT("false");
				}
				else if (Val->TryGetString(StrVal))
				{
					ValueJson = StrVal;
				}
			}
		}
	}
	if (ValueJson.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	UMovieSceneSection* Section = ClaireonLevelSequenceInternal::ResolveFocusedSection(Data, SectionIndex, Error);
	if (!Section)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Keyframe")));
	if (!ApplyAddKeyframe(Section, FFrameNumber(Frame), ValueJson, Error))
	{
		return MakeErrorResult(Error);
	}
	Section->MarkAsChanged();
	ClaireonLevelSequenceInternal::MarkMutated(Data->Sequence.Get());

	Data->LastOperationStatus = FString::Printf(TEXT("Added keyframe at frame %d"), Frame);
	return BuildStateResponse(SessionId, Data);
}
