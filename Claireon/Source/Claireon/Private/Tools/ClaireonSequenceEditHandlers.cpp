// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonSequenceEditHandlers.h"
#include "Tools/ClaireonSequenceHelpers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/DefaultValueHelper.h"

#include "LevelSequence.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneChannel.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneBoolChannel.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Channels/MovieSceneIntegerChannel.h"
#include "Curves/RichCurve.h"
#include "KeyParams.h"
#include "UObject/UnrealType.h"

// ============================================================================
// Apply* handlers -- free functions used by decomposed claireon.level_sequence_*
// tools and by FClaireonSpecApplicator_LevelSequence. Definitions extracted from
// the deleted ClaireonTool_SequenceEdit.cpp so the apply-handler surface (see
// FEATURE_6_CLAIREON_SPEC_APPLICATOR.md "Handler extraction") continues to link
// after decomposition.
// ============================================================================

namespace Claireon::SequenceEdit
{

bool ApplyAddPossessable(ULevelSequence* Sequence, FName Label, UClass* ObjectClass, FMovieSceneBinding& OutBinding, FString& OutError)
{
	if (!Sequence)
	{
		OutError = TEXT("sequence is null");
		return false;
	}
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		OutError = TEXT("sequence has no MovieScene");
		return false;
	}
	if (!ObjectClass)
	{
		OutError = TEXT("object_class is null");
		return false;
	}
	const FGuid Guid = MovieScene->AddPossessable(Label.ToString(), ObjectClass);
	if (!Guid.IsValid())
	{
		OutError = TEXT("AddPossessable failed");
		return false;
	}
	for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
	{
		if (Binding.GetObjectGuid() == Guid)
		{
			OutBinding = Binding;
			return true;
		}
	}
	OutError = TEXT("added possessable but could not find binding");
	return false;
}

bool ApplyRemovePossessable(ULevelSequence* Sequence, const FGuid& Guid, FString& OutError)
{
	if (!Sequence || !Sequence->GetMovieScene())
	{
		OutError = TEXT("sequence or MovieScene is null");
		return false;
	}
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (MovieScene->RemovePossessable(Guid))
	{
		return true;
	}
	if (MovieScene->RemoveSpawnable(Guid))
	{
		return true;
	}
	OutError = TEXT("binding with that GUID not found");
	return false;
}

bool ApplyAddTrack(ULevelSequence* Sequence, const FGuid& BindingGuid, UClass* TrackClass, UMovieSceneTrack*& OutTrack, FString& OutError)
{
	if (!Sequence || !Sequence->GetMovieScene())
	{
		OutError = TEXT("sequence or MovieScene is null");
		return false;
	}
	if (!TrackClass || !TrackClass->IsChildOf(UMovieSceneTrack::StaticClass()))
	{
		OutError = TEXT("track_class is invalid");
		return false;
	}
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (BindingGuid.IsValid())
	{
		OutTrack = MovieScene->AddTrack(TSubclassOf<UMovieSceneTrack>(TrackClass), BindingGuid);
	}
	else
	{
		OutTrack = MovieScene->AddTrack(TSubclassOf<UMovieSceneTrack>(TrackClass));
	}
	if (!OutTrack)
	{
		OutError = TEXT("AddTrack returned null");
		return false;
	}
	return true;
}

