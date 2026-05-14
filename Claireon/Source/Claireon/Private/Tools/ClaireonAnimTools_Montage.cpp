// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimTools_Montage.h"
#include "Tools/ClaireonAnimHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ============================================================================
// anim_add_section
// ============================================================================

FString ClaireonAnimTool_AddSection::GetOperation() const { return TEXT("add_section"); }

FString ClaireonAnimTool_AddSection::GetDescription() const
{
	return TEXT("Add a section to the montage at a specific time in the open editing session. Requires open session_id from anim_open. Transactional. The section_name must be unique within the montage; section ordering is determined by start_time. Errors if start_time falls outside the asset duration.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_AddSection::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddString(TEXT("section_name"), TEXT("Name of the section to add"), true);
	S.AddNumber(TEXT("start_time"), TEXT("Time in seconds where the section starts"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_AddSection::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	UAnimMontage* Montage = RequireMontage(Data, Error);
	if (!Montage) return Error;

	FString SectionName;
	if (!Arguments->TryGetStringField(TEXT("section_name"), SectionName) || SectionName.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: section_name"));

	double StartTime = 0.0;
	if (!Arguments->TryGetNumberField(TEXT("start_time"), StartTime))
		return MakeErrorResult(TEXT("Missing required parameter: start_time"));

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimAddSection", "MCP: Add Montage Section"));
	Montage->Modify();

	FString OpError;
	if (!ClaireonAnimHelpers::AddMontageSection(Montage, SectionName, static_cast<float>(StartTime), OpError))
		return MakeErrorResult(OpError);

	Montage->PostEditChange();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("add_section -> Added section '%s' at %.3fs"), *SectionName, StartTime);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// anim_remove_section
// ============================================================================

FString ClaireonAnimTool_RemoveSection::GetOperation() const { return TEXT("remove_section"); }

FString ClaireonAnimTool_RemoveSection::GetDescription() const
{
	return TEXT("Remove a section from the montage in the open animation editing session. Requires open session_id from anim_open. Transactional. Removing the only section is allowed but leaves the montage unplayable until a replacement is added. Other sections that linked to this one have their next-section link cleared.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_RemoveSection::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddString(TEXT("section_name"), TEXT("Name of the section to remove"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_RemoveSection::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	UAnimMontage* Montage = RequireMontage(Data, Error);
	if (!Montage) return Error;

	FString SectionName;
	if (!Arguments->TryGetStringField(TEXT("section_name"), SectionName) || SectionName.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: section_name"));

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimRemoveSection", "MCP: Remove Montage Section"));
	Montage->Modify();

	FString OpError;
	if (!ClaireonAnimHelpers::RemoveMontageSection(Montage, SectionName, OpError))
		return MakeErrorResult(OpError);

	Montage->PostEditChange();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_section -> Removed section '%s'"), *SectionName);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// anim_set_section_link
// ============================================================================

FString ClaireonAnimTool_SetSectionLink::GetOperation() const { return TEXT("set_section_link"); }

FString ClaireonAnimTool_SetSectionLink::GetDescription() const
{
	return TEXT("Set which section plays next after a given section in the open montage editing session. Requires open session_id from anim_open. Transactional. Pass next_section_name='' to clear the link (the section ends at its boundary). The next section name must reference an existing section.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_SetSectionLink::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddString(TEXT("section_name"), TEXT("Name of the section to modify"), true);
	S.AddString(TEXT("next_section_name"), TEXT("Name of the section to play next (empty string to clear the link)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_SetSectionLink::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	UAnimMontage* Montage = RequireMontage(Data, Error);
	if (!Montage) return Error;

	FString SectionName;
	if (!Arguments->TryGetStringField(TEXT("section_name"), SectionName) || SectionName.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: section_name"));

	FString NextSectionName;
	if (!Arguments->TryGetStringField(TEXT("next_section_name"), NextSectionName))
		return MakeErrorResult(TEXT("Missing required parameter: next_section_name"));

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimSetSectionLink", "MCP: Set Section Link"));
	Montage->Modify();

	FString OpError;
	if (!ClaireonAnimHelpers::SetMontageSectionLink(Montage, SectionName, NextSectionName, OpError))
		return MakeErrorResult(OpError);

	Montage->PostEditChange();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("set_section_link -> '%s' -> '%s'"), *SectionName, *NextSectionName);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// anim_set_section_link_method
// ============================================================================

FString ClaireonAnimTool_SetSectionLinkMethod::GetOperation() const { return TEXT("set_section_link_method"); }

FString ClaireonAnimTool_SetSectionLinkMethod::GetDescription() const
{
	return TEXT("Set the link method for a montage section in the open animation editing session. Requires open session_id from anim_open. Transactional. Link method controls how playback transitions to the next section (e.g. absolute, relative, proportional). Common pitfall: section_name must match an existing section exactly.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_SetSectionLinkMethod::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddString(TEXT("section_name"), TEXT("Name of the section to modify"), true);
	S.AddEnum(TEXT("link_method"), TEXT("Link method for the section"),
		{TEXT("absolute"), TEXT("relative"), TEXT("proportional")}, true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_SetSectionLinkMethod::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	UAnimMontage* Montage = RequireMontage(Data, Error);
	if (!Montage) return Error;

	FString SectionName;
	if (!Arguments->TryGetStringField(TEXT("section_name"), SectionName) || SectionName.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: section_name"));

	FString LinkMethodStr;
	if (!Arguments->TryGetStringField(TEXT("link_method"), LinkMethodStr) || LinkMethodStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: link_method"));

	// Find section index
	int32 SectionIndex = Montage->GetSectionIndex(FName(*SectionName));
	if (SectionIndex == INDEX_NONE)
	{
		return MakeErrorResult(FString::Printf(TEXT("Section '%s' not found in montage"), *SectionName));
	}

	// Map link method string to EAnimLinkMethod::Type
	EAnimLinkMethod::Type DesiredMethod;
	FString MethodLower = LinkMethodStr.ToLower();
	if (MethodLower == TEXT("absolute"))
	{
		DesiredMethod = EAnimLinkMethod::Absolute;
	}
	else if (MethodLower == TEXT("relative"))
	{
		DesiredMethod = EAnimLinkMethod::Relative;
	}
	else if (MethodLower == TEXT("proportional"))
	{
		DesiredMethod = EAnimLinkMethod::Proportional;
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid link_method '%s'. Must be absolute, relative, or proportional."), *LinkMethodStr));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimSetSectionLinkMethod", "MCP: Set Section Link Method"));
	Montage->Modify();

	Montage->CompositeSections[SectionIndex].ChangeLinkMethod(DesiredMethod);

	Montage->PostEditChange();
	Montage->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("set_section_link_method -> Section '%s' link method set to '%s'"), *SectionName, *LinkMethodStr);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// anim_add_segment
// ============================================================================

FString ClaireonAnimTool_AddSegment::GetOperation() const { return TEXT("add_segment"); }

FString ClaireonAnimTool_AddSegment::GetDescription() const
{
	return TEXT("Add an animation segment to a montage slot in the open editing session. Requires open session_id from anim_open. Transactional. The animation_path must point to a compatible AnimSequence using the same skeleton as the montage. New segments append to the end of the slot timeline.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_AddSegment::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddString(TEXT("anim_path"), TEXT("Asset path of the animation to add as a segment"), true);
	S.AddInteger(TEXT("slot_index"), TEXT("Slot track index (default 0)"));
	S.AddNumber(TEXT("start_pos"), TEXT("Start position in the montage timeline (default: end of track)"));
	S.AddNumber(TEXT("play_rate"), TEXT("Playback rate for the segment"));
	S.AddNumber(TEXT("anim_start_time"), TEXT("Start time within the source animation"));
	S.AddNumber(TEXT("anim_end_time"), TEXT("End time within the source animation"));
	S.AddString(TEXT("section_name"), TEXT("Also create a montage section at the segment start position"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_AddSegment::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	UAnimMontage* Montage = RequireMontage(Data, Error);
	if (!Montage) return Error;

	// Animation asset path (required)
	FString AnimPath;
	if (!Arguments->TryGetStringField(TEXT("anim_path"), AnimPath) || AnimPath.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: anim_path"));

	// Normalize caller-provided path through the central resolver before LoadObject.
	const auto AnimPathResolve = ClaireonPathResolver::Resolve(AnimPath);
	if (!AnimPathResolve.bSuccess)
		return MakeErrorResult(AnimPathResolve.Error);
	AnimPath = AnimPathResolve.ResolvedPath.Path;

	UAnimSequenceBase* AnimAsset = LoadObject<UAnimSequenceBase>(nullptr, *AnimPath);
	if (!AnimAsset)
		return MakeErrorResult(FString::Printf(TEXT("Failed to load animation: %s"), *AnimPath));

	// Slot index (default 0)
	double SlotIndexD = 0.0;
	Arguments->TryGetNumberField(TEXT("slot_index"), SlotIndexD);
	int32 SlotIndex = static_cast<int32>(SlotIndexD);

	if (SlotIndex < 0 || SlotIndex >= Montage->SlotAnimTracks.Num())
		return MakeErrorResult(FString::Printf(TEXT("Slot index %d out of range [0, %d)"), SlotIndex, Montage->SlotAnimTracks.Num()));

	FAnimTrack& Track = Montage->SlotAnimTracks[SlotIndex].AnimTrack;

	// Validate
	FText ValidationReason;
	if (!Track.IsValidToAdd(AnimAsset, &ValidationReason))
		return MakeErrorResult(FString::Printf(TEXT("Cannot add animation to slot: %s"), *ValidationReason.ToString()));

	// Start position: default to end of current track
	double StartPosD = -1.0;
	Arguments->TryGetNumberField(TEXT("start_pos"), StartPosD);
	float StartPos = (StartPosD >= 0.0) ? static_cast<float>(StartPosD) : Track.GetLength();

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimAddSegment", "MCP: Add Montage Segment"));
	Montage->Modify();

	FAnimSegment NewSeg;
	NewSeg.SetAnimReference(AnimAsset, true);
	NewSeg.StartPos = StartPos;

	// Optional overrides
	double PlayRate = 1.0;
	if (Arguments->TryGetNumberField(TEXT("play_rate"), PlayRate))
	{
		NewSeg.AnimPlayRate = static_cast<float>(PlayRate);
	}

	double AnimStartTime = -1.0;
	if (Arguments->TryGetNumberField(TEXT("anim_start_time"), AnimStartTime))
	{
		NewSeg.AnimStartTime = static_cast<float>(AnimStartTime);
	}

	double AnimEndTime = -1.0;
	if (Arguments->TryGetNumberField(TEXT("anim_end_time"), AnimEndTime))
	{
		NewSeg.AnimEndTime = static_cast<float>(AnimEndTime);
	}

	Track.AnimSegments.Add(NewSeg);
	Track.CollapseAnimSegments();

	// Find the segment we just added and read back its exact StartPos after collapse
	int32 NewIndex = Track.AnimSegments.Num() - 1;
	float ActualStartPos = Track.AnimSegments[NewIndex].StartPos;

	// Optionally create a section at the exact segment start position
	FString SectionName;
	if (Arguments->TryGetStringField(TEXT("section_name"), SectionName) && !SectionName.IsEmpty())
	{
		Montage->AddAnimCompositeSection(FName(*SectionName), ActualStartPos);
	}

	Montage->UpdateLinkableElements();
	Montage->PostEditChange();
	Montage->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("add_segment -> Added '%s' to slot [%d] at %.6fs [%d]%s"),
		*AnimAsset->GetName(), SlotIndex, ActualStartPos, NewIndex,
		SectionName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (section '%s')"), *SectionName));
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// anim_remove_segment
// ============================================================================

FString ClaireonAnimTool_RemoveSegment::GetOperation() const { return TEXT("remove_segment"); }

FString ClaireonAnimTool_RemoveSegment::GetDescription() const
{
	return TEXT("Remove a segment from a montage slot in the open animation editing session. Requires open session_id from anim_open. Transactional. Common pitfall: segment indices shift after removal, so cache them up front when removing multiple segments in sequence on the same slot.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_RemoveSegment::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("segment_index"), TEXT("Index of the segment to remove"), true);
	S.AddInteger(TEXT("slot_index"), TEXT("Slot track index (default 0)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_RemoveSegment::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	UAnimMontage* Montage = RequireMontage(Data, Error);
	if (!Montage) return Error;

	double SlotIndexD = 0.0;
	Arguments->TryGetNumberField(TEXT("slot_index"), SlotIndexD);
	int32 SlotIndex = static_cast<int32>(SlotIndexD);

	if (SlotIndex < 0 || SlotIndex >= Montage->SlotAnimTracks.Num())
		return MakeErrorResult(FString::Printf(TEXT("Slot index %d out of range [0, %d)"), SlotIndex, Montage->SlotAnimTracks.Num()));

	double SegIndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("segment_index"), SegIndexD))
		return MakeErrorResult(TEXT("Missing required parameter: segment_index"));
	int32 SegIndex = static_cast<int32>(SegIndexD);

	FAnimTrack& Track = Montage->SlotAnimTracks[SlotIndex].AnimTrack;
	if (SegIndex < 0 || SegIndex >= Track.AnimSegments.Num())
		return MakeErrorResult(FString::Printf(TEXT("Segment index %d out of range [0, %d)"), SegIndex, Track.AnimSegments.Num()));

	FString SegName = Track.AnimSegments[SegIndex].GetAnimReference() ? Track.AnimSegments[SegIndex].GetAnimReference()->GetName() : TEXT("null");

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimRemoveSegment", "MCP: Remove Montage Segment"));
	Montage->Modify();

	Track.AnimSegments.RemoveAt(SegIndex);
	Track.CollapseAnimSegments();
	Montage->UpdateLinkableElements();
	Montage->PostEditChange();
	Montage->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_segment -> Removed '%s' from slot [%d] segment [%d]"), *SegName, SlotIndex, SegIndex);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// anim_set_segment_property
// ============================================================================

FString ClaireonAnimTool_SetSegmentProperty::GetOperation() const { return TEXT("set_segment_property"); }

FString ClaireonAnimTool_SetSegmentProperty::GetDescription() const
{
	return TEXT("Set a property on a montage segment in the open animation editing session. Requires open session_id from anim_open. Transactional. Supports start_pos, end_pos, play_rate, animation_path, and loop_count. Common pitfall: changing animation_path validates skeleton compatibility before mutating.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_SetSegmentProperty::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("segment_index"), TEXT("Index of the segment to modify"), true);
	S.AddString(TEXT("property_name"), TEXT("Property to set (animation, play_rate, anim_start_time, anim_end_time, looping_count, start_pos)"), true);
	S.AddString(TEXT("value"), TEXT("New value for the property"), true);
	S.AddInteger(TEXT("slot_index"), TEXT("Slot track index (default 0)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_SetSegmentProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	UAnimMontage* Montage = RequireMontage(Data, Error);
	if (!Montage) return Error;

	double SlotIndexD = 0.0;
	Arguments->TryGetNumberField(TEXT("slot_index"), SlotIndexD);
	int32 SlotIndex = static_cast<int32>(SlotIndexD);

	if (SlotIndex < 0 || SlotIndex >= Montage->SlotAnimTracks.Num())
		return MakeErrorResult(FString::Printf(TEXT("Slot index %d out of range [0, %d)"), SlotIndex, Montage->SlotAnimTracks.Num()));

	double SegIndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("segment_index"), SegIndexD))
		return MakeErrorResult(TEXT("Missing required parameter: segment_index"));
	int32 SegIndex = static_cast<int32>(SegIndexD);

	FAnimTrack& Track = Montage->SlotAnimTracks[SlotIndex].AnimTrack;
	if (SegIndex < 0 || SegIndex >= Track.AnimSegments.Num())
		return MakeErrorResult(FString::Printf(TEXT("Segment index %d out of range [0, %d)"), SegIndex, Track.AnimSegments.Num()));

	FString PropertyName;
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: property_name"));

	FString Value;
	if (!Arguments->TryGetStringField(TEXT("value"), Value))
		return MakeErrorResult(TEXT("Missing required parameter: value"));

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimSetSegmentProp", "MCP: Set Segment Property"));
	Montage->Modify();

	FAnimSegment& Seg = Track.AnimSegments[SegIndex];
	FString PropLower = PropertyName.ToLower();

	if (PropLower == TEXT("anim_path") || PropLower == TEXT("animation"))
	{
		UAnimSequenceBase* NewAnim = LoadObject<UAnimSequenceBase>(nullptr, *Value);
		if (!NewAnim)
			return MakeErrorResult(FString::Printf(TEXT("Failed to load animation: %s"), *Value));
		Seg.SetAnimReference(NewAnim);
	}
	else if (PropLower == TEXT("play_rate"))
	{
		Seg.AnimPlayRate = FCString::Atof(*Value);
	}
	else if (PropLower == TEXT("anim_start_time"))
	{
		Seg.AnimStartTime = FCString::Atof(*Value);
	}
	else if (PropLower == TEXT("anim_end_time"))
	{
		Seg.AnimEndTime = FCString::Atof(*Value);
	}
	else if (PropLower == TEXT("looping_count"))
	{
		Seg.LoopingCount = FCString::Atoi(*Value);
	}
	else if (PropLower == TEXT("start_pos"))
	{
		Seg.StartPos = FCString::Atof(*Value);
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Unknown segment property '%s'. Supported: animation, play_rate, anim_start_time, anim_end_time, looping_count, start_pos"), *PropertyName));
	}

	Track.CollapseAnimSegments();
	Montage->UpdateLinkableElements();
	Montage->PostEditChange();
	Montage->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("set_segment_property -> slot [%d] segment [%d]: %s = %s"), SlotIndex, SegIndex, *PropertyName, *Value);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// anim_add_slot
// ============================================================================

FString ClaireonAnimTool_AddSlot::GetOperation() const { return TEXT("add_slot"); }

FString ClaireonAnimTool_AddSlot::GetDescription() const
{
	return TEXT("Add a new slot track to the montage in the open editing session. Requires open session_id from anim_open. Transactional. The slot_name must match a slot defined on the target skeleton (e.g. 'DefaultSlot', 'UpperBody'). Errors if the slot is unknown to the skeleton's slot tree.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_AddSlot::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddString(TEXT("slot_name"), TEXT("Name of the slot track to add"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_AddSlot::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	UAnimMontage* Montage = RequireMontage(Data, Error);
	if (!Montage) return Error;

	FString SlotName;
	if (!Arguments->TryGetStringField(TEXT("slot_name"), SlotName) || SlotName.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: slot_name"));

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimAddSlot", "MCP: Add Montage Slot"));
	Montage->Modify();

	Montage->AddSlot(FName(*SlotName));
	Montage->PostEditChange();
	Montage->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	int32 NewIndex = Montage->SlotAnimTracks.Num() - 1;
	Data->LastOperationStatus = FString::Printf(TEXT("add_slot -> Added slot '%s' [%d]"), *SlotName, NewIndex);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// anim_remove_slot
// ============================================================================

FString ClaireonAnimTool_RemoveSlot::GetOperation() const { return TEXT("remove_slot"); }

FString ClaireonAnimTool_RemoveSlot::GetDescription() const
{
	return TEXT("Remove a slot track from the montage in the open editing session. Requires open session_id from anim_open. Transactional. Common pitfall: removing the last slot is rejected because every montage requires at least one slot for playback. All segments on the removed slot are deleted with it.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_RemoveSlot::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("slot_index"), TEXT("Index of the slot track to remove"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_RemoveSlot::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	UAnimMontage* Montage = RequireMontage(Data, Error);
	if (!Montage) return Error;

	double SlotIndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("slot_index"), SlotIndexD))
		return MakeErrorResult(TEXT("Missing required parameter: slot_index"));
	int32 SlotIndex = static_cast<int32>(SlotIndexD);

	if (SlotIndex < 0 || SlotIndex >= Montage->SlotAnimTracks.Num())
		return MakeErrorResult(FString::Printf(TEXT("Slot index %d out of range [0, %d)"), SlotIndex, Montage->SlotAnimTracks.Num()));

	if (Montage->SlotAnimTracks.Num() <= 1)
		return MakeErrorResult(TEXT("Cannot remove the last slot track"));

	FString SlotName = Montage->SlotAnimTracks[SlotIndex].SlotName.ToString();

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimRemoveSlot", "MCP: Remove Montage Slot"));
	Montage->Modify();

	Montage->SlotAnimTracks.RemoveAt(SlotIndex);
	Montage->PostEditChange();
	Montage->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_slot -> Removed slot '%s' [%d]"), *SlotName, SlotIndex);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// anim_set_slot_property
// ============================================================================

FString ClaireonAnimTool_SetSlotProperty::GetOperation() const { return TEXT("set_slot_property"); }

FString ClaireonAnimTool_SetSlotProperty::GetDescription() const
{
	return TEXT("Rename a montage slot in the open animation editing session. Requires open session_id from anim_open. Transactional. The new slot name must exist in the skeleton slot tree. Common pitfall: this only updates the slot's identifier; it does not migrate existing animation segments between named slots.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_SetSlotProperty::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("slot_index"), TEXT("Index of the slot track to rename"), true);
	S.AddString(TEXT("slot_name"), TEXT("New name for the slot track"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_SetSlotProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	UAnimMontage* Montage = RequireMontage(Data, Error);
	if (!Montage) return Error;

	double SlotIndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("slot_index"), SlotIndexD))
		return MakeErrorResult(TEXT("Missing required parameter: slot_index"));
	int32 SlotIndex = static_cast<int32>(SlotIndexD);

	if (SlotIndex < 0 || SlotIndex >= Montage->SlotAnimTracks.Num())
		return MakeErrorResult(FString::Printf(TEXT("Slot index %d out of range [0, %d)"), SlotIndex, Montage->SlotAnimTracks.Num()));

	FString SlotName;
	if (!Arguments->TryGetStringField(TEXT("slot_name"), SlotName) || SlotName.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: slot_name"));

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimSetSlotProp", "MCP: Set Slot Property"));
	Montage->Modify();

	FString OldName = Montage->SlotAnimTracks[SlotIndex].SlotName.ToString();
	Montage->SlotAnimTracks[SlotIndex].SlotName = FName(*SlotName);
	Montage->PostEditChange();
	Montage->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("set_slot_property -> Renamed slot [%d] '%s' -> '%s'"), SlotIndex, *OldName, *SlotName);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// anim_inspect_segment
// ============================================================================

FString ClaireonAnimTool_InspectSegment::GetOperation() const { return TEXT("inspect_segment"); }

FString ClaireonAnimTool_InspectSegment::GetDescription() const
{
	return TEXT("Get detailed info about a montage segment in the open animation editing session. Requires open session_id from anim_open. Read-only. Returns segment timing, animation_path, play_rate, loop_count, and the list of notifies that fall inside the segment's time range.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_InspectSegment::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("segment_index"), TEXT("Index of the segment to inspect"), true);
	S.AddInteger(TEXT("slot_index"), TEXT("Slot track index (default 0)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_InspectSegment::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	UAnimMontage* Montage = RequireMontage(Data, Error);
	if (!Montage) return Error;

	double SlotIndexD = 0.0;
	Arguments->TryGetNumberField(TEXT("slot_index"), SlotIndexD);
	int32 SlotIndex = static_cast<int32>(SlotIndexD);

	if (SlotIndex < 0 || SlotIndex >= Montage->SlotAnimTracks.Num())
		return MakeErrorResult(FString::Printf(TEXT("Slot index %d out of range [0, %d)"), SlotIndex, Montage->SlotAnimTracks.Num()));

	double SegIndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("segment_index"), SegIndexD))
		return MakeErrorResult(TEXT("Missing required parameter: segment_index"));
	int32 SegIndex = static_cast<int32>(SegIndexD);

	const FAnimTrack& Track = Montage->SlotAnimTracks[SlotIndex].AnimTrack;
	if (SegIndex < 0 || SegIndex >= Track.AnimSegments.Num())
		return MakeErrorResult(FString::Printf(TEXT("Segment index %d out of range [0, %d)"), SegIndex, Track.AnimSegments.Num()));

	const FAnimSegment& Seg = Track.AnimSegments[SegIndex];
	UAnimSequenceBase* AnimRef = Seg.GetAnimReference();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("session_id"), SessionId);
	Result->SetNumberField(TEXT("slot_index"), SlotIndex);
	Result->SetNumberField(TEXT("segment_index"), SegIndex);
	Result->SetStringField(TEXT("animation"), AnimRef ? AnimRef->GetPathName() : TEXT("None"));
	Result->SetStringField(TEXT("animation_name"), AnimRef ? AnimRef->GetName() : TEXT("None"));
	Result->SetNumberField(TEXT("start_pos"), Seg.StartPos);
	Result->SetNumberField(TEXT("anim_start_time"), Seg.AnimStartTime);
	Result->SetNumberField(TEXT("anim_end_time"), Seg.AnimEndTime);
	Result->SetNumberField(TEXT("play_rate"), Seg.AnimPlayRate);
	Result->SetNumberField(TEXT("looping_count"), Seg.LoopingCount);
	Result->SetNumberField(TEXT("duration"), Seg.GetLength());
	Result->SetNumberField(TEXT("end_pos"), Seg.StartPos + Seg.GetLength());
	Result->SetNumberField(TEXT("source_length"), AnimRef ? AnimRef->GetPlayLength() : 0.0f);

	// Find notifies linked to this segment
	TArray<TSharedPtr<FJsonValue>> NotifyArray;
	for (int32 n = 0; n < Montage->Notifies.Num(); ++n)
	{
		const FAnimNotifyEvent& Notify = Montage->Notifies[n];
		if (Notify.GetSlotIndex() == SlotIndex && Notify.GetSegmentIndex() == SegIndex)
		{
			TSharedPtr<FJsonObject> NotifyObj = MakeShared<FJsonObject>();
			NotifyObj->SetNumberField(TEXT("notify_index"), n);
			NotifyObj->SetStringField(TEXT("name"), Notify.NotifyName.ToString());
			NotifyObj->SetNumberField(TEXT("time"), Notify.GetTime());
			if (Notify.NotifyStateClass)
			{
				NotifyObj->SetNumberField(TEXT("duration"), Notify.GetDuration());
				NotifyObj->SetStringField(TEXT("class"), Notify.NotifyStateClass->GetClass()->GetName());
			}
			else if (Notify.Notify)
			{
				NotifyObj->SetStringField(TEXT("class"), Notify.Notify->GetClass()->GetName());
			}
			// Show link method
			switch (Notify.GetLinkMethod())
			{
				case EAnimLinkMethod::Absolute: NotifyObj->SetStringField(TEXT("link_method"), TEXT("absolute")); break;
				case EAnimLinkMethod::Relative: NotifyObj->SetStringField(TEXT("link_method"), TEXT("relative")); break;
				case EAnimLinkMethod::Proportional: NotifyObj->SetStringField(TEXT("link_method"), TEXT("proportional")); break;
			}
			NotifyArray.Add(MakeShared<FJsonValueObject>(NotifyObj));
		}
	}
	Result->SetArrayField(TEXT("notifies"), NotifyArray);

	Data->LastOperationStatus = FString::Printf(TEXT("inspect_segment -> slot [%d] segment [%d]: %s (%.3fs-%.3fs)"),
		SlotIndex, SegIndex, AnimRef ? *AnimRef->GetName() : TEXT("None"), Seg.StartPos, Seg.StartPos + Seg.GetLength());
	Result->SetStringField(TEXT("status"), Data->LastOperationStatus);
	return MakeSuccessResult(Result, Data->LastOperationStatus);
}

// ============================================================================
// anim_retime_segment
// ============================================================================

FString ClaireonAnimTool_RetimeSegment::GetOperation() const { return TEXT("retime_segment"); }

FString ClaireonAnimTool_RetimeSegment::GetDescription() const
{
	return TEXT("Change the timing of a montage segment in the open animation editing session. Requires open session_id from anim_open. Transactional. Pass retime_notifies=true to scale notify times that fall inside the segment by the same factor; otherwise notify times are left as-is and may end up outside the new segment range.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_RetimeSegment::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("segment_index"), TEXT("Index of the segment to retime"), true);
	S.AddNumber(TEXT("new_end_time"), TEXT("New AnimEndTime for the segment"));
	S.AddNumber(TEXT("new_duration"), TEXT("Desired duration (adjusts play rate to achieve it)"));
	S.AddNumber(TEXT("new_play_rate"), TEXT("New play rate for the segment"));
	S.AddInteger(TEXT("slot_index"), TEXT("Slot track index (default 0)"));
	S.AddBoolean(TEXT("retime_notifies"), TEXT("Whether to retime notifies within the segment (default true)"));
	S.AddEnum(TEXT("notify_link_method"), TEXT("How to handle contained notifies when retiming"),
		{TEXT("manual"), TEXT("proportional"), TEXT("relative"), TEXT("absolute"), TEXT("none")});
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_RetimeSegment::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	UAnimMontage* Montage = RequireMontage(Data, Error);
	if (!Montage) return Error;

	double SlotIndexD = 0.0;
	Arguments->TryGetNumberField(TEXT("slot_index"), SlotIndexD);
	int32 SlotIndex = static_cast<int32>(SlotIndexD);

	if (SlotIndex < 0 || SlotIndex >= Montage->SlotAnimTracks.Num())
		return MakeErrorResult(FString::Printf(TEXT("Slot index %d out of range [0, %d)"), SlotIndex, Montage->SlotAnimTracks.Num()));

	double SegIndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("segment_index"), SegIndexD))
		return MakeErrorResult(TEXT("Missing required parameter: segment_index"));
	int32 SegIndex = static_cast<int32>(SegIndexD);

	FAnimTrack& Track = Montage->SlotAnimTracks[SlotIndex].AnimTrack;
	if (SegIndex < 0 || SegIndex >= Track.AnimSegments.Num())
		return MakeErrorResult(FString::Printf(TEXT("Segment index %d out of range [0, %d)"), SegIndex, Track.AnimSegments.Num()));

	FAnimSegment& Seg = Track.AnimSegments[SegIndex];

	// Capture old timing
	float OldStartPos = Seg.StartPos;
	float OldLength = Seg.GetLength();

	// Apply the requested change
	double NewEndTime = -1.0, NewDuration = -1.0, NewPlayRate = -1.0;
	Arguments->TryGetNumberField(TEXT("new_end_time"), NewEndTime);
	Arguments->TryGetNumberField(TEXT("new_duration"), NewDuration);
	Arguments->TryGetNumberField(TEXT("new_play_rate"), NewPlayRate);

	if (NewEndTime < 0.0 && NewDuration < 0.0 && NewPlayRate < 0.0)
		return MakeErrorResult(TEXT("At least one of new_end_time, new_duration, or new_play_rate is required"));

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimRetimeSegment", "MCP: Retime Montage Segment"));
	Montage->Modify();

	if (NewEndTime >= 0.0)
	{
		Seg.AnimEndTime = static_cast<float>(NewEndTime);
	}
	else if (NewPlayRate > 0.0)
	{
		Seg.AnimPlayRate = static_cast<float>(NewPlayRate);
	}
	else if (NewDuration > 0.0)
	{
		// Adjust play rate to achieve desired duration
		float AnimRange = Seg.AnimEndTime - Seg.AnimStartTime;
		if (AnimRange > 0.0f)
		{
			Seg.AnimPlayRate = (AnimRange * Seg.LoopingCount) / static_cast<float>(NewDuration);
		}
	}

	float NewLength = Seg.GetLength();
	float LengthRatio = (OldLength > 0.0f) ? (NewLength / OldLength) : 1.0f;

	// Determine notify retiming mode
	//   "manual" (default) - proportionally scale contained notifies ourselves
	//   "proportional"     - change link method to Proportional, let engine auto-scale
	//   "relative"         - change link method to Relative (move with segment start, don't scale)
	//   "absolute"         - change link method to Absolute (don't move at all)
	//   "none"             - don't touch notifies
	FString NotifyMode = TEXT("manual");
	{
		bool bRetimeNotifies = true;
		if (Arguments->TryGetBoolField(TEXT("retime_notifies"), bRetimeNotifies) && !bRetimeNotifies)
		{
			NotifyMode = TEXT("none");
		}
		FString ModeStr;
		if (Arguments->TryGetStringField(TEXT("notify_link_method"), ModeStr))
		{
			NotifyMode = ModeStr.ToLower();
		}
	}

	if (NotifyMode != TEXT("none") && FMath::Abs(LengthRatio - 1.0f) > KINDA_SMALL_NUMBER)
	{
		float OldSegEnd = OldStartPos + OldLength;

		if (NotifyMode == TEXT("proportional") || NotifyMode == TEXT("relative") || NotifyMode == TEXT("absolute"))
		{
			// Change link method only on notifies fully contained within this segment
			EAnimLinkMethod::Type DesiredMethod = EAnimLinkMethod::Absolute;
			if (NotifyMode == TEXT("proportional")) DesiredMethod = EAnimLinkMethod::Proportional;
			else if (NotifyMode == TEXT("relative")) DesiredMethod = EAnimLinkMethod::Relative;

			for (FAnimNotifyEvent& Notify : Montage->Notifies)
			{
				float NotifyStart = Notify.GetTime();
				float NotifyEnd = Notify.NotifyStateClass ? (NotifyStart + Notify.GetDuration()) : NotifyStart;
				if (NotifyStart < OldStartPos - KINDA_SMALL_NUMBER || NotifyEnd > OldSegEnd + KINDA_SMALL_NUMBER)
				{
					continue;
				}
				if (Notify.GetLinkMethod() != DesiredMethod)
				{
					Notify.ChangeLinkMethod(DesiredMethod);
				}
			}
		}
		else // "manual" - proportionally scale contained notifies ourselves
		{
			for (FAnimNotifyEvent& Notify : Montage->Notifies)
			{
				float NotifyStart = Notify.GetTime();
				float NotifyEnd = Notify.NotifyStateClass ? (NotifyStart + Notify.GetDuration()) : NotifyStart;

				// Only retime notifies fully contained within this segment's old time range
				if (NotifyStart < OldStartPos - KINDA_SMALL_NUMBER || NotifyEnd > OldSegEnd + KINDA_SMALL_NUMBER)
				{
					continue;
				}

				float RelativePos = (OldLength > 0.0f) ? (NotifyStart - OldStartPos) / OldLength : 0.0f;
				Notify.SetTime(OldStartPos + RelativePos * NewLength);

				if (Notify.NotifyStateClass && Notify.GetDuration() > 0.0f)
				{
					Notify.SetDuration(Notify.GetDuration() * LengthRatio);
				}
			}
		}
	}

	Track.CollapseAnimSegments();
	Montage->UpdateLinkableElements();
	Montage->PostEditChange();
	Montage->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("retime_segment -> slot [%d] segment [%d]: %.3fs -> %.3fs (ratio: %.3f, notify_mode: %s)"),
		SlotIndex, SegIndex, OldLength, NewLength, LengthRatio, *NotifyMode);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// anim_batch_retime
// ============================================================================

FString ClaireonAnimTool_BatchRetimeAnimation::GetOperation() const { return TEXT("batch_retime"); }

FString ClaireonAnimTool_BatchRetimeAnimation::GetDescription() const
{
	return TEXT("Find all montages referencing an animation and retime their segments after the animation changed length. Stateless / non-session: opens each affected montage internally with a transaction. Common pitfall: this scans the asset registry for references and may modify many assets, so dry-run first.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_BatchRetimeAnimation::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddString(TEXT("anim_path"), TEXT("Asset path of the animation whose length changed"), true);
	S.AddNumber(TEXT("new_length"), TEXT("New length of the animation (default: current asset length)"));
	S.AddNumber(TEXT("old_length"), TEXT("Previous length of the animation (auto-detected from segment if omitted)"));
	S.AddBoolean(TEXT("retime_notifies"), TEXT("Whether to retime notifies within affected segments (default true)"));
	S.AddString(TEXT("notify_link_method"), TEXT("Notify retiming mode: manual, proportional, relative, absolute, none"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_BatchRetimeAnimation::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	UAnimMontage* Montage = RequireMontage(Data, Error);
	if (!Montage) return Error;

	FString AnimPath;
	if (!Arguments->TryGetStringField(TEXT("anim_path"), AnimPath) || AnimPath.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: anim_path"));

	// Normalize caller-provided path through the central resolver before LoadObject.
	const auto AnimPathResolve = ClaireonPathResolver::Resolve(AnimPath);
	if (!AnimPathResolve.bSuccess)
		return MakeErrorResult(AnimPathResolve.Error);
	AnimPath = AnimPathResolve.ResolvedPath.Path;

	UAnimSequenceBase* TargetAnim = LoadObject<UAnimSequenceBase>(nullptr, *AnimPath);
	if (!TargetAnim)
		return MakeErrorResult(FString::Printf(TEXT("Failed to load animation: %s"), *AnimPath));

	float CurrentAnimLength = TargetAnim->GetPlayLength();

	double NewLengthD = -1.0;
	Arguments->TryGetNumberField(TEXT("new_length"), NewLengthD);
	float NewLength = (NewLengthD > 0.0) ? static_cast<float>(NewLengthD) : CurrentAnimLength;

	double OldLengthD = -1.0;
	Arguments->TryGetNumberField(TEXT("old_length"), OldLengthD);
	float OldSourceLength = (OldLengthD > 0.0) ? static_cast<float>(OldLengthD) : 0.0f; // 0 = auto-detect from segment

	// Notify retiming mode: same options as retime_segment
	FString NotifyMode = TEXT("manual");
	{
		bool bRetimeNotifies = true;
		if (Arguments->TryGetBoolField(TEXT("retime_notifies"), bRetimeNotifies) && !bRetimeNotifies)
		{
			NotifyMode = TEXT("none");
		}
		FString ModeStr;
		if (Arguments->TryGetStringField(TEXT("notify_link_method"), ModeStr))
		{
			NotifyMode = ModeStr.ToLower();
		}
	}

	// Use GetReferencers to find only montages that actually reference this animation
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FName AnimPackageName = TargetAnim->GetOutermost()->GetFName();

	TArray<FAssetIdentifier> Referencers;
	AssetRegistry.GetReferencers(FAssetIdentifier(AnimPackageName), Referencers, UE::AssetRegistry::EDependencyCategory::Package);

	int32 MontagesUpdated = 0;
	int32 SegmentsUpdated = 0;
	TArray<TSharedPtr<FJsonValue>> UpdatedMontages;

	for (const FAssetIdentifier& Ref : Referencers)
	{
		// Only load if it's a montage - check asset data first
		TArray<FAssetData> AssetsInPackage;
		AssetRegistry.GetAssetsByPackageName(Ref.PackageName, AssetsInPackage);

		for (const FAssetData& AssetData : AssetsInPackage)
		{
			if (!AssetData.AssetClassPath.GetAssetName().ToString().Contains(TEXT("AnimMontage")))
			{
				continue;
			}

			UAnimMontage* RefMontage = Cast<UAnimMontage>(AssetData.GetAsset());
			if (!RefMontage) continue;

			bool bMontageModified = false;

			FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimBatchRetime", "MCP: Batch Retime Animation"));
			RefMontage->Modify();

			for (int32 SlotIdx = 0; SlotIdx < RefMontage->SlotAnimTracks.Num(); ++SlotIdx)
			{
				FAnimTrack& Track = RefMontage->SlotAnimTracks[SlotIdx].AnimTrack;
				for (int32 SegIdx = 0; SegIdx < Track.AnimSegments.Num(); ++SegIdx)
				{
					FAnimSegment& Seg = Track.AnimSegments[SegIdx];
					if (Seg.GetAnimReference() != TargetAnim) continue;

					float OldStartPos = Seg.StartPos;
					float OldLength = Seg.GetLength();

					// Scale AnimEndTime and AnimStartTime proportionally rather than replacing them.
					// This preserves trims: if segment was trimmed to 80% of old source, it stays at 80% of new source.
					float EffectiveOldSource = (OldSourceLength > 0.0f) ? OldSourceLength : Seg.AnimEndTime;
					float ScaleRatio = (EffectiveOldSource > 0.0f) ? (NewLength / EffectiveOldSource) : 1.0f;
					Seg.AnimEndTime *= ScaleRatio;
					Seg.AnimStartTime *= ScaleRatio;

					float NewSegLength = Seg.GetLength();
					float LengthRatio = (OldLength > 0.0f) ? (NewSegLength / OldLength) : 1.0f;

					// Retime contained notifies
					if (NotifyMode != TEXT("none") && FMath::Abs(LengthRatio - 1.0f) > KINDA_SMALL_NUMBER)
					{
						float OldSegEnd = OldStartPos + OldLength;

						if (NotifyMode == TEXT("proportional") || NotifyMode == TEXT("relative") || NotifyMode == TEXT("absolute"))
						{
							EAnimLinkMethod::Type DesiredMethod = EAnimLinkMethod::Absolute;
							if (NotifyMode == TEXT("proportional")) DesiredMethod = EAnimLinkMethod::Proportional;
							else if (NotifyMode == TEXT("relative")) DesiredMethod = EAnimLinkMethod::Relative;

							for (FAnimNotifyEvent& Notify : RefMontage->Notifies)
							{
								float NotifyStart = Notify.GetTime();
								float NotifyEnd = Notify.NotifyStateClass ? (NotifyStart + Notify.GetDuration()) : NotifyStart;
								if (NotifyStart < OldStartPos - KINDA_SMALL_NUMBER || NotifyEnd > OldSegEnd + KINDA_SMALL_NUMBER)
									continue;
								if (Notify.GetLinkMethod() != DesiredMethod)
									Notify.ChangeLinkMethod(DesiredMethod);
							}
						}
						else // manual
						{
							for (FAnimNotifyEvent& Notify : RefMontage->Notifies)
							{
								float NotifyStart = Notify.GetTime();
								float NotifyEnd = Notify.NotifyStateClass ? (NotifyStart + Notify.GetDuration()) : NotifyStart;
								if (NotifyStart < OldStartPos - KINDA_SMALL_NUMBER || NotifyEnd > OldSegEnd + KINDA_SMALL_NUMBER)
									continue;

								float RelativePos = (OldLength > 0.0f) ? (NotifyStart - OldStartPos) / OldLength : 0.0f;
								Notify.SetTime(OldStartPos + RelativePos * NewSegLength);
								if (Notify.NotifyStateClass && Notify.GetDuration() > 0.0f)
									Notify.SetDuration(Notify.GetDuration() * LengthRatio);
							}
						}
					}

					bMontageModified = true;
					SegmentsUpdated++;
				}

				if (bMontageModified)
				{
					Track.CollapseAnimSegments();
				}
			}

			if (bMontageModified)
			{
				RefMontage->UpdateLinkableElements();
				RefMontage->PostEditChange();
				RefMontage->MarkPackageDirty();
				ClaireonAssetUtils::RefreshAssetEditorIfOpen(RefMontage);
				MontagesUpdated++;

				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("montage_path"), RefMontage->GetPathName());
				Entry->SetStringField(TEXT("montage_name"), RefMontage->GetName());
				UpdatedMontages.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("session_id"), SessionId);
	Result->SetStringField(TEXT("anim_path"), AnimPath);
	Result->SetNumberField(TEXT("new_length"), NewLength);
	Result->SetNumberField(TEXT("montages_updated"), MontagesUpdated);
	Result->SetNumberField(TEXT("segments_updated"), SegmentsUpdated);
	Result->SetArrayField(TEXT("updated_montages"), UpdatedMontages);

	Data->LastOperationStatus = FString::Printf(TEXT("batch_retime_animation -> Updated %d segment(s) in %d montage(s) for '%s' (new length: %.3fs)"),
		SegmentsUpdated, MontagesUpdated, *TargetAnim->GetName(), NewLength);
	Result->SetStringField(TEXT("status"), Data->LastOperationStatus);
	return MakeSuccessResult(Result, Data->LastOperationStatus);
}
