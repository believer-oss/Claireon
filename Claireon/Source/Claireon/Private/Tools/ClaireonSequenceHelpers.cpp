// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSequenceHelpers.h"

#include "ClaireonPathResolver.h"
#include "LevelSequence.h"
#include "LevelSequenceDirector.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "MovieSceneMarkedFrame.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneChannel.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneVisibilityTrack.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Tracks/MovieSceneAudioTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Sections/MovieSceneEventSectionBase.h"
#include "Channels/MovieSceneEvent.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/DefaultValueHelper.h"

// F5: Director Blueprint authoring
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"

// ---------------------------------------------------------------------------
// Asset loading
// ---------------------------------------------------------------------------

ULevelSequence* FClaireonSequenceHelpers::LoadLevelSequenceAsset(const FString& Path, FString& OutError)
{
	OutError.Reset();

	if (Path.IsEmpty())
	{
		OutError = TEXT("asset_path is empty");
		return nullptr;
	}

	ClaireonPathResolver::FResolveResult ResolveResult = ClaireonPathResolver::Resolve(Path);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return nullptr;
	}

	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;
	FSoftObjectPath SoftPath(ResolvedPath);
	UObject* LoadedObj = SoftPath.TryLoad();
	if (!LoadedObj)
	{
		OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *ResolvedPath);
		return nullptr;
	}

	ULevelSequence* Sequence = Cast<ULevelSequence>(LoadedObj);
	if (!Sequence)
	{
		OutError = FString::Printf(TEXT("Asset at %s is not a Level Sequence (actual type: %s)"),
			*ResolvedPath, *LoadedObj->GetClass()->GetName());
		return nullptr;
	}

	return Sequence;
}

// ---------------------------------------------------------------------------
// Track class resolution
// ---------------------------------------------------------------------------

UClass* FClaireonSequenceHelpers::ResolveTrackClass(const FString& TypeName)
{
	if (TypeName.Equals(TEXT("transform"), ESearchCase::IgnoreCase))
	{
		return UMovieScene3DTransformTrack::StaticClass();
	}
	if (TypeName.Equals(TEXT("visibility"), ESearchCase::IgnoreCase))
	{
		return UMovieSceneVisibilityTrack::StaticClass();
	}
	if (TypeName.Equals(TEXT("event"), ESearchCase::IgnoreCase))
	{
		return UMovieSceneEventTrack::StaticClass();
	}
	if (TypeName.Equals(TEXT("audio"), ESearchCase::IgnoreCase))
	{
		return UMovieSceneAudioTrack::StaticClass();
	}
	if (TypeName.Equals(TEXT("camera_cut"), ESearchCase::IgnoreCase))
	{
		return UMovieSceneCameraCutTrack::StaticClass();
	}
	if (TypeName.Equals(TEXT("float"), ESearchCase::IgnoreCase))
	{
		return UMovieSceneFloatTrack::StaticClass();
	}
	return nullptr;
}

// ---------------------------------------------------------------------------
// Frame / seconds conversion
// ---------------------------------------------------------------------------

FFrameNumber FClaireonSequenceHelpers::SecondsToFrame(const ULevelSequence* Sequence, double Seconds)
{
	if (!Sequence)
	{
		return FFrameNumber(0);
	}
	const UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		return FFrameNumber(0);
	}
	const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	// Convert seconds to a frame in tick resolution (the storage format of keyframes).
	const FFrameNumber TickFrame = (Seconds * TickResolution).FloorToFrame();
	return TickFrame;
}

double FClaireonSequenceHelpers::FrameToSeconds(const ULevelSequence* Sequence, FFrameNumber Frame)
{
	if (!Sequence)
	{
		return 0.0;
	}
	const UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		return 0.0;
	}
	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	return TickResolution.AsSeconds(FFrameTime(Frame));
}

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