bool ApplyRemoveTrack(ULevelSequence* Sequence, const FGuid& BindingGuid, int32 TrackIndex, FString& OutError)
{
	if (!Sequence || !Sequence->GetMovieScene())
	{
		OutError = TEXT("sequence or MovieScene is null");
		return false;
	}
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!BindingGuid.IsValid())
	{
		const TArray<UMovieSceneTrack*>& Tracks = MovieScene->GetTracks();
		if (TrackIndex < 0 || TrackIndex >= Tracks.Num() || !Tracks[TrackIndex])
		{
			OutError = FString::Printf(TEXT("root track index %d out of range (tracks=%d)"), TrackIndex, Tracks.Num());
			return false;
		}
		if (!MovieScene->RemoveTrack(*Tracks[TrackIndex]))
		{
			OutError = TEXT("RemoveTrack failed");
			return false;
		}
		return true;
	}
	for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
	{
		if (Binding.GetObjectGuid() == BindingGuid)
		{
			const TArray<UMovieSceneTrack*>& Tracks = Binding.GetTracks();
			if (TrackIndex < 0 || TrackIndex >= Tracks.Num() || !Tracks[TrackIndex])
			{
				OutError = FString::Printf(TEXT("track index %d out of range (tracks=%d)"), TrackIndex, Tracks.Num());
				return false;
			}
			if (!MovieScene->RemoveTrack(*Tracks[TrackIndex]))
			{
				OutError = TEXT("RemoveTrack failed");
				return false;
			}
			return true;
		}
	}
	OutError = TEXT("binding not found for track removal");
	return false;
}

bool ApplyAddSection(UMovieSceneTrack* Track, FFrameNumber Start, FFrameNumber End, int32 RowIndex, UMovieSceneSection*& OutSection, FString& OutError)
{
	if (!Track)
	{
		OutError = TEXT("track is null");
		return false;
	}
	OutSection = Track->CreateNewSection();
	if (!OutSection)
	{
		OutError = TEXT("CreateNewSection returned null");
		return false;
	}
	OutSection->SetRange(TRange<FFrameNumber>(Start, End));
	if (RowIndex >= 0)
	{
		OutSection->SetRowIndex(RowIndex);
	}
	Track->AddSection(*OutSection);
	return true;
}

bool ApplyRemoveSection(UMovieSceneTrack* Track, int32 SectionIndex, FString& OutError)
{
	if (!Track)
	{
		OutError = TEXT("track is null");
		return false;
	}
	const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
	if (SectionIndex < 0 || SectionIndex >= Sections.Num() || !Sections[SectionIndex])
	{
		OutError = FString::Printf(TEXT("section index %d out of range (sections=%d)"), SectionIndex, Sections.Num());
		return false;
	}
	Track->RemoveSectionAt(SectionIndex);
	return true;
}

namespace ClaireonSequenceEditHandlersInternal
{

TSharedPtr<FJsonObject> ParseJsonObject(const FString& JsonPayload)
{
	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonPayload);
	if (FJsonSerializer::Deserialize(Reader, Obj))
	{
		return Obj;
	}
	return nullptr;
}

}  // namespace ClaireonSequenceEditHandlersInternal

