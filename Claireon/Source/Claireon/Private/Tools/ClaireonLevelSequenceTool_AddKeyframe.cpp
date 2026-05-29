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

FString ClaireonLevelSequenceTool_AddKeyframe::GetOperation() const { return TEXT("add_keyframe"); }

FString ClaireonLevelSequenceTool_AddKeyframe::GetDescription() const
{
	// Payload shape depends on the section's channel layout (introspect via the
	// focused track's section in the structure dump). 3D Transform (9-channel) sections
	// accept both compact-array and per-axis forms.
	return TEXT("Insert a keyframe on the focused section. Payload shape depends on channel type:\n"
				"  - Float channel: number, or {\"value\": <float>}\n"
				"  - Bool channel: true/false, or {\"value\": <bool>}\n"
				"  - Vector2D/Vector (2 or 3 double channels): [x,y[,z]], {x:..,y:..[,z:..]}, or {\"value\":[..]}\n"
				"  - 3D Transform (6 or 9 double channels): compact {\"location\":[x,y,z],\"rotation\":[p,y,r],\"scale\":[sx,sy,sz]}, "
				"OR per-axis {\"translation_x\":.., \"translation_y\":.., \"translation_z\":.., "
				"\"rotation_x\":.., \"rotation_y\":.., \"rotation_z\":.., \"scale_x\":.., \"scale_y\":.., \"scale_z\":..}. "
				"Per-axis keys allow writing a single sub-channel; scale fields are accepted only on 9-channel sections.");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_AddKeyframe::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	Builder.AddInteger(TEXT("frame"), TEXT("Frame number for the keyframe (tick units; raw MovieScene frame, not display-rate)."), true);
	Builder.AddInteger(TEXT("section_index"), TEXT("Optional explicit section index (defaults to first section on focused track)."));
	Builder.AddObject(TEXT("value"), TEXT("Keyframe payload (see operation description for per-channel shape). 3D Transform accepts compact arrays or per-axis translation_x/y/z, rotation_x/y/z, scale_x/y/z."), true);
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
	if (!Claireon::SequenceEdit::ApplyAddKeyframe(Section, FFrameNumber(Frame), ValueJson, Error))
	{
		return MakeErrorResult(Error);
	}
	Section->MarkAsChanged();
	ClaireonLevelSequenceInternal::MarkMutated(Data->Sequence.Get());

	Data->LastOperationStatus = FString::Printf(TEXT("Added keyframe at frame %d"), Frame);
	return BuildStateResponse(SessionId, Data);
}