FString FClaireonSequenceHelpers::FormatBinding(const FMovieSceneBinding& Binding, const UMovieScene* MovieScene)
{
	FString Output;
	const FGuid& Guid = Binding.GetObjectGuid();
	const FString& Name = Binding.GetName();

	const TCHAR* Kind = TEXT("Binding");
	FString ClassName;
	if (MovieScene)
	{
		UMovieScene* MutableMS = const_cast<UMovieScene*>(MovieScene);
		if (FMovieScenePossessable* Poss = MutableMS->FindPossessable(Guid))
		{
			Kind = TEXT("Possessable");
			if (const UClass* Cls = Poss->GetPossessedObjectClass())
			{
				ClassName = Cls->GetName();
			}
		}
		else if (FMovieSceneSpawnable* Spawn = MutableMS->FindSpawnable(Guid))
		{
			Kind = TEXT("Spawnable");
			if (UObject* Template = Spawn->GetObjectTemplate())
			{
				ClassName = Template->GetClass()->GetName();
			}
		}
	}

	if (ClassName.IsEmpty())
	{
		Output += FString::Printf(TEXT("Binding [%s] '%s' (guid=%s)\n"),
			Kind, *Name, *Guid.ToString(EGuidFormats::DigitsWithHyphens));
	}
	else
	{
		Output += FString::Printf(TEXT("Binding [%s] '%s' class=%s (guid=%s)\n"),
			Kind, *Name, *ClassName, *Guid.ToString(EGuidFormats::DigitsWithHyphens));
	}
	return Output;
}

FString FClaireonSequenceHelpers::FormatTrack(const UMovieSceneTrack* Track, bool bIncludeSections)
{
	if (!Track)
	{
		return TEXT("  Track: (null)\n");
	}

	FString Output;
	const FString TrackClassName = Track->GetClass()->GetName();
	const FString TrackName = Track->GetTrackName().ToString();
	const FString DisplayName = Track->GetDisplayName().ToString();

	Output += FString::Printf(TEXT("  Track type=%s name='%s' display='%s'\n"),
		*TrackClassName, *TrackName, *DisplayName);

	if (bIncludeSections)
	{
		const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
		Output += FString::Printf(TEXT("    Sections: %d\n"), Sections.Num());
		for (int32 Idx = 0; Idx < Sections.Num(); ++Idx)
		{
			if (Sections[Idx])
			{
				Output += FString::Printf(TEXT("    [%d] %s"), Idx, *FormatSection(Sections[Idx]));
			}
		}
	}

	return Output;
}

FString FClaireonSequenceHelpers::FormatSection(const UMovieSceneSection* Section)
{
	if (!Section)
	{
		return TEXT("Section: (null)\n");
	}

	FString Output;
	const TRange<FFrameNumber> Range = Section->GetRange();

	FString StartStr = TEXT("-inf");
	FString EndStr = TEXT("+inf");
	if (Range.HasLowerBound())
	{
		StartStr = FString::Printf(TEXT("%d"), Range.GetLowerBoundValue().Value);
	}
	if (Range.HasUpperBound())
	{
		EndStr = FString::Printf(TEXT("%d"), Range.GetUpperBoundValue().Value);
	}

	const int32 RowIndex = Section->GetRowIndex();
	const int32 EaseIn = Section->Easing.GetEaseInDuration();
	const int32 EaseOut = Section->Easing.GetEaseOutDuration();

	Output += FString::Printf(TEXT("Section class=%s range=[%s, %s) row=%d ease_in=%d ease_out=%d\n"),
		*Section->GetClass()->GetName(), *StartStr, *EndStr, RowIndex, EaseIn, EaseOut);

	return Output;
}

FString FClaireonSequenceHelpers::FormatKeyframe(const FKeyHandle& Handle, const FMovieSceneChannelProxy& Channels)
{
	// Generic formatter: look across all channel entries, locate the channel that
	// owns this handle, and emit "type=<type> time=<frame>". Used by section-level
	// keyframe summaries (see FormatSectionKeyframes below for the bulk path).
	for (const FMovieSceneChannelEntry& Entry : Channels.GetAllEntries())
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
			for (int32 Idx = 0; Idx < Handles.Num(); ++Idx)
			{
				if (Handles[Idx] == Handle)
				{
					return FString::Printf(TEXT("key type=%s time=%d"),
						*Entry.GetChannelTypeName().ToString(), Times[Idx].Value);
				}
			}
		}
	}
	return TEXT("key (unresolved)");
}