bool ApplyAddKeyframe(UMovieSceneSection* Section, FFrameNumber Frame, const FString& ValueJson, FString& OutError)
{
	if (!Section)
	{
		OutError = TEXT("section is null");
		return false;
	}

	FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();

	TArrayView<FMovieSceneFloatChannel*> FloatChannels = Proxy.GetChannels<FMovieSceneFloatChannel>();
	if (FloatChannels.Num() > 0)
	{
		double ValueD = 0.0;
		const FString Trim = ValueJson.TrimStartAndEnd();
		bool bParsed = FDefaultValueHelper::ParseDouble(Trim, ValueD);
		if (!bParsed)
		{
			if (TSharedPtr<FJsonObject> Obj = ClaireonSequenceEditHandlersInternal::ParseJsonObject(ValueJson))
			{
				bParsed = Obj->TryGetNumberField(TEXT("value"), ValueD);
			}
		}
		if (!bParsed)
		{
			OutError = TEXT("float channel expects numeric payload or {\"value\": <float>}");
			return false;
		}
		FloatChannels[0]->GetData().UpdateOrAddKey(Frame, FMovieSceneFloatValue(static_cast<float>(ValueD)));
		return true;
	}

	TArrayView<FMovieSceneBoolChannel*> BoolChannels = Proxy.GetChannels<FMovieSceneBoolChannel>();
	if (BoolChannels.Num() > 0)
	{
		const FString Trim = ValueJson.TrimStartAndEnd();
		bool bValue = false;
		bool bParsed = false;
		if (Trim.Equals(TEXT("true"), ESearchCase::IgnoreCase)) { bValue = true; bParsed = true; }
		else if (Trim.Equals(TEXT("false"), ESearchCase::IgnoreCase)) { bValue = false; bParsed = true; }
		else if (TSharedPtr<FJsonObject> Obj = ClaireonSequenceEditHandlersInternal::ParseJsonObject(ValueJson))
		{
			bParsed = Obj->TryGetBoolField(TEXT("value"), bValue);
		}
		if (!bParsed)
		{
			OutError = TEXT("bool channel expects true/false or {\"value\": <bool>}");
			return false;
		}
		BoolChannels[0]->GetData().UpdateOrAddKey(Frame, bValue);
		return true;
	}

	TArrayView<FMovieSceneDoubleChannel*> DoubleChannels = Proxy.GetChannels<FMovieSceneDoubleChannel>();
	if (DoubleChannels.Num() >= 6)
	{
		TSharedPtr<FJsonObject> Obj = ClaireonSequenceEditHandlersInternal::ParseJsonObject(ValueJson);
		if (!Obj.IsValid())
		{
			OutError = TEXT("transform channel expects {\"location\":[x,y,z],\"rotation\":[p,y,r],\"scale\":[sx,sy,sz]?} "
				"or per-channel {translation_x,...,rotation_x,...,scale_x,...}");
			return false;
		}

		// Accept either compact form (location/rotation/scale arrays) or
		// per-channel form (translation_x, translation_y, ..., scale_z). Per-channel
		// form lets a caller key a single sub-channel without touching the others
		// (Sequencer engine code paths assume per-channel writes, so this matches
		// 9-channel transform-section expectations).
		double Loc[3] = { 0.0, 0.0, 0.0 };
		double Rot[3] = { 0.0, 0.0, 0.0 };
		double Scl[3] = { 1.0, 1.0, 1.0 };
		bool bHasLocAny = false, bHasRotAny = false, bHasSclAny = false;
		bool bSetLoc[3] = { false, false, false };
		bool bSetRot[3] = { false, false, false };
		bool bSetScl[3] = { false, false, false };

		// Compact array form first.
		const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* SclArr = nullptr;
		if (Obj->TryGetArrayField(TEXT("location"), LocArr) && LocArr && LocArr->Num() == 3)
		{
			for (int32 i = 0; i < 3; ++i) { Loc[i] = (*LocArr)[i]->AsNumber(); bSetLoc[i] = true; }
			bHasLocAny = true;
		}
		if (Obj->TryGetArrayField(TEXT("rotation"), RotArr) && RotArr && RotArr->Num() == 3)
		{
			for (int32 i = 0; i < 3; ++i) { Rot[i] = (*RotArr)[i]->AsNumber(); bSetRot[i] = true; }
			bHasRotAny = true;
		}
		if (Obj->TryGetArrayField(TEXT("scale"), SclArr) && SclArr && SclArr->Num() == 3)
		{
			for (int32 i = 0; i < 3; ++i) { Scl[i] = (*SclArr)[i]->AsNumber(); bSetScl[i] = true; }
			bHasSclAny = true;
		}

		// Per-channel named-axis form (overrides compact form for any explicitly-named axis).
		static const TCHAR* const Axes[3] = { TEXT("x"), TEXT("y"), TEXT("z") };
		auto TryReadAxis = [&Obj](const FString& Prefix, const TCHAR* Axis, double& OutValue) -> bool
		{
			double V = 0.0;
			if (Obj->TryGetNumberField(FString::Printf(TEXT("%s_%s"), *Prefix, Axis), V))
			{
				OutValue = V;
				return true;
			}
			return false;
		};
		for (int32 i = 0; i < 3; ++i)
		{
			if (TryReadAxis(TEXT("translation"), Axes[i], Loc[i])) { bSetLoc[i] = true; bHasLocAny = true; }
			if (TryReadAxis(TEXT("rotation"),    Axes[i], Rot[i])) { bSetRot[i] = true; bHasRotAny = true; }
			if (TryReadAxis(TEXT("scale"),       Axes[i], Scl[i])) { bSetScl[i] = true; bHasSclAny = true; }
		}

		if (!bHasLocAny && !bHasRotAny && !bHasSclAny)
		{
			OutError = TEXT("transform channel requires at least one of: location[], rotation[], scale[], or per-axis "
				"translation_x/.../scale_z");
			return false;
		}

		// Apply location (channels 0-2) and rotation (channels 3-5). Scale (channels 6-8)
		// only if the section has >=9 channels (full 3D Transform section).
		for (int32 i = 0; i < 3; ++i)
		{
			if (bSetLoc[i])
			{
				DoubleChannels[i]->GetData().UpdateOrAddKey(Frame, FMovieSceneDoubleValue(Loc[i]));
			}
			if (bSetRot[i])
			{
				DoubleChannels[3 + i]->GetData().UpdateOrAddKey(Frame, FMovieSceneDoubleValue(Rot[i]));
			}
		}
		if (DoubleChannels.Num() >= 9)
		{
			for (int32 i = 0; i < 3; ++i)
			{
				if (bSetScl[i])
				{
					DoubleChannels[6 + i]->GetData().UpdateOrAddKey(Frame, FMovieSceneDoubleValue(Scl[i]));
				}
			}
		}
		else if (bHasSclAny)
		{
			OutError = FString::Printf(
				TEXT("scale keys requested but section has only %d channels (need 9 for scale)"),
				DoubleChannels.Num());
			return false;
		}
		return true;
	}

	// 2 or 3-channel double tracks (e.g. Vector2D / Vector / Translation / Scale)
	// accept either a flat `{x, y, z}` JSON object or an array `[x, y, z]`. UMG widget
	// animation Translation/Scale tracks live here -- before this branch the only payload
	// shape was the 6-channel transform path above, so vector-typed widget tracks failed
	// with "no supported channel".
	if (DoubleChannels.Num() == 2 || DoubleChannels.Num() == 3)
	{
		double Comps[3] = { 0.0, 0.0, 0.0 };
		bool bParsed = false;

		TSharedPtr<FJsonObject> Obj = ClaireonSequenceEditHandlersInternal::ParseJsonObject(ValueJson);
		if (Obj.IsValid())
		{
			// Try array form first: { "value": [x, y, z] } or {"location": [x, y, z]}
			const TArray<TSharedPtr<FJsonValue>>* ArrPtr = nullptr;
			if ((Obj->TryGetArrayField(TEXT("value"), ArrPtr) || Obj->TryGetArrayField(TEXT("location"), ArrPtr))
				&& ArrPtr && ArrPtr->Num() >= DoubleChannels.Num())
			{
				for (int32 i = 0; i < DoubleChannels.Num(); ++i)
				{
					Comps[i] = (*ArrPtr)[i]->AsNumber();
				}
				bParsed = true;
			}
			// Try named-axis form: { "X": .., "Y": .., "Z": .. } (case-insensitive)
			if (!bParsed)
			{
				static const TCHAR* const AxisNames[3] = { TEXT("X"), TEXT("Y"), TEXT("Z") };
				int32 Hits = 0;
				for (int32 i = 0; i < DoubleChannels.Num(); ++i)
				{
					double V = 0.0;
					if (Obj->TryGetNumberField(AxisNames[i], V))
					{
						Comps[i] = V;
						++Hits;
					}
				}
				if (Hits == DoubleChannels.Num())
				{
					bParsed = true;
				}
			}
		}
		// Bare JSON array form: "[x, y, z]"
		if (!bParsed)
		{
			TSharedPtr<FJsonValue> ArrayValue;
			TSharedRef<TJsonReader<>> ArrReader = TJsonReaderFactory<>::Create(ValueJson);
			if (FJsonSerializer::Deserialize(ArrReader, ArrayValue) && ArrayValue.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* ArrPtr = nullptr;
				if (ArrayValue->TryGetArray(ArrPtr) && ArrPtr && ArrPtr->Num() >= DoubleChannels.Num())
				{
					for (int32 i = 0; i < DoubleChannels.Num(); ++i)
					{
						Comps[i] = (*ArrPtr)[i]->AsNumber();
					}
					bParsed = true;
				}
			}
		}

		if (!bParsed)
		{
			OutError = FString::Printf(
				TEXT("%d-channel vector track expects {\"value\":[x,y,z]}, {\"x\":..,\"y\":..,\"z\":..}, or a bare JSON array"),
				DoubleChannels.Num());
			return false;
		}

		for (int32 i = 0; i < DoubleChannels.Num(); ++i)
		{
			DoubleChannels[i]->GetData().UpdateOrAddKey(Frame, FMovieSceneDoubleValue(Comps[i]));
		}
		return true;
	}

	// 1-channel double track (e.g. Color subchannel / scalar property animation).
	if (DoubleChannels.Num() == 1)
	{
		double ValueD = 0.0;
		const FString Trim = ValueJson.TrimStartAndEnd();
		bool bParsed = FDefaultValueHelper::ParseDouble(Trim, ValueD);
		if (!bParsed)
		{
			if (TSharedPtr<FJsonObject> Obj = ClaireonSequenceEditHandlersInternal::ParseJsonObject(ValueJson))
			{
				bParsed = Obj->TryGetNumberField(TEXT("value"), ValueD);
			}
		}
		if (!bParsed)
		{
			OutError = TEXT("double channel expects numeric payload or {\"value\": <number>}");
			return false;
		}
		DoubleChannels[0]->GetData().UpdateOrAddKey(Frame, FMovieSceneDoubleValue(ValueD));
		return true;
	}

	OutError = FString::Printf(TEXT("section %s has no supported channel for keyframe insertion"),
		*Section->GetClass()->GetName());
	return false;
}

