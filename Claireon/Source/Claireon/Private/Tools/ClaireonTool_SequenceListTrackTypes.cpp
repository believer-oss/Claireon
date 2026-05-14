// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_SequenceListTrackTypes.h"
#include "Tools/ClaireonSequenceHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AssertionMacros.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneVisibilityTrack.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Tracks/MovieSceneAudioTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"

// ---------------------------------------------------------------------------
// F3 track-type table
// ---------------------------------------------------------------------------
//
// Hard-coded table mirroring FClaireonSequenceHelpers::ResolveTrackClass.
// The F3 symmetry test (ClaireonLevelSequenceTests.cpp) asserts that each
// entry's `name` resolves via ResolveTrackClass() back to the same UClass as
// its `uclass` path field, catching drift between this table and the helper.
//
// If a future feature adds more track types (media, subtitle, sub-sequence,
// control rig, etc.), extend BOTH this table AND ResolveTrackClass in the
// same PR.
// ---------------------------------------------------------------------------

namespace
{
	struct FTrackTypeEntry
	{
		const TCHAR* Name;
		const TCHAR* UClassPath;
		const TCHAR* BindingContext;
		// Up to two accepted section types -- all current v1 entries have exactly one.
		const TCHAR* AcceptedSectionType0;
		const TCHAR* AcceptedSectionType1;
	};

	static const FTrackTypeEntry GTrackTypeTable[] = {
		{
			TEXT("transform"),
			TEXT("/Script/MovieSceneTracks.MovieScene3DTransformTrack"),
			TEXT("possessable"),
			TEXT("/Script/MovieSceneTracks.MovieScene3DTransformSection"),
			nullptr,
		},
		{
			TEXT("visibility"),
			TEXT("/Script/MovieSceneTracks.MovieSceneVisibilityTrack"),
			TEXT("possessable"),
			// UMovieSceneVisibilitySection subclasses UMovieSceneBoolSection (which
			// lives in /Script/MovieScene, not /Script/MovieSceneTracks). The
			// visibility track emits VisibilitySection instances, so advertise the
			// concrete subclass whose module matches the track's module.
			TEXT("/Script/MovieSceneTracks.MovieSceneVisibilitySection"),
			nullptr,
		},
		{
			TEXT("event"),
			TEXT("/Script/MovieSceneTracks.MovieSceneEventTrack"),
			TEXT("root"),
			TEXT("/Script/MovieSceneTracks.MovieSceneEventSection"),
			nullptr,
		},
		{
			TEXT("audio"),
			TEXT("/Script/MovieSceneTracks.MovieSceneAudioTrack"),
			TEXT("possessable_or_root"),
			TEXT("/Script/MovieSceneTracks.MovieSceneAudioSection"),
			nullptr,
		},
		{
			TEXT("camera_cut"),
			TEXT("/Script/MovieSceneTracks.MovieSceneCameraCutTrack"),
			TEXT("root"),
			TEXT("/Script/MovieSceneTracks.MovieSceneCameraCutSection"),
			nullptr,
		},
		{
			TEXT("float"),
			TEXT("/Script/MovieSceneTracks.MovieSceneFloatTrack"),
			TEXT("possessable"),
			TEXT("/Script/MovieSceneTracks.MovieSceneFloatSection"),
			nullptr,
		},
	};
}

FString ClaireonTool_SequenceListTrackTypes::GetCategory() const { return TEXT("level"); }
FString ClaireonTool_SequenceListTrackTypes::GetOperation() const { return TEXT("sequence_list_track_types"); }

FString ClaireonTool_SequenceListTrackTypes::GetDescription() const
{
	return TEXT("Enumerate Claireon-supported Level Sequence track types. "
				"Stateless; returns the canonical list of track-type name -> UClass path, "
				"binding context requirement, and accepted section types.");
}

FString ClaireonTool_SequenceListTrackTypes::GetFullDescription() const
{
	return TEXT(
		"Enumerate Claireon-supported Level Sequence track types.\n"
		"\n"
		"Canonical list Claireon accepts for `sequence_edit.add_track` (F2) and for\n"
		"ClaireonSpecApplicator_LevelSequence specs (F6). Stateless, no input fields.\n"
		"\n"
		"Each entry has:\n"
		"  - name: short string used by sequence_edit.add_track (e.g. \"transform\").\n"
		"  - uclass: /Script/... class path for the track UClass.\n"
		"  - binding_context: one of \"root\", \"possessable\", or \"possessable_or_root\".\n"
		"      * \"root\"                 -- added directly on UMovieScene (event, camera_cut)\n"
		"      * \"possessable\"          -- requires a possessable/spawnable parent (transform, visibility, float)\n"
		"      * \"possessable_or_root\"  -- valid in both contexts (audio)\n"
		"  - accepted_section_types: list of /Script/... class paths for sections that\n"
		"      may be added to this track.\n"
		"\n"
		"Example output:\n"
		"  {\n"
		"    \"track_types\": [\n"
		"      {\n"
		"        \"name\": \"transform\",\n"
		"        \"uclass\": \"/Script/MovieSceneTracks.MovieScene3DTransformTrack\",\n"
		"        \"binding_context\": \"possessable\",\n"
		"        \"accepted_section_types\": [\"/Script/MovieSceneTracks.MovieScene3DTransformSection\"]\n"
		"      },\n"
		"      ...\n"
		"    ]\n"
		"  }\n"
	);
}

TSharedPtr<FJsonObject> ClaireonTool_SequenceListTrackTypes::GetInputSchema() const
{
	// No input fields; empty object schema.
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	Schema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_SequenceListTrackTypes::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> TrackTypesArr;

	for (const FTrackTypeEntry& Entry : GTrackTypeTable)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Entry.Name);
		Obj->SetStringField(TEXT("uclass"), Entry.UClassPath);
		Obj->SetStringField(TEXT("binding_context"), Entry.BindingContext);

		TArray<TSharedPtr<FJsonValue>> SectionTypes;
		if (Entry.AcceptedSectionType0 != nullptr)
		{
			SectionTypes.Add(MakeShared<FJsonValueString>(Entry.AcceptedSectionType0));
		}
		if (Entry.AcceptedSectionType1 != nullptr)
		{
			SectionTypes.Add(MakeShared<FJsonValueString>(Entry.AcceptedSectionType1));
		}
		Obj->SetArrayField(TEXT("accepted_section_types"), SectionTypes);

		// Anti-drift guard: the helper must agree with this table on what UClass
		// each name resolves to. The F3 symmetry test asserts this too; runtime
		// `ensure` gives early detection if someone edits one side and not the
		// other.
		UClass* HelperResolved = FClaireonSequenceHelpers::ResolveTrackClass(Entry.Name);
		UClass* TableResolved = FindObject<UClass>(nullptr, Entry.UClassPath);
		ensureMsgf(HelperResolved != nullptr && HelperResolved == TableResolved,
			TEXT("F3 track-type table / ResolveTrackClass drift for name='%s': helper=%s table=%s"),
			Entry.Name,
			HelperResolved ? *HelperResolved->GetPathName() : TEXT("null"),
			TableResolved ? *TableResolved->GetPathName() : TEXT("null"));

		TrackTypesArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	Data->SetArrayField(TEXT("track_types"), TrackTypesArr);

	const FString Summary = FString::Printf(TEXT("%d track types"), TrackTypesArr.Num());
	return MakeSuccessResult(Data, Summary);
}