// Emit per-section keyframe list (used by FormatSequenceStructure when
// bIncludeKeyframes is set). Walks every channel in the proxy, lists times.
static FString FormatSectionKeyframes(const UMovieSceneSection* Section, const FString& Indent)
{
	if (!Section)
	{
		return FString();
	}

	FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
	FString Output;
	int32 ChannelNumber = 0;
	for (const FMovieSceneChannelEntry& Entry : Proxy.GetAllEntries())
	{
		const FName TypeName = Entry.GetChannelTypeName();
		for (FMovieSceneChannel* Channel : Entry.GetChannels())
		{
			if (!Channel)
			{
				continue;
			}
			TArray<FFrameNumber> Times;
			Channel->GetKeys(TRange<FFrameNumber>::All(), &Times, nullptr);
			Output += FString::Printf(TEXT("%schannel[%d] type=%s keys=%d"),
				*Indent, ChannelNumber, *TypeName.ToString(), Times.Num());
			if (Times.Num() > 0)
			{
				Output += TEXT(" times=[");
				const int32 Max = FMath::Min(Times.Num(), 8);
				for (int32 Idx = 0; Idx < Max; ++Idx)
				{
					if (Idx > 0)
					{
						Output += TEXT(", ");
					}
					Output += FString::Printf(TEXT("%d"), Times[Idx].Value);
				}
				if (Times.Num() > Max)
				{
					Output += FString::Printf(TEXT(", +%d more"), Times.Num() - Max);
				}
				Output += TEXT("]");
			}
			Output += TEXT("\n");
			++ChannelNumber;
		}
	}

	// Event-track bonus: resolve endpoint function name(s) from event entries.
	if (const UMovieSceneEventSectionBase* EventSection = Cast<UMovieSceneEventSectionBase>(Section))
	{
		TArrayView<FMovieSceneEvent> Events = const_cast<UMovieSceneEventSectionBase*>(EventSection)->GetAllEntryPoints();
		if (Events.Num() > 0)
		{
			Output += FString::Printf(TEXT("%sevents: %d\n"), *Indent, Events.Num());
			for (int32 EvIdx = 0; EvIdx < Events.Num(); ++EvIdx)
			{
				const FMovieSceneEvent& Ev = Events[EvIdx];
				FString EndpointName = TEXT("(unbound)");
				if (UFunction* Func = Ev.Ptrs.Function.Get())
				{
					EndpointName = Func->GetName();
				}
				Output += FString::Printf(TEXT("%s  [%d] endpoint=%s\n"), *Indent, EvIdx, *EndpointName);
			}
		}
	}

	return Output;
}