bool ApplyRemoveKeyframe(UMovieSceneSection* Section, FFrameNumber Frame, FString& OutError)
{
	if (!Section)
	{
		OutError = TEXT("section is null");
		return false;
	}
	FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
	bool bAnyRemoved = false;
	for (const FMovieSceneChannelEntry& Entry : Proxy.GetAllEntries())
	{
		for (FMovieSceneChannel* Channel : Entry.GetChannels())
		{
			if (!Channel)
			{
				continue;
			}
			TArray<FKeyHandle> Handles;
			TArray<FFrameNumber> Times;
			Channel->GetKeys(TRange<FFrameNumber>::All(), &Times, &Handles);
			TArray<FKeyHandle> ToDelete;
			for (int32 i = 0; i < Times.Num(); ++i)
			{
				if (Times[i] == Frame)
				{
					ToDelete.Add(Handles[i]);
				}
			}
			if (ToDelete.Num() > 0)
			{
				Channel->DeleteKeys(ToDelete);
				bAnyRemoved = true;
			}
		}
	}
	if (!bAnyRemoved)
	{
		OutError = FString::Printf(TEXT("no keys found at frame %d"), Frame.Value);
		return false;
	}
	return true;
}

// EMovieSceneKeyInterpolation (storage) -> ERichCurveInterpMode (per-key field on
// FMovieSceneFloatValue / FMovieSceneDoubleValue). Maps follow the engine
// convention used by TMovieSceneCurveChannelImpl::AddKeyToChannel at
// MovieSceneCurveChannelImpl.cpp:1043.
ERichCurveInterpMode InterpolationToRichMode(EMovieSceneKeyInterpolation In)
{
	switch (In)
	{
	case EMovieSceneKeyInterpolation::SmartAuto: return RCIM_Cubic;
	case EMovieSceneKeyInterpolation::Auto:      return RCIM_Cubic;
	case EMovieSceneKeyInterpolation::User:      return RCIM_Cubic;
	case EMovieSceneKeyInterpolation::Break:     return RCIM_Cubic;
	case EMovieSceneKeyInterpolation::Linear:    return RCIM_Linear;
	case EMovieSceneKeyInterpolation::Constant:  return RCIM_Constant;
	default:                                     return RCIM_Cubic;
	}
}

