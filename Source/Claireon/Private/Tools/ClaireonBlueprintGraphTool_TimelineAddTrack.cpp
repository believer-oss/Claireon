// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintGraphTool_TimelineAddTrack.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase_Internal.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonLog.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "K2Node_Timeline.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlueprintGraphTool_TimelineAddTrack::GetOperation() const { return TEXT("timeline_add_track"); }

TArray<FString> ClaireonBlueprintGraphTool_TimelineAddTrack::GetSearchKeywords() const
{
    return {TEXT("bp"), TEXT("timeline"), TEXT("track"), TEXT("curve"), TEXT("keys"), TEXT("add"), TEXT("float"), TEXT("vector"), TEXT("event")};
}

FString ClaireonBlueprintGraphTool_TimelineAddTrack::GetDescription() const
{
    return TEXT("Add a float/vector/event track (with curve keys) to an EXISTING timeline on the session Blueprint, post-creation. The bound K2Node_Timeline is reconstructed so the new track's output pin appears immediately. Complements bp_add_node's create-time float_tracks/vector_tracks/event_tracks arrays. Accepts session_id or asset_path; auto-opens when asset_path is supplied.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_TimelineAddTrack::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("timeline_name"), TEXT("Name of the existing timeline (as passed to add_node Timeline)."), true);
    Builder.AddString(TEXT("track_type"), TEXT("'float' | 'vector' | 'event' (default 'float')."));
    Builder.AddString(TEXT("track_name"), TEXT("Name of the new track. Errors if a track of this name already exists on the timeline."), true);
    Builder.AddString(TEXT("interpolation"), TEXT("'linear' | 'cubic' | 'constant' key interpolation (float/vector tracks; default 'linear')."));
    Builder.AddArray(TEXT("keys"), TEXT("Curve keys: float [{time,value}], vector [{time,x,y,z}], event [{time}]."));
    Builder.AddNumber(TEXT("length"), TEXT("Optional new timeline length in seconds; when omitted the length is extended to the latest key time if needed."));
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_TimelineAddTrack::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("timeline_add_track"), Params, SessionId, Data, Error))
    {
        return Error;
    }

	UBlueprint* Blueprint = Data->Blueprint.Get();
	if (!IsValid(Blueprint))
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	FString TimelineName;
	if (!Params->TryGetStringField(TEXT("timeline_name"), TimelineName))
	{
		return MakeErrorResult(TEXT("Missing required field: timeline_name"));
	}
	FString TrackName;
	if (!Params->TryGetStringField(TEXT("track_name"), TrackName))
	{
		return MakeErrorResult(TEXT("Missing required field: track_name"));
	}

	UTimelineTemplate* Template = Blueprint->FindTimelineTemplateByVariableName(FName(*TimelineName));
	if (!IsValid(Template))
	{
		TArray<FString> Available;
		for (const UTimelineTemplate* T : Blueprint->Timelines)
		{
			if (IsValid(T)) { Available.Add(T->GetVariableName().ToString()); }
		}
		return MakeErrorResult(FString::Printf(
			TEXT("Timeline '%s' not found on this Blueprint. Available: %s"),
			*TimelineName, Available.Num() ? *FString::Join(Available, TEXT(", ")) : TEXT("(none)")));
	}

	// Duplicate-name check across all track kinds.
	const FName NewTrackFName(*TrackName);
	auto TrackNameTaken = [&]() -> bool
	{
		for (const FTTFloatTrack& T : Template->FloatTracks) { if (T.GetTrackName() == NewTrackFName) return true; }
		for (const FTTVectorTrack& T : Template->VectorTracks) { if (T.GetTrackName() == NewTrackFName) return true; }
		for (const FTTEventTrack& T : Template->EventTracks) { if (T.GetTrackName() == NewTrackFName) return true; }
		for (const FTTLinearColorTrack& T : Template->LinearColorTracks) { if (T.GetTrackName() == NewTrackFName) return true; }
		return false;
	};
	if (TrackNameTaken())
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Track '%s' already exists on timeline '%s'"), *TrackName, *TimelineName));
	}

	FString TrackType = TEXT("float");
	Params->TryGetStringField(TEXT("track_type"), TrackType);
	TrackType = TrackType.ToLower();
	if (TrackType != TEXT("float") && TrackType != TEXT("vector") && TrackType != TEXT("event"))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Unsupported track_type '%s' (expected 'float', 'vector', or 'event')"), *TrackType));
	}

	FString Interp = TEXT("linear");
	Params->TryGetStringField(TEXT("interpolation"), Interp);
	ERichCurveInterpMode InterpMode = ERichCurveInterpMode::RCIM_Linear;
	if (Interp == TEXT("constant")) { InterpMode = ERichCurveInterpMode::RCIM_Constant; }
	else if (Interp == TEXT("cubic")) { InterpMode = ERichCurveInterpMode::RCIM_Cubic; }

	const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
	Params->TryGetArrayField(TEXT("keys"), KeysArray);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Timeline Add Track")));
	Blueprint->Modify();
	Template->Modify();

	double MaxKeyTime = 0.0;

	if (TrackType == TEXT("float"))
	{
		FTTFloatTrack FloatTrack;
		FloatTrack.SetTrackName(NewTrackFName, Template);
		const FName CurveName = *FString::Printf(TEXT("%s_%s_Curve"), *TimelineName, *TrackName);
		UCurveFloat* CurveFloat = NewObject<UCurveFloat>(Blueprint->GeneratedClass, CurveName);
		FloatTrack.CurveFloat = CurveFloat;
		if (KeysArray)
		{
			for (const auto& KeyVal : *KeysArray)
			{
				const TSharedPtr<FJsonObject>& KeyObj = KeyVal->AsObject();
				if (!KeyObj) { continue; }
				double Time = 0.0, Value = 0.0;
				KeyObj->TryGetNumberField(TEXT("time"), Time);
				KeyObj->TryGetNumberField(TEXT("value"), Value);
				const FKeyHandle Handle = CurveFloat->FloatCurve.AddKey(static_cast<float>(Time), static_cast<float>(Value));
				CurveFloat->FloatCurve.SetKeyInterpMode(Handle, InterpMode);
				MaxKeyTime = FMath::Max(MaxKeyTime, Time);
			}
		}
		Template->FloatTracks.Add(FloatTrack);
		Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_FloatInterp, Template->FloatTracks.Num() - 1));
	}
	else if (TrackType == TEXT("vector"))
	{
		FTTVectorTrack VectorTrack;
		VectorTrack.SetTrackName(NewTrackFName, Template);
		const FName CurveName = *FString::Printf(TEXT("%s_%s_Curve"), *TimelineName, *TrackName);
		UCurveVector* CurveVector = NewObject<UCurveVector>(Blueprint->GeneratedClass, CurveName);
		VectorTrack.CurveVector = CurveVector;
		if (KeysArray)
		{
			for (const auto& KeyVal : *KeysArray)
			{
				const TSharedPtr<FJsonObject>& KeyObj = KeyVal->AsObject();
				if (!KeyObj) { continue; }
				double Time = 0.0, X = 0.0, Y = 0.0, Z = 0.0;
				KeyObj->TryGetNumberField(TEXT("time"), Time);
				KeyObj->TryGetNumberField(TEXT("x"), X);
				KeyObj->TryGetNumberField(TEXT("y"), Y);
				KeyObj->TryGetNumberField(TEXT("z"), Z);
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					const double Val = (Axis == 0) ? X : (Axis == 1) ? Y : Z;
					const FKeyHandle Handle = CurveVector->FloatCurves[Axis].AddKey(static_cast<float>(Time), static_cast<float>(Val));
					CurveVector->FloatCurves[Axis].SetKeyInterpMode(Handle, InterpMode);
				}
				MaxKeyTime = FMath::Max(MaxKeyTime, Time);
			}
		}
		Template->VectorTracks.Add(VectorTrack);
		Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_VectorInterp, Template->VectorTracks.Num() - 1));
	}
	else // event
	{
		FTTEventTrack EventTrack;
		EventTrack.SetTrackName(NewTrackFName, Template);
		const FName CurveName = *FString::Printf(TEXT("%s_%s_EventCurve"), *TimelineName, *TrackName);
		UCurveFloat* EventCurve = NewObject<UCurveFloat>(Blueprint->GeneratedClass, CurveName);
		EventTrack.CurveKeys = EventCurve;
		if (KeysArray)
		{
			for (const auto& KeyVal : *KeysArray)
			{
				const TSharedPtr<FJsonObject>& KeyObj = KeyVal->AsObject();
				if (!KeyObj) { continue; }
				double Time = 0.0;
				KeyObj->TryGetNumberField(TEXT("time"), Time);
				EventCurve->FloatCurve.AddKey(static_cast<float>(Time), 1.0f);
				MaxKeyTime = FMath::Max(MaxKeyTime, Time);
			}
		}
		Template->EventTracks.Add(EventTrack);
		Template->AddDisplayTrack(FTTTrackId(FTTTrackBase::TT_Event, Template->EventTracks.Num() - 1));
	}

	double ExplicitLength = 0.0;
	if (Params->TryGetNumberField(TEXT("length"), ExplicitLength))
	{
		Template->TimelineLength = static_cast<float>(ExplicitLength);
	}
	else if (MaxKeyTime > Template->TimelineLength)
	{
		Template->TimelineLength = static_cast<float>(MaxKeyTime);
	}

	// Reconstruct every K2Node_Timeline bound to this template so the new
	// track's output pin exists for subsequent connect_pins calls.
	int32 ReconstructedNodes = 0;
	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);
	for (UEdGraph* Graph : AllGraphs)
	{
		if (!IsValid(Graph)) { continue; }
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node);
			if (IsValid(TimelineNode) && TimelineNode->TimelineName == FName(*TimelineName))
			{
				TimelineNode->Modify();
				TimelineNode->ReconstructNode();
				Data->LastOperationAffectedNodes.Add(TimelineNode->NodeGuid);
				++ReconstructedNodes;
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Added %s track '%s' to timeline '%s' (%d key(s); %d timeline node(s) reconstructed)"),
		*TrackType, *TrackName, *TimelineName, KeysArray ? KeysArray->Num() : 0, ReconstructedNodes);

	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