FString FClaireonSequenceHelpers::FormatSequenceStructure(const ULevelSequence* Sequence, bool bIncludeKeyframes, bool bIncludeSections)
{
	if (!Sequence)
	{
		return TEXT("(null Level Sequence)");
	}

	const UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		return FString::Printf(TEXT("=== Level Sequence: %s ===\n(no MovieScene)\n"), *Sequence->GetName());
	}

	FString Output;
	Output += FString::Printf(TEXT("=== Level Sequence: %s ===\n"), *Sequence->GetName());
	Output += FString::Printf(TEXT("Path: %s\n"), *Sequence->GetPathName());

	const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	Output += FString::Printf(TEXT("Display Rate: %d/%d (%.4f fps)\n"),
		DisplayRate.Numerator, DisplayRate.Denominator, DisplayRate.AsDecimal());
	Output += FString::Printf(TEXT("Tick Resolution: %d/%d\n"),
		TickResolution.Numerator, TickResolution.Denominator);

	const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
	FString PBStart = TEXT("-inf");
	FString PBEnd = TEXT("+inf");
	double StartSeconds = 0.0;
	double EndSeconds = 0.0;
	if (PlaybackRange.HasLowerBound())
	{
		const FFrameNumber StartFrame = PlaybackRange.GetLowerBoundValue();
		PBStart = FString::Printf(TEXT("%d"), StartFrame.Value);
		StartSeconds = TickResolution.AsSeconds(FFrameTime(StartFrame));
	}
	if (PlaybackRange.HasUpperBound())
	{
		const FFrameNumber EndFrame = PlaybackRange.GetUpperBoundValue();
		PBEnd = FString::Printf(TEXT("%d"), EndFrame.Value);
		EndSeconds = TickResolution.AsSeconds(FFrameTime(EndFrame));
	}
	Output += FString::Printf(TEXT("Playback Range: [%s, %s) frames (%.4f - %.4f s)\n"),
		*PBStart, *PBEnd, StartSeconds, EndSeconds);

	// Marked frames
	const TArray<FMovieSceneMarkedFrame>& MarkedFrames = MovieScene->GetMarkedFrames();
	Output += FString::Printf(TEXT("Marked Frames: %d\n"), MarkedFrames.Num());
	for (int32 Idx = 0; Idx < MarkedFrames.Num(); ++Idx)
	{
		const FMovieSceneMarkedFrame& Mark = MarkedFrames[Idx];
		Output += FString::Printf(TEXT("  [%d] frame=%d label='%s'\n"),
			Idx, Mark.FrameNumber.Value, *Mark.Label);
	}

	// Bindings
	const TArray<FMovieSceneBinding>& Bindings = MovieScene->GetBindings();
	Output += FString::Printf(TEXT("Bindings: %d\n"), Bindings.Num());
	for (const FMovieSceneBinding& Binding : Bindings)
	{
		Output += FormatBinding(Binding, MovieScene);
		const TArray<UMovieSceneTrack*>& Tracks = Binding.GetTracks();
		for (const UMovieSceneTrack* Track : Tracks)
		{
			Output += FormatTrack(Track, bIncludeSections);
			if (bIncludeKeyframes && Track && bIncludeSections)
			{
				for (const UMovieSceneSection* Section : Track->GetAllSections())
				{
					if (Section)
					{
						Output += FormatSectionKeyframes(Section, TEXT("      "));
					}
				}
			}
		}
	}

	return Output;
}

// ---------------------------------------------------------------------------
// Keyframe value coercion (used by F2 sequence_edit)
// ---------------------------------------------------------------------------

static TSharedPtr<FJsonObject> ParseJsonObjectPayload(const FString& JsonPayload)
{
	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonPayload);
	if (FJsonSerializer::Deserialize(Reader, Obj))
	{
		return Obj;
	}
	return nullptr;
}