bool ApplySetKeyInterpMode(UMovieSceneSection* Section, FFrameNumber Frame,
	EMovieSceneKeyInterpolation InterpMode, FString& OutError)
{
	if (!Section)
	{
		OutError = TEXT("section is null");
		return false;
	}

	FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
	const ERichCurveInterpMode RichMode = InterpolationToRichMode(InterpMode);

	bool bMatched = false;
	TArray<FString> UnsupportedChannelErrors;

	for (const FMovieSceneChannelEntry& Entry : Proxy.GetAllEntries())
	{
		const FName TypeName = Entry.GetChannelTypeName();
		for (FMovieSceneChannel* Channel : Entry.GetChannels())
		{
			if (!Channel)
			{
				continue;
			}

			// Detect whether the channel holds a key at the frame first -- this is
			// the discriminator used for the "unsupported type" accounting below.
			TArray<FFrameNumber> Times;
			TArray<FKeyHandle> Handles;
			Channel->GetKeys(TRange<FFrameNumber>(Frame, Frame), &Times, &Handles);
			if (Times.Num() == 0)
			{
				continue;
			}

			const FName FloatTypeName   = FMovieSceneFloatChannel::StaticStruct()->GetFName();
			const FName DoubleTypeName  = FMovieSceneDoubleChannel::StaticStruct()->GetFName();
			const FName IntegerTypeName = FMovieSceneIntegerChannel::StaticStruct()->GetFName();

			if (TypeName == FloatTypeName)
			{
				FMovieSceneFloatChannel* FloatChannel = static_cast<FMovieSceneFloatChannel*>(Channel);
				TMovieSceneChannelData<FMovieSceneFloatValue> Data = FloatChannel->GetData();
				for (const FKeyHandle& Handle : Handles)
				{
					const int32 ValueIdx = Data.GetIndex(Handle);
					if (ValueIdx != INDEX_NONE)
					{
						Data.GetValues()[ValueIdx].InterpMode = RichMode;
						bMatched = true;
					}
				}
				continue;
			}

			if (TypeName == DoubleTypeName)
			{
				FMovieSceneDoubleChannel* DoubleChannel = static_cast<FMovieSceneDoubleChannel*>(Channel);
				TMovieSceneChannelData<FMovieSceneDoubleValue> Data = DoubleChannel->GetData();
				for (const FKeyHandle& Handle : Handles)
				{
					const int32 ValueIdx = Data.GetIndex(Handle);
					if (ValueIdx != INDEX_NONE)
					{
						Data.GetValues()[ValueIdx].InterpMode = RichMode;
						bMatched = true;
					}
				}
				continue;
			}

			if (TypeName == IntegerTypeName)
			{
				// Integer channels have no per-key interp mode; treat as success no-op
				// once a key is located at the frame.
				bMatched = true;
				continue;
			}

			UnsupportedChannelErrors.Add(FString::Printf(
				TEXT("channel of type '%s' does not support interpolation"), *TypeName.ToString()));
		}
	}

	if (!bMatched && UnsupportedChannelErrors.Num() == 0)
	{
		OutError = FString::Printf(TEXT("no key at frame %d on section"), Frame.Value);
		return false;
	}

	if (UnsupportedChannelErrors.Num() > 0)
	{
		OutError = FString::Printf(
			TEXT("interpolation not supported on channel type; supported types: float, double, integer (%s)"),
			*FString::Join(UnsupportedChannelErrors, TEXT("; ")));
		return false;
	}

	return true;
}

