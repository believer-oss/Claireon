// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimTools_Notify.h"
#include "Tools/ClaireonAnimHelpers.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "ClaireonLog.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ============================================================================
// anim_add_notify
// ============================================================================

FString ClaireonAnimTool_AddNotify::GetOperation() const { return TEXT("add_notify"); }

FString ClaireonAnimTool_AddNotify::GetDescription() const
{
	return TEXT("Add a notify (or notify state) to the animation in the open editing session. Requires open session_id from anim_open. Transactional. Pass notify_type='skeleton' (requires notify_name) for skeleton notifies, or a class name like 'AnimNotify_PlaySound' for class-based notifies.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_AddNotify::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddString(TEXT("notify_type"), TEXT("'skeleton' for skeleton notifies, or a class name (e.g. 'AnimNotify_PlaySound', 'ANS_ComboWindow')"), true);
	S.AddNumber(TEXT("time"), TEXT("Time in seconds where the notify starts"), true);
	S.AddString(TEXT("notify_name"), TEXT("Name for skeleton notifies (required when notify_type='skeleton')"));
	S.AddNumber(TEXT("duration"), TEXT("Duration in seconds (for state notifies)"));
	S.AddNumber(TEXT("end_time"), TEXT("End time in seconds (alternative to duration; duration = end_time - time)"));
	S.AddInteger(TEXT("track_index"), TEXT("Notify track index (default 0)"));
	S.AddObject(TEXT("properties"), TEXT("Key-value pairs of properties to set on the notify sub-object"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_AddNotify::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	FString NotifyType;
	if (!Arguments->TryGetStringField(TEXT("notify_type"), NotifyType) || NotifyType.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: notify_type"));
	}

	double Time = 0.0;
	if (!Arguments->TryGetNumberField(TEXT("time"), Time))
	{
		return MakeErrorResult(TEXT("Missing required parameter: time"));
	}

	FString NotifyName;
	Arguments->TryGetStringField(TEXT("notify_name"), NotifyName);

	double Duration = -1.0;
	Arguments->TryGetNumberField(TEXT("duration"), Duration);

	double EndTime = -1.0;
	Arguments->TryGetNumberField(TEXT("end_time"), EndTime);

	if (EndTime >= 0.0 && Duration <= 0.0)
	{
		Duration = EndTime - Time;
	}

	double TrackIndexD = 0.0;
	Arguments->TryGetNumberField(TEXT("track_index"), TrackIndexD);
	int32 TrackIndex = static_cast<int32>(TrackIndexD);

	UAnimSequenceBase* Anim = Data->Animation.Get();
	if (!Anim)
	{
		return MakeErrorResult(TEXT("Animation asset is no longer valid"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimAddNotify", "MCP: Add Notify"));
	Anim->Modify();

	FString OpError;
	int32 NewIndex = -1;

	if (NotifyType.Equals(TEXT("skeleton"), ESearchCase::IgnoreCase))
	{
		if (NotifyName.IsEmpty())
		{
			return MakeErrorResult(TEXT("notify_name is required when notify_type='skeleton'"));
		}
		NewIndex = ClaireonAnimHelpers::AddSkeletonNotify(Anim, NotifyName, static_cast<float>(Time), TrackIndex, OpError);
	}
	else
	{
		bool bIsState = NotifyType.Contains(TEXT("State")) || NotifyType.StartsWith(TEXT("ANS_"));
		UClass* NotifyClass = ClaireonAnimHelpers::ResolveNotifyClass(NotifyType, bIsState, OpError);
		if (!NotifyClass)
		{
			return MakeErrorResult(OpError);
		}
		NewIndex = ClaireonAnimHelpers::AddClassNotify(Anim, NotifyClass, static_cast<float>(Time), static_cast<float>(Duration), TrackIndex, OpError);
	}

	if (NewIndex < 0)
	{
		return MakeErrorResult(OpError);
	}

	// Apply optional properties
	const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("properties"), PropertiesObj) && PropertiesObj && (*PropertiesObj).IsValid())
	{
		for (const auto& Pair : (*PropertiesObj)->Values)
		{
			FString PropValue;
			if (Pair.Value.IsValid() && Pair.Value->TryGetString(PropValue))
			{
				FString PropError;
				ClaireonAnimHelpers::SetNotifyProperty(Anim, NewIndex, Pair.Key, PropValue, PropError);
				if (!PropError.IsEmpty())
				{
					UE_LOG(LogClaireon, Warning, TEXT("AddNotify: Failed to set property '%s': %s"), *Pair.Key, *PropError);
				}
			}
		}
	}

	Data->FocusedNotifyIndex = NewIndex;
	Data->LastOperationStatus = FString::Printf(TEXT("add_notify -> Added %s at %.3fs (index %d)"), *NotifyType, Time, NewIndex);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// anim_remove_notify
// ============================================================================

FString ClaireonAnimTool_RemoveNotify::GetOperation() const { return TEXT("remove_notify"); }

FString ClaireonAnimTool_RemoveNotify::GetDescription() const
{
	return TEXT("Remove a notify by zero-based index from the animation in the open editing session. Requires open session_id from anim_open. Transactional. Common pitfall: indices shift after removal, so cache them up front when removing multiple notifies in sequence.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_RemoveNotify::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("notify_index"), TEXT("Index of the notify to remove (0-based)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_RemoveNotify::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	double NotifyIndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("notify_index"), NotifyIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: notify_index"));
	}
	int32 NotifyIndex = static_cast<int32>(NotifyIndexD);

	UAnimSequenceBase* Anim = Data->Animation.Get();
	if (!Anim)
	{
		return MakeErrorResult(TEXT("Animation asset is no longer valid"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimRemoveNotify", "MCP: Remove Notify"));
	Anim->Modify();

	FString OpError;
	if (!ClaireonAnimHelpers::RemoveNotify(Anim, NotifyIndex, OpError))
	{
		return MakeErrorResult(OpError);
	}

	Data->FocusedNotifyIndex = -1;
	Data->LastOperationStatus = FString::Printf(TEXT("remove_notify -> Removed notify at index %d"), NotifyIndex);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// anim_move_notify
// ============================================================================

FString ClaireonAnimTool_MoveNotify::GetOperation() const { return TEXT("move_notify"); }

FString ClaireonAnimTool_MoveNotify::GetDescription() const
{
	return TEXT("Move or resize a notify in the open animation editing session. Requires open session_id from anim_open. Transactional. Omitted fields are left unchanged. Pass end_time to derive duration. Common pitfall: track_index must reference an existing track or this errors.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_MoveNotify::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("notify_index"), TEXT("Index of the notify to move (0-based)"), true);
	S.AddNumber(TEXT("time"), TEXT("New start time in seconds"));
	S.AddNumber(TEXT("duration"), TEXT("New duration in seconds"));
	S.AddNumber(TEXT("end_time"), TEXT("New end time in seconds (alternative to duration)"));
	S.AddInteger(TEXT("track_index"), TEXT("New track index"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_MoveNotify::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	double NotifyIndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("notify_index"), NotifyIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: notify_index"));
	}
	int32 NotifyIndex = static_cast<int32>(NotifyIndexD);

	double NewTime = -1.0;
	Arguments->TryGetNumberField(TEXT("time"), NewTime);

	double NewDuration = -1.0;
	Arguments->TryGetNumberField(TEXT("duration"), NewDuration);

	double EndTime = -1.0;
	Arguments->TryGetNumberField(TEXT("end_time"), EndTime);

	double NewTrackIndexD = -1.0;
	Arguments->TryGetNumberField(TEXT("track_index"), NewTrackIndexD);
	int32 NewTrackIndex = static_cast<int32>(NewTrackIndexD);

	// Compute duration from end_time if needed
	if (EndTime >= 0.0 && NewDuration < 0.0)
	{
		UAnimSequenceBase* Anim = Data->Animation.Get();
		if (!Anim)
		{
			return MakeErrorResult(TEXT("Animation asset is no longer valid"));
		}

		// Use the new time if provided, otherwise use the current start time
		float StartTime = static_cast<float>(NewTime);
		if (NewTime < 0.0)
		{
			if (NotifyIndex >= 0 && NotifyIndex < Anim->Notifies.Num())
			{
				StartTime = Anim->Notifies[NotifyIndex].GetTime();
			}
		}
		NewDuration = EndTime - static_cast<double>(StartTime);
	}

	UAnimSequenceBase* Anim = Data->Animation.Get();
	if (!Anim)
	{
		return MakeErrorResult(TEXT("Animation asset is no longer valid"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimMoveNotify", "MCP: Move Notify"));
	Anim->Modify();

	FString OpError;
	if (!ClaireonAnimHelpers::MoveNotify(Anim, NotifyIndex, static_cast<float>(NewTime), static_cast<float>(NewDuration), NewTrackIndex, OpError))
	{
		return MakeErrorResult(OpError);
	}

	Data->FocusedNotifyIndex = NotifyIndex;
	Data->LastOperationStatus = FString::Printf(TEXT("move_notify -> Moved notify %d"), NotifyIndex);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// anim_duplicate_notify
// ============================================================================

FString ClaireonAnimTool_DuplicateNotify::GetOperation() const { return TEXT("duplicate_notify"); }

FString ClaireonAnimTool_DuplicateNotify::GetDescription() const
{
	return TEXT("Duplicate a notify to a new time and/or track in the open animation editing session. Requires open session_id from anim_open. Transactional. Sub-objects are deep-copied. Missing track positions are auto-created. Returns the new notify index.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_DuplicateNotify::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("notify_index"), TEXT("Index of the notify to duplicate (0-based)"), true);
	S.AddNumber(TEXT("time"), TEXT("Time for the duplicate (default: same as source)"));
	S.AddInteger(TEXT("track_index"), TEXT("Track index for the duplicate (default: same as source)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_DuplicateNotify::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	double NotifyIndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("notify_index"), NotifyIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: notify_index"));
	}
	int32 SourceIndex = static_cast<int32>(NotifyIndexD);

	UAnimSequenceBase* Anim = Data->Animation.Get();
	if (!Anim)
	{
		return MakeErrorResult(TEXT("Animation asset is no longer valid"));
	}

	if (SourceIndex < 0 || SourceIndex >= Anim->Notifies.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Notify index %d is out of range (0-%d)"), SourceIndex, Anim->Notifies.Num() - 1));
	}

	const FAnimNotifyEvent& SourceEvent = Anim->Notifies[SourceIndex];

	// Read optional overrides
	double TargetTime = static_cast<double>(SourceEvent.GetTime());
	Arguments->TryGetNumberField(TEXT("time"), TargetTime);

	double TargetTrackD = static_cast<double>(SourceEvent.TrackIndex);
	Arguments->TryGetNumberField(TEXT("track_index"), TargetTrackD);
	int32 TargetTrack = static_cast<int32>(TargetTrackD);

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimDuplicateNotify", "MCP: Duplicate Notify"));
	Anim->Modify();

	// Ensure target track exists
	while (TargetTrack >= Anim->AnimNotifyTracks.Num())
	{
		FAnimNotifyTrack NewTrack;
		NewTrack.TrackName = FName(*FString::Printf(TEXT("Track %d"), Anim->AnimNotifyTracks.Num()));
		Anim->AnimNotifyTracks.Add(NewTrack);
	}

	// Create the duplicate event
	FAnimNotifyEvent NewEvent = SourceEvent;
	NewEvent.Guid = FGuid::NewGuid();
	NewEvent.TrackIndex = TargetTrack;
	NewEvent.SetTime(static_cast<float>(TargetTime));

	// Duplicate sub-objects
	if (SourceEvent.Notify)
	{
		NewEvent.Notify = DuplicateObject<UAnimNotify>(SourceEvent.Notify, Anim);
	}
	if (SourceEvent.NotifyStateClass)
	{
		NewEvent.NotifyStateClass = DuplicateObject<UAnimNotifyState>(SourceEvent.NotifyStateClass, Anim);
	}

	int32 NewIndex = Anim->Notifies.Add(NewEvent);

	Anim->RefreshCacheData();
	Anim->MarkPackageDirty();

	Data->FocusedNotifyIndex = NewIndex;
	Data->LastOperationStatus = FString::Printf(TEXT("duplicate_notify -> Duplicated notify %d to index %d at %.3fs on track %d"), SourceIndex, NewIndex, TargetTime, TargetTrack);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// anim_set_notify_property
// ============================================================================

FString ClaireonAnimTool_SetNotifyProperty::GetOperation() const { return TEXT("set_notify_property"); }

FString ClaireonAnimTool_SetNotifyProperty::GetDescription() const
{
	return TEXT("Set a property on the sub-object of a notify in the open animation editing session. Requires open session_id from anim_open. Transactional. Common pitfall: property_name must match the UPROPERTY name on the notify's UAnimNotify or UAnimNotifyState class.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_SetNotifyProperty::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("notify_index"), TEXT("Index of the notify (0-based)"), true);
	S.AddString(TEXT("property_name"), TEXT("Name of the property to set"), true);
	S.AddString(TEXT("value"), TEXT("Value to set (string representation)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_SetNotifyProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	double NotifyIndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("notify_index"), NotifyIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: notify_index"));
	}
	int32 NotifyIndex = static_cast<int32>(NotifyIndexD);

	FString PropertyName;
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_name"));
	}

	FString Value;
	if (!Arguments->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	UAnimSequenceBase* Anim = Data->Animation.Get();
	if (!Anim)
	{
		return MakeErrorResult(TEXT("Animation asset is no longer valid"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimSetNotifyProperty", "MCP: Set Notify Property"));
	Anim->Modify();

	FString OpError;
	if (!ClaireonAnimHelpers::SetNotifyProperty(Anim, NotifyIndex, PropertyName, Value, OpError))
	{
		return MakeErrorResult(OpError);
	}

	Data->FocusedNotifyIndex = NotifyIndex;
	Data->LastOperationStatus = FString::Printf(TEXT("set_notify_property -> Set %s = %s on notify %d"), *PropertyName, *Value, NotifyIndex);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// anim_get_notify_property
// ============================================================================

FString ClaireonAnimTool_GetNotifyProperty::GetOperation() const { return TEXT("get_notify_property"); }

FString ClaireonAnimTool_GetNotifyProperty::GetDescription() const
{
	return TEXT("Get a property value from the sub-object of a notify in the open animation editing session. Requires open session_id from anim_open. Read-only. Common pitfall: property_name must match the UPROPERTY name on the underlying UAnimNotify(State) class.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_GetNotifyProperty::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("notify_index"), TEXT("Index of the notify (0-based)"), true);
	S.AddString(TEXT("property_name"), TEXT("Name of the property to read"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_GetNotifyProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	double NotifyIndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("notify_index"), NotifyIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: notify_index"));
	}
	int32 NotifyIndex = static_cast<int32>(NotifyIndexD);

	FString PropertyName;
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_name"));
	}

	UAnimSequenceBase* Anim = Data->Animation.Get();
	if (!Anim)
	{
		return MakeErrorResult(TEXT("Animation asset is no longer valid"));
	}

	FString OpError;
	FString Value = ClaireonAnimHelpers::GetNotifyProperty(Anim, NotifyIndex, PropertyName, OpError);
	if (!OpError.IsEmpty())
	{
		return MakeErrorResult(OpError);
	}

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetNumberField(TEXT("notify_index"), NotifyIndex);
	ResponseData->SetStringField(TEXT("property_name"), PropertyName);
	ResponseData->SetStringField(TEXT("value"), Value);

	return MakeSuccessResult(ResponseData, FString::Printf(TEXT("notify[%d].%s = %s"), NotifyIndex, *PropertyName, *Value));
}

// ============================================================================
// anim_list_notify_properties
// ============================================================================

FString ClaireonAnimTool_ListNotifyProperties::GetOperation() const { return TEXT("list_notify_properties"); }

FString ClaireonAnimTool_ListNotifyProperties::GetDescription() const
{
	return TEXT("List all editable properties on the sub-object of a notify in the open animation editing session. Requires open session_id from anim_open. Read-only. Returns property name, type, and current value tuples for use with anim_set_notify_property.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_ListNotifyProperties::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("notify_index"), TEXT("Index of the notify (0-based)"), true);
	S.AddString(TEXT("filter"), TEXT("Optional substring filter on property names"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_ListNotifyProperties::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	double NotifyIndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("notify_index"), NotifyIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: notify_index"));
	}
	int32 NotifyIndex = static_cast<int32>(NotifyIndexD);

	FString Filter;
	Arguments->TryGetStringField(TEXT("filter"), Filter);

	UAnimSequenceBase* Anim = Data->Animation.Get();
	if (!Anim)
	{
		return MakeErrorResult(TEXT("Animation asset is no longer valid"));
	}

	if (NotifyIndex < 0 || NotifyIndex >= Anim->Notifies.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Notify index %d is out of range (0-%d)"), NotifyIndex, Anim->Notifies.Num() - 1));
	}

	const FAnimNotifyEvent& Event = Anim->Notifies[NotifyIndex];
	UObject* SubObject = Event.Notify ? static_cast<UObject*>(Event.Notify) : static_cast<UObject*>(Event.NotifyStateClass);

	if (!SubObject)
	{
		return MakeErrorResult(FString::Printf(TEXT("Notify at index %d is a skeleton notify with no sub-object (no properties to list)"), NotifyIndex));
	}

	TSharedPtr<FJsonObject> Properties = ClaireonPropertyUtils::GetAllProperties(SubObject, Filter);

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetNumberField(TEXT("notify_index"), NotifyIndex);
	ResponseData->SetStringField(TEXT("class"), SubObject->GetClass()->GetName());
	ResponseData->SetObjectField(TEXT("properties"), Properties);

	return MakeSuccessResult(ResponseData, FString::Printf(TEXT("notify[%d] (%s): %d properties"), NotifyIndex, *SubObject->GetClass()->GetName(), Properties->Values.Num()));
}

// ============================================================================
// anim_add_notify_track
// ============================================================================

FString ClaireonAnimTool_AddNotifyTrack::GetOperation() const { return TEXT("add_notify_track"); }

FString ClaireonAnimTool_AddNotifyTrack::GetDescription() const
{
	return TEXT("Add a new notify track to the animation in the open editing session. Requires open session_id from anim_open. Transactional. The new track is appended at the highest index. Use the returned track index when placing notifies on it.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_AddNotifyTrack::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddString(TEXT("track_name"), TEXT("Name for the new track (auto-named if omitted)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_AddNotifyTrack::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	FString TrackName;
	if (!Arguments->TryGetStringField(TEXT("track_name"), TrackName) || TrackName.IsEmpty())
	{
		UAnimSequenceBase* Anim = Data->Animation.Get();
		if (Anim)
		{
			TrackName = FString::Printf(TEXT("Track %d"), Anim->AnimNotifyTracks.Num());
		}
		else
		{
			return MakeErrorResult(TEXT("Animation asset is no longer valid"));
		}
	}

	UAnimSequenceBase* Anim = Data->Animation.Get();
	if (!Anim)
	{
		return MakeErrorResult(TEXT("Animation asset is no longer valid"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimAddNotifyTrack", "MCP: Add Notify Track"));
	Anim->Modify();

	FAnimNotifyTrack NewTrack;
	NewTrack.TrackName = FName(*TrackName);
	int32 NewIndex = Anim->AnimNotifyTracks.Add(NewTrack);

	Anim->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("add_notify_track -> Added track '%s' at index %d"), *TrackName, NewIndex);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// anim_remove_notify_track
// ============================================================================

FString ClaireonAnimTool_RemoveNotifyTrack::GetOperation() const { return TEXT("remove_notify_track"); }

FString ClaireonAnimTool_RemoveNotifyTrack::GetDescription() const
{
	return TEXT("Remove a notify track from the animation in the open editing session. Requires open session_id from anim_open. Transactional. Notifies on the removed track are reassigned to track 0; remaining track indices shift down by one above the removed slot.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_RemoveNotifyTrack::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("track_index"), TEXT("Index of the track to remove (0-based)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_RemoveNotifyTrack::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	double TrackIndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("track_index"), TrackIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: track_index"));
	}
	int32 TrackIndex = static_cast<int32>(TrackIndexD);

	UAnimSequenceBase* Anim = Data->Animation.Get();
	if (!Anim)
	{
		return MakeErrorResult(TEXT("Animation asset is no longer valid"));
	}

	if (Anim->AnimNotifyTracks.Num() <= 1)
	{
		return MakeErrorResult(TEXT("Cannot remove the last notify track"));
	}

	if (TrackIndex < 0 || TrackIndex >= Anim->AnimNotifyTracks.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Track index %d is out of range (0-%d)"), TrackIndex, Anim->AnimNotifyTracks.Num() - 1));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimRemoveNotifyTrack", "MCP: Remove Notify Track"));
	Anim->Modify();

	FString RemovedName = Anim->AnimNotifyTracks[TrackIndex].TrackName.ToString();

	// Reassign notifies from the removed track
	for (FAnimNotifyEvent& Event : Anim->Notifies)
	{
		if (Event.TrackIndex == TrackIndex)
		{
			Event.TrackIndex = 0;
		}
		else if (Event.TrackIndex > TrackIndex)
		{
			Event.TrackIndex--;
		}
	}

	Anim->AnimNotifyTracks.RemoveAt(TrackIndex);
	Anim->RefreshCacheData();
	Anim->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("remove_notify_track -> Removed track '%s' (index %d), notifies reassigned"), *RemovedName, TrackIndex);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// anim_rename_notify_track
// ============================================================================

FString ClaireonAnimTool_RenameNotifyTrack::GetOperation() const { return TEXT("rename_notify_track"); }

FString ClaireonAnimTool_RenameNotifyTrack::GetDescription() const
{
	return TEXT("Rename a notify track in the animation in the open editing session. Requires open session_id from anim_open. Transactional. The new name must be unique within the animation; collisions error out and the rename is rolled back via the transaction.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_RenameNotifyTrack::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("track_index"), TEXT("Index of the track to rename (0-based)"), true);
	S.AddString(TEXT("track_name"), TEXT("New name for the track"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_RenameNotifyTrack::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	double TrackIndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("track_index"), TrackIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: track_index"));
	}
	int32 TrackIndex = static_cast<int32>(TrackIndexD);

	FString TrackName;
	if (!Arguments->TryGetStringField(TEXT("track_name"), TrackName) || TrackName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: track_name"));
	}

	UAnimSequenceBase* Anim = Data->Animation.Get();
	if (!Anim)
	{
		return MakeErrorResult(TEXT("Animation asset is no longer valid"));
	}

	if (TrackIndex < 0 || TrackIndex >= Anim->AnimNotifyTracks.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Track index %d is out of range (0-%d)"), TrackIndex, Anim->AnimNotifyTracks.Num() - 1));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimRenameNotifyTrack", "MCP: Rename Notify Track"));
	Anim->Modify();

	FString OldName = Anim->AnimNotifyTracks[TrackIndex].TrackName.ToString();
	Anim->AnimNotifyTracks[TrackIndex].TrackName = FName(*TrackName);
	Anim->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("rename_notify_track -> Renamed track %d from '%s' to '%s'"), TrackIndex, *OldName, *TrackName);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// anim_reorder_notify_track
// ============================================================================

FString ClaireonAnimTool_ReorderNotifyTrack::GetOperation() const { return TEXT("reorder_notify_track"); }

FString ClaireonAnimTool_ReorderNotifyTrack::GetDescription() const
{
	return TEXT("Reorder a notify track to a new position in the open animation editing session. Requires open session_id from anim_open. Transactional. Common pitfall: notifies retain their stored track index, which is updated to follow the reorder so existing notifies stay on the moved track.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_ReorderNotifyTrack::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("track_index"), TEXT("Current index of the track to move (0-based)"), true);
	S.AddInteger(TEXT("new_index"), TEXT("Target index for the track (0-based)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_ReorderNotifyTrack::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	double TrackIndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("track_index"), TrackIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: track_index"));
	}
	int32 TrackIndex = static_cast<int32>(TrackIndexD);

	double NewIndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("new_index"), NewIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: new_index"));
	}
	int32 NewIndex = static_cast<int32>(NewIndexD);

	UAnimSequenceBase* Anim = Data->Animation.Get();
	if (!Anim)
	{
		return MakeErrorResult(TEXT("Animation asset is no longer valid"));
	}

	const int32 NumTracks = Anim->AnimNotifyTracks.Num();
	if (TrackIndex < 0 || TrackIndex >= NumTracks)
	{
		return MakeErrorResult(FString::Printf(TEXT("Track index %d is out of range (0-%d)"), TrackIndex, NumTracks - 1));
	}
	if (NewIndex < 0 || NewIndex >= NumTracks)
	{
		return MakeErrorResult(FString::Printf(TEXT("New index %d is out of range (0-%d)"), NewIndex, NumTracks - 1));
	}
	if (TrackIndex == NewIndex)
	{
		Data->LastOperationStatus = FString::Printf(TEXT("reorder_notify_track -> Track %d already at position %d"), TrackIndex, NewIndex);
		return BuildStateResponse(SessionId, Data);
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimReorderNotifyTrack", "MCP: Reorder Notify Track"));
	Anim->Modify();

	// Build a mapping from old track index to new track index
	TArray<int32> IndexMap;
	IndexMap.SetNum(NumTracks);
	for (int32 i = 0; i < NumTracks; ++i)
	{
		IndexMap[i] = i;
	}

	// Simulate the move: remove from TrackIndex, insert at NewIndex
	FAnimNotifyTrack MovedTrack = Anim->AnimNotifyTracks[TrackIndex];
	Anim->AnimNotifyTracks.RemoveAt(TrackIndex);
	Anim->AnimNotifyTracks.Insert(MovedTrack, NewIndex);

	// Compute the remap: for each old index, figure out where it ended up
	// A track originally at position i:
	// - If i == TrackIndex, it moved to NewIndex
	// - Otherwise, adjust based on the removal and insertion
	for (int32 OldIdx = 0; OldIdx < NumTracks; ++OldIdx)
	{
		if (OldIdx == TrackIndex)
		{
			IndexMap[OldIdx] = NewIndex;
		}
		else
		{
			int32 Adjusted = OldIdx;
			// Account for removal: indices above the removed slot shift down
			if (OldIdx > TrackIndex)
			{
				Adjusted--;
			}
			// Account for insertion: indices at or above the insertion slot shift up
			if (Adjusted >= NewIndex)
			{
				Adjusted++;
			}
			IndexMap[OldIdx] = Adjusted;
		}
	}

	// Remap all notify track indices
	for (FAnimNotifyEvent& Event : Anim->Notifies)
	{
		if (Event.TrackIndex >= 0 && Event.TrackIndex < NumTracks)
		{
			Event.TrackIndex = IndexMap[Event.TrackIndex];
		}
	}

	Anim->RefreshCacheData();
	Anim->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("reorder_notify_track -> Moved track %d to position %d"), TrackIndex, NewIndex);
	return BuildStateResponse(SessionId, Data);
}