bool FClaireonSequenceHelpers::CoerceKeyframeValue(UMovieSceneTrack* Track, const FString& JsonPayload, FFrameNumber Frame, FString& OutError)
{
	OutError.Reset();
	if (!Track)
	{
		OutError = TEXT("track is null");
		return false;
	}

	const UClass* TrackClass = Track->GetClass();

	// Float track -- payload is a raw number or {"value": <float>}
	if (TrackClass->IsChildOf(UMovieSceneFloatTrack::StaticClass()))
	{
		double ValueD = 0.0;
		bool bParsed = FDefaultValueHelper::ParseDouble(JsonPayload.TrimStartAndEnd(), ValueD);
		if (!bParsed)
		{
			if (TSharedPtr<FJsonObject> Obj = ParseJsonObjectPayload(JsonPayload))
			{
				bParsed = Obj->TryGetNumberField(TEXT("value"), ValueD);
			}
		}
		if (!bParsed)
		{
			OutError = TEXT("float track requires numeric payload or {\"value\": <float>}");
			return false;
		}
		// The actual key insertion is the caller's job (F2 stage 011 wires the section
		// add path); this helper only validates the payload shape.
		return true;
	}

	// Visibility track -- Unreal models visibility via a bool channel. Payload is a
	// boolean literal or {"value": <bool>}.
	if (TrackClass->IsChildOf(UMovieSceneVisibilityTrack::StaticClass()))
	{
		FString Trim = JsonPayload.TrimStartAndEnd();
		if (Trim.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Trim.Equals(TEXT("false"), ESearchCase::IgnoreCase))
		{
			return true;
		}
		if (TSharedPtr<FJsonObject> Obj = ParseJsonObjectPayload(JsonPayload))
		{
			bool bValue = false;
			if (Obj->TryGetBoolField(TEXT("value"), bValue))
			{
				return true;
			}
		}
		OutError = TEXT("visibility track requires boolean payload or {\"value\": <bool>}");
		return false;
	}

	// Transform track -- payload must be {"location":[x,y,z], "rotation":[p,y,r], "scale":[sx,sy,sz]?}
	if (TrackClass->IsChildOf(UMovieScene3DTransformTrack::StaticClass()))
	{
		TSharedPtr<FJsonObject> Obj = ParseJsonObjectPayload(JsonPayload);
		if (!Obj.IsValid())
		{
			OutError = TEXT("transform track requires JSON object payload");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
		const bool bHasLoc = Obj->TryGetArrayField(TEXT("location"), LocArr) && LocArr && LocArr->Num() == 3;
		const bool bHasRot = Obj->TryGetArrayField(TEXT("rotation"), RotArr) && RotArr && RotArr->Num() == 3;
		if (!bHasLoc || !bHasRot)
		{
			OutError = TEXT("transform track requires {\"location\":[x,y,z],\"rotation\":[p,y,r]} (optional \"scale\":[sx,sy,sz])");
			return false;
		}
		return true;
	}

	// Event track -- payload is an endpoint name, either raw string or {"endpoint": "Name"}.
	if (TrackClass->IsChildOf(UMovieSceneEventTrack::StaticClass()))
	{
		FString Trim = JsonPayload.TrimStartAndEnd();
		if (Trim.StartsWith(TEXT("\"")) && Trim.EndsWith(TEXT("\"")) && Trim.Len() >= 2)
		{
			return true;
		}
		if (TSharedPtr<FJsonObject> Obj = ParseJsonObjectPayload(JsonPayload))
		{
			FString Endpoint;
			if (Obj->TryGetStringField(TEXT("endpoint"), Endpoint) && !Endpoint.IsEmpty())
			{
				return true;
			}
		}
		OutError = TEXT("event track requires endpoint name string or {\"endpoint\": \"<name>\"}");
		return false;
	}

	OutError = FString::Printf(TEXT("unsupported track type: %s"), *TrackClass->GetName());
	return false;
}

// ---------------------------------------------------------------------------
// F5 -- Director Blueprint event endpoint authoring (stage 013)
// ---------------------------------------------------------------------------

UBlueprint* FClaireonSequenceHelpers::EnsureDirectorBlueprint(ULevelSequence* Sequence, FString& OutError)
{
	OutError.Reset();
	if (!Sequence)
	{
		OutError = TEXT("sequence is null");
		return nullptr;
	}

	if (UBlueprint* Existing = Sequence->GetDirectorBlueprint())
	{
		return Existing;
	}

	UObject* Outer = Sequence;
	const FName DirectorBPName = MakeUniqueObjectName(Outer, UBlueprint::StaticClass(),
		FName(*FString::Printf(TEXT("%s_Director"), *Sequence->GetName())));

	UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
		ULevelSequenceDirector::StaticClass(),
		Outer,
		DirectorBPName,
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		FName(TEXT("Claireon.SequenceEdit.EnsureDirectorBlueprint")));

	if (!NewBP)
	{
		OutError = TEXT("FKismetEditorUtilities::CreateBlueprint returned null for Director");
		return nullptr;
	}

	Sequence->SetDirectorBlueprint(NewBP);
	Sequence->MarkPackageDirty();
	return NewBP;
}