bool ApplyCreateEventEndpoint(ULevelSequence* Sequence, FName EndpointName,
	ESequenceEventEndpointSignature Signature, UBlueprint*& OutDirectorBP, UFunction*& OutFunction, FString& OutError)
{
	OutDirectorBP = nullptr;
	OutFunction = nullptr;
	if (!Sequence)
	{
		OutError = TEXT("sequence is null");
		return false;
	}
	UBlueprint* DirectorBP = FClaireonSequenceHelpers::EnsureDirectorBlueprint(Sequence, OutError);
	if (!DirectorBP)
	{
		return false;
	}
	UFunction* Func = FClaireonSequenceHelpers::CreateEventEndpointNode(DirectorBP, EndpointName, Signature, OutError);
	if (!Func)
	{
		return false;
	}

	int32 ParamCount = 0;
	bool bHasReturn = false;
	FObjectProperty* SingleObjectParam = nullptr;
	for (TFieldIterator<FProperty> It(Func); It && (It->PropertyFlags & CPF_Parm); ++It)
	{
		FProperty* Prop = *It;
		if (Prop->PropertyFlags & CPF_ReturnParm)
		{
			bHasReturn = true;
			continue;
		}
		++ParamCount;
		SingleObjectParam = CastField<FObjectProperty>(Prop);
	}
	if (bHasReturn)
	{
		OutError = FString::Printf(TEXT("endpoint '%s' has a return value; event-track endpoints must return void"),
			*EndpointName.ToString());
		return false;
	}
	const int32 Expected = (Signature == ESequenceEventEndpointSignature::BoundObject) ? 1 : 0;
	if (ParamCount != Expected)
	{
		OutError = FString::Printf(TEXT("endpoint '%s' has %d params, expected %d for signature %s"),
			*EndpointName.ToString(), ParamCount, Expected,
			Signature == ESequenceEventEndpointSignature::BoundObject ? TEXT("BoundObject") : TEXT("NoParams"));
		return false;
	}
	if (Signature == ESequenceEventEndpointSignature::BoundObject && !SingleObjectParam)
	{
		OutError = FString::Printf(TEXT("endpoint '%s' BoundObject param is not a UObject* property"),
			*EndpointName.ToString());
		return false;
	}

	OutDirectorBP = DirectorBP;
	OutFunction = Func;
	return true;
}

