// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Curves/RichCurve.h"

class UAnimationAsset;
class UAnimSequenceBase;
class UAnimSequence;
class UAnimMontage;
class UAnimComposite;
class UAnimNotify;
class UAnimNotifyState;
struct FAnimNotifyEvent;

/**
 * Shared helper functions for animation MCP tools (inspect + edit).
 * Provides asset loading, structure formatting, notify/curve/section manipulation,
 * and class resolution utilities.
 */
namespace ClaireonAnimHelpers
{
	// ========================================================================
	// Asset Loading
	// ========================================================================

	/** Load an animation asset (AnimSequence, AnimMontage, or AnimComposite). Auto-detects type. */
	UAnimSequenceBase* LoadAnimAsset(const FString& AssetPath, FString& OutAssetType, FString& OutError);

	// ========================================================================
	// Formatting Functions
	// ========================================================================

	/** Format the full structure of an animation asset as human-readable text. Dispatches by type. */
	FString FormatAnimStructure(const UAnimSequenceBase* Anim, const FString& AssetType, bool bFullDetail = true, const FString& FocusSection = FString());

	/** Format a single FAnimNotifyEvent with full sub-object property detail. Used by notify_index focus. */
	FString FormatSingleNotify(const UAnimSequenceBase* Anim, int32 NotifyIndex);

	/** Format a single FAnimNotifyEvent. */
	FString FormatNotifyEvent(const UAnimSequenceBase* Anim, const FAnimNotifyEvent& Event, int32 Index, bool bFullDetail = true);

	/** Format all notifies on an animation. */
	FString FormatNotifies(const UAnimSequenceBase* Anim, bool bFullDetail = true);

	/** Format all curves on an animation. */
	FString FormatCurves(const UAnimSequenceBase* Anim, bool bFullDetail = true);

	/** Format sync markers (AnimSequence only). */
	FString FormatSyncMarkers(const UAnimSequence* AnimSeq);

	/** Format montage sections. */
	FString FormatMontageSections(const UAnimMontage* Montage);

	/** Format montage slots. */
	FString FormatMontageSlots(const UAnimMontage* Montage);

	/** Format montage blend settings. */
	FString FormatMontageBlendSettings(const UAnimMontage* Montage);

	/** Format data modifiers (AnimSequence only). */
	FString FormatModifiers(const UAnimSequence* AnimSeq, bool bFullDetail = true);

	/** Format animation metadata. Accepts UAnimationAsset* so both anim sequences and blend spaces can use it. */
	FString FormatMetadata(const UAnimationAsset* Asset, bool bFullDetail = true);

	/** Format all properties of a notify sub-object as indented text. */
	FString FormatNotifySubObjectProperties(const UObject* NotifyObj, const FString& Indent = TEXT("      "));

	// ========================================================================
	// Notify Helpers
	// ========================================================================

	/** Resolve a notify class name (short or full) to UClass*. bIsState indicates looking for UAnimNotifyState subclass. */
	UClass* ResolveNotifyClass(const FString& ClassName, bool bIsState, FString& OutError);

	/** Add a skeleton-style notify (no sub-object, plain name only). Registers name on skeleton. Returns index or -1. */
	int32 AddSkeletonNotify(UAnimSequenceBase* Anim, const FString& NotifyName, float Time, int32 TrackIndex, FString& OutError);

	/** Add a class-based notify with sub-object. Returns index or -1. */
	int32 AddClassNotify(UAnimSequenceBase* Anim, UClass* NotifyClass, float Time, float Duration, int32 TrackIndex, FString& OutError);

	/** Remove a notify by index. */
	bool RemoveNotify(UAnimSequenceBase* Anim, int32 NotifyIndex, FString& OutError);

	/** Move a notify to a new time and/or track. Pass -1 for unchanged values. */
	bool MoveNotify(UAnimSequenceBase* Anim, int32 NotifyIndex, float NewTime, float NewDuration, int32 NewTrackIndex, FString& OutError);

	/** Set a property on a notify's sub-object. */
	bool SetNotifyProperty(UAnimSequenceBase* Anim, int32 NotifyIndex, const FString& PropertyName, const FString& PropertyValue, FString& OutError);

	/** Get a property from a notify's sub-object. */
	FString GetNotifyProperty(const UAnimSequenceBase* Anim, int32 NotifyIndex, const FString& PropertyName, FString& OutError);

	// ========================================================================
	// Curve Helpers
	// ========================================================================

	/** Add a float curve. */
	bool AddCurve(UAnimSequenceBase* Anim, const FString& CurveName, FString& OutError);

	/** Remove a curve by name. */
	bool RemoveCurve(UAnimSequenceBase* Anim, const FString& CurveName, FString& OutError);

	/** Add a key to a float curve. Accepts a fully configured FRichCurveKey. */
	bool AddCurveKey(UAnimSequenceBase* Anim, const FString& CurveName, const FRichCurveKey& Key, FString& OutError);

	/** Remove a curve key by time (snaps to nearest key within tolerance). */
	bool RemoveCurveKey(UAnimSequenceBase* Anim, const FString& CurveName, float Time, FString& OutError);

	/** Find a curve key by time (with tolerance snapping). Returns nullptr if not found. */
	const FRichCurveKey* FindCurveKey(const UAnimSequenceBase* Anim, const FString& CurveName, float Time, float& OutSnappedTime, FString& OutError);

	/** Set a property on an existing curve key (interp_mode, tangent_mode, arrive_tangent, leave_tangent, etc.) */
	bool SetCurveKeyProperty(UAnimSequenceBase* Anim, const FString& CurveName, float Time, const FString& PropertyName, const FString& Value, FString& OutError);

	// ========================================================================
	// Montage Section Helpers
	// ========================================================================

	/** Add a section to a montage. */
	bool AddMontageSection(UAnimMontage* Montage, const FString& SectionName, float StartTime, FString& OutError);

	/** Remove a montage section. */
	bool RemoveMontageSection(UAnimMontage* Montage, const FString& SectionName, FString& OutError);

	/** Set the next-section link for combo chains. */
	bool SetMontageSectionLink(UAnimMontage* Montage, const FString& SectionName, const FString& NextSectionName, FString& OutError);

} // namespace ClaireonAnimHelpers