// Locate the first K2 ubergraph page. Event endpoints live on the ubergraph
// (not a function graph) because UK2Node_CustomEvent is a root event node.
static UEdGraph* FindDirectorUbergraph(UBlueprint* DirectorBP)
{
	if (!DirectorBP)
	{
		return nullptr;
	}
	for (UEdGraph* Page : DirectorBP->UbergraphPages)
	{
		if (Page && Page->Schema && Page->Schema->IsChildOf(UEdGraphSchema_K2::StaticClass()))
		{
			return Page;
		}
	}
	// Fallback: if no ubergraph page exists (unlikely for a BPTYPE_Normal BP), return first page.
	return DirectorBP->UbergraphPages.Num() > 0 ? DirectorBP->UbergraphPages[0] : nullptr;
}

UFunction* FClaireonSequenceHelpers::CreateEventEndpointNode(UBlueprint* DirectorBlueprint, FName EndpointName, ESequenceEventEndpointSignature Signature, FString& OutError)
{
	OutError.Reset();
	if (!DirectorBlueprint)
	{
		OutError = TEXT("director blueprint is null");
		return nullptr;
	}
	if (EndpointName.IsNone())
	{
		OutError = TEXT("endpoint name is empty");
		return nullptr;
	}

	UEdGraph* Graph = FindDirectorUbergraph(DirectorBlueprint);
	if (!Graph)
	{
		OutError = TEXT("director blueprint has no K2 ubergraph page");
		return nullptr;
	}

	// Collision check: existing UK2Node_CustomEvent / UK2Node_FunctionEntry with same name.
	const FString EndpointStr = EndpointName.ToString();
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (const UK2Node_CustomEvent* ExistingEvt = Cast<UK2Node_CustomEvent>(Node))
		{
			if (ExistingEvt->CustomFunctionName == EndpointName)
			{
				OutError = FString::Printf(TEXT("endpoint '%s' already exists on director ubergraph"), *EndpointStr);
				return nullptr;
			}
		}
		else if (const UK2Node_FunctionEntry* ExistingEntry = Cast<UK2Node_FunctionEntry>(Node))
		{
			if (ExistingEntry->CustomGeneratedFunctionName == EndpointName)
			{
				OutError = FString::Printf(TEXT("endpoint '%s' already exists on director ubergraph"), *EndpointStr);
				return nullptr;
			}
		}
	}

	// Spawn the custom event node.
	UK2Node_CustomEvent* EventNode = NewObject<UK2Node_CustomEvent>(Graph);
	EventNode->CustomFunctionName = EndpointName;
	EventNode->bIsEditable = true;
	EventNode->CreateNewGuid();
	EventNode->PostPlacedNewNode();
	EventNode->AllocateDefaultPins();
	Graph->AddNode(EventNode, /*bFromUserPerspective*/false, /*bSelectNewNode*/false);

	// For BoundObject: add a single UObject* input pin named "BoundObject".
	if (Signature == ESequenceEventEndpointSignature::BoundObject)
	{
		FEdGraphPinType PinType;
		PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
		PinType.PinSubCategory = NAME_None;
		PinType.PinSubCategoryObject = UObject::StaticClass();
		PinType.ContainerType = EPinContainerType::None;
		PinType.bIsReference = false;

		UEdGraphPin* NewPin = EventNode->CreateUserDefinedPin(
			FName(TEXT("BoundObject")), PinType, EGPD_Output, /*bUseUniqueName*/false);
		if (!NewPin)
		{
			OutError = TEXT("CreateUserDefinedPin failed for BoundObject parameter");
			return nullptr;
		}
		EventNode->ReconstructNode();
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(DirectorBlueprint);
	FKismetEditorUtilities::CompileBlueprint(DirectorBlueprint);

	if (DirectorBlueprint->Status == BS_Error)
	{
		OutError = FString::Printf(TEXT("director blueprint compile failed after adding endpoint '%s'"), *EndpointStr);
		return nullptr;
	}

	UClass* DirectorClass = DirectorBlueprint->GeneratedClass;
	if (!DirectorClass)
	{
		OutError = TEXT("director blueprint has no GeneratedClass after compile");
		return nullptr;
	}

	UFunction* Func = DirectorClass->FindFunctionByName(EndpointName);
	if (!Func)
	{
		OutError = FString::Printf(TEXT("endpoint '%s' not found on DirectorClass after compile"), *EndpointStr);
		return nullptr;
	}
	return Func;
}