bool ApplyRebindActor(ULevelSequence* Sequence, const FGuid& BindingGuid,
    AActor* Actor, bool bClear, FString& OutError)
{
    if (!Sequence || !Sequence->GetMovieScene())
    {
        OutError = TEXT("sequence or MovieScene is null");
        return false;
    }
    UMovieScene* MovieScene = Sequence->GetMovieScene();

    FMovieScenePossessable* Possessable = MovieScene->FindPossessable(BindingGuid);
    if (!Possessable)
    {
        for (const FMovieSceneBinding& B : MovieScene->GetBindings())
        {
            if (B.GetObjectGuid() == BindingGuid)
            {
                OutError = FString::Printf(
                    TEXT("binding %s is a spawnable; use claireon.level_sequence_<spawnable-tool> "
                         "or set_spawnable_binding_id instead"),
                    *BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
                return false;
            }
        }
        OutError = FString::Printf(TEXT("binding %s not found"),
            *BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
        return false;
    }

    if (!Sequence->CanRebindPossessable(*Possessable))
    {
        OutError = FString::Printf(
            TEXT("binding %s has parent %s and cannot be rebound directly"),
            *BindingGuid.ToString(EGuidFormats::DigitsWithHyphens),
            *Possessable->GetParent().ToString(EGuidFormats::DigitsWithHyphens));
        return false;
    }

    if (!bClear && !Actor)
    {
        OutError = TEXT("actor is null and clear is false");
        return false;
    }

#if WITH_EDITORONLY_DATA
    if (!bClear && Actor)
    {
        if (const UClass* Required = Possessable->GetPossessedObjectClass())
        {
            if (!Actor->GetClass()->IsChildOf(Required))
            {
                OutError = FString::Printf(
                    TEXT("actor class '%s' is not a child of possessable class '%s'"),
                    *Actor->GetClass()->GetName(), *Required->GetName());
                return false;
            }
        }
    }
#endif

    UWorld* ResolutionContext = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!bClear && !ResolutionContext)
    {
        OutError = TEXT("editor world unavailable for binding context");
        return false;
    }

    // BindPossessableObject appends to BindingReferences; unbind first to avoid
    // a second rebind leaving two references for the same GUID.
    Sequence->UnbindPossessableObjects(BindingGuid);

    if (!bClear)
    {
        Sequence->BindPossessableObject(BindingGuid, *Actor, ResolutionContext);
    }

    return true;
}

}  // namespace Claireon::SequenceEdit
