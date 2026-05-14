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
#include "MovieScene.h"
#include "MovieSceneBinding.h"
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
// Apply* handlers -- free functions used by decomposed level_sequence_*
// tools and by FClaireonSpecApplicator_LevelSequence. Definitions extracted from
// the deleted ClaireonTool_SequenceEdit.cpp so the apply-handler surface (see
// FEATURE_6_CLAIREON_SPEC_APPLICATOR.md "Handler extraction") continues to link
// after decomposition.
// ============================================================================

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

static TSharedPtr<FJsonObject> ParseJsonObject(const FString& JsonPayload)
{
	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonPayload);
	if (FJsonSerializer::Deserialize(Reader, Obj))
	{
		return Obj;
	}
	return nullptr;
}

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
			if (TSharedPtr<FJsonObject> Obj = ParseJsonObject(ValueJson))
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
		else if (TSharedPtr<FJsonObject> Obj = ParseJsonObject(ValueJson))
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
		TSharedPtr<FJsonObject> Obj = ParseJsonObject(ValueJson);
		if (!Obj.IsValid())
		{
			OutError = TEXT("transform channel expects {\"location\":[x,y,z],\"rotation\":[p,y,r]}");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
		const bool bHasLoc = Obj->TryGetArrayField(TEXT("location"), LocArr) && LocArr && LocArr->Num() == 3;
		const bool bHasRot = Obj->TryGetArrayField(TEXT("rotation"), RotArr) && RotArr && RotArr->Num() == 3;
		if (!bHasLoc || !bHasRot)
		{
			OutError = TEXT("transform channel requires 3-element location + rotation arrays");
			return false;
		}
		const double Loc[3] = {
			(*LocArr)[0]->AsNumber(), (*LocArr)[1]->AsNumber(), (*LocArr)[2]->AsNumber() };
		const double Rot[3] = {
			(*RotArr)[0]->AsNumber(), (*RotArr)[1]->AsNumber(), (*RotArr)[2]->AsNumber() };
		for (int32 i = 0; i < 3; ++i)
		{
			DoubleChannels[i]->GetData().UpdateOrAddKey(Frame, FMovieSceneDoubleValue(Loc[i]));
		}
		for (int32 i = 0; i < 3; ++i)
		{
			DoubleChannels[3 + i]->GetData().UpdateOrAddKey(Frame, FMovieSceneDoubleValue(Rot[i]));
		}
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

namespace Claireon::SequenceEdit
{
	// EMovieSceneKeyInterpolation (storage) -> ERichCurveInterpMode (per-key field on
	// FMovieSceneFloatValue / FMovieSceneDoubleValue). Maps follow the engine
	// convention used by TMovieSceneCurveChannelImpl::AddKeyToChannel at
	// MovieSceneCurveChannelImpl.cpp:1043.
	static ERichCurveInterpMode InterpolationToRichMode(EMovieSceneKeyInterpolation In)
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
	const ERichCurveInterpMode RichMode = Claireon::SequenceEdit::InterpolationToRichMode(InterpMode);

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
