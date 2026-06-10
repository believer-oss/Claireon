// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimHelpers.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "Animation/AnimationAsset.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimMetaData.h"
#include "AnimationModifier.h"
#include "AnimationModifiersAssetUserData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// ============================================================================
// Asset Loading
// ============================================================================

UAnimSequenceBase* ClaireonAnimHelpers::LoadAnimAsset(const FString& AssetPath, FString& OutAssetType, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return nullptr;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	// Try loading via soft object path first for reliable package resolution
	FSoftObjectPath SoftPath(ResolvedPath);
	UObject* LoadedObj = SoftPath.TryLoad();
	if (!LoadedObj)
	{
		// Fallback: try LoadObject directly
		LoadedObj = LoadObject<UAnimSequenceBase>(nullptr, *ResolvedPath);
	}

	if (!LoadedObj)
	{
		OutError = FString::Printf(TEXT("Failed to load asset at path: %s. Verify the path is correct and the asset exists."), *ResolvedPath);
		return nullptr;
	}

	UAnimSequenceBase* Anim = Cast<UAnimSequenceBase>(LoadedObj);
	if (!Anim)
	{
		OutError = FString::Printf(TEXT("Asset at %s is not an animation (actual type: %s). Expected AnimSequence, AnimMontage, or AnimComposite."),
			*ResolvedPath, *LoadedObj->GetClass()->GetName());
		return nullptr;
	}

	// Detect type: check montage first (montage IS-A sequence), then composite, then sequence
	if (Cast<UAnimMontage>(Anim))
	{
		OutAssetType = TEXT("AnimMontage");
	}
	else if (Cast<UAnimComposite>(Anim))
	{
		OutAssetType = TEXT("AnimComposite");
	}
	else if (Cast<UAnimSequence>(Anim))
	{
		OutAssetType = TEXT("AnimSequence");
	}
	else
	{
		OutAssetType = TEXT("AnimSequenceBase");
	}

	return Anim;
}

// ============================================================================
// Formatting Helpers
// ============================================================================

FString ClaireonAnimHelpers::FormatNotifySubObjectProperties(const UObject* NotifyObj, const FString& Indent)
{
	if (!NotifyObj)
	{
		return FString();
	}

	FString Output;
	const UClass* ObjClass = NotifyObj->GetClass();

	for (TFieldIterator<FProperty> PropIt(ObjClass, EFieldIteratorFlags::IncludeSuper); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;

		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
		{
			continue;
		}

		// Skip internal/inherited UObject properties that clutter the output
		const FString PropName = Prop->GetName();
		if (PropName == TEXT("NativeClass") || PropName == TEXT("ObjectFlags") || PropName == TEXT("bIsNativeBranchingPoint"))
		{
			continue;
		}

		// Skip properties inherited from UAnimNotify/UAnimNotifyState base that are not useful
		if (PropName == TEXT("NotifyColor"))
		{
			continue;
		}

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(NotifyObj);
		FString ValueStr;
		Prop->ExportText_Direct(ValueStr, ValuePtr, ValuePtr, nullptr, PPF_None);

		// Skip empty/default values to reduce noise
		if (!ValueStr.IsEmpty() && ValueStr != TEXT("None") && ValueStr != TEXT("0") && ValueStr != TEXT("()") && ValueStr != TEXT("0.000000"))
		{
			if (ValueStr.Len() > 200)
			{
				ValueStr = ValueStr.Left(197) + TEXT("...");
			}
			Output += FString::Printf(TEXT("%s%s = %s\n"), *Indent, *PropName, *ValueStr);
		}
	}

	return Output;
}

FString ClaireonAnimHelpers::FormatNotifyEvent(const UAnimSequenceBase* Anim, const FAnimNotifyEvent& Event, int32 Index, bool bFullDetail)
{
	FString Output;

	const float TriggerTime = Event.GetTriggerTime();
	const float Duration = Event.GetDuration();
	const int32 TrackIndex = Event.TrackIndex;

	// Determine notify type and format accordingly
	const bool bIsSkeleton = (Event.Notify == nullptr && Event.NotifyStateClass == nullptr);
	const bool bIsState = (Event.NotifyStateClass != nullptr);

	if (bIsSkeleton)
	{
		// Skeleton-style notify (plain name, no sub-object)
		Output += FString::Printf(TEXT("[%d] \"%s\" (skeleton) @ %.3fs [Track %d]\n"),
			Index, *Event.NotifyName.ToString(), TriggerTime, TrackIndex);
	}
	else if (bIsState)
	{
		// State notify (has duration)
		const float EndTime = TriggerTime + Duration;
		const FString ClassName = Event.NotifyStateClass->GetClass()->GetName();
		Output += FString::Printf(TEXT("[%d] %s @ %.3fs-%.3fs (%.3fs) [Track %d]\n"),
			Index, *ClassName, TriggerTime, EndTime, Duration, TrackIndex);

		if (bFullDetail)
		{
			FString Props = FormatNotifySubObjectProperties(Event.NotifyStateClass, TEXT("      "));
			if (!Props.IsEmpty())
			{
				Output += Props;
			}
		}
	}
	else
	{
		// Class-based instant notify
		const FString ClassName = Event.Notify->GetClass()->GetName();
		Output += FString::Printf(TEXT("[%d] %s @ %.3fs [Track %d]\n"),
			Index, *ClassName, TriggerTime, TrackIndex);

		if (bFullDetail)
		{
			FString Props = FormatNotifySubObjectProperties(Event.Notify, TEXT("      "));
			if (!Props.IsEmpty())
			{
				Output += Props;
			}
		}
	}

	return Output;
}

FString ClaireonAnimHelpers::FormatSingleNotify(const UAnimSequenceBase* Anim, int32 NotifyIndex)
{
	if (!Anim || NotifyIndex < 0 || NotifyIndex >= Anim->Notifies.Num())
	{
		return TEXT("(invalid notify index)");
	}

	// Always format with full detail for single-notify focus
	return FormatNotifyEvent(Anim, Anim->Notifies[NotifyIndex], NotifyIndex, /*bFullDetail=*/ true);
}

FString ClaireonAnimHelpers::FormatNotifies(const UAnimSequenceBase* Anim, bool bFullDetail)
{
	if (!Anim)
	{
		return FString();
	}

	FString Output;
	Output += TEXT("=== Notifies ===\n");

	if (Anim->Notifies.Num() == 0)
	{
		Output += TEXT("  (none)\n");
		return Output;
	}

	// Show track info
	Output += FString::Printf(TEXT("  Tracks: %d\n"), Anim->AnimNotifyTracks.Num());
	for (int32 TrackIdx = 0; TrackIdx < Anim->AnimNotifyTracks.Num(); ++TrackIdx)
	{
		Output += FString::Printf(TEXT("    [%d] %s\n"), TrackIdx, *Anim->AnimNotifyTracks[TrackIdx].TrackName.ToString());
	}
	Output += TEXT("\n");

	// Show each notify
	for (int32 i = 0; i < Anim->Notifies.Num(); ++i)
	{
		Output += TEXT("  ") + FormatNotifyEvent(Anim, Anim->Notifies[i], i, bFullDetail);
	}

	return Output;
}

FString ClaireonAnimHelpers::FormatCurves(const UAnimSequenceBase* Anim, bool bFullDetail)
{
	if (!Anim)
	{
		return FString();
	}

	const IAnimationDataModel* DataModel = Anim->GetDataModel();
	if (!DataModel)
	{
		return TEXT("=== Curves ===\n  (no data model)\n");
	}

	FString Output;
	Output += TEXT("=== Curves ===\n");

	const TArray<FFloatCurve>& FloatCurves = DataModel->GetFloatCurves();

	if (FloatCurves.Num() == 0)
	{
		Output += TEXT("  (none)\n");
		return Output;
	}

	Output += FString::Printf(TEXT("  Float Curves: %d\n"), FloatCurves.Num());

	for (int32 i = 0; i < FloatCurves.Num(); ++i)
	{
		const FFloatCurve& Curve = FloatCurves[i];
		const FRichCurve& RichCurve = Curve.FloatCurve;
		const int32 KeyCount = RichCurve.GetNumKeys();

		Output += FString::Printf(TEXT("  [%d] \"%s\" (%d keys)"),
			i, *Curve.GetName().ToString(), KeyCount);

		if (KeyCount > 0)
		{
			const TArray<FRichCurveKey>& Keys = RichCurve.GetConstRefOfKeys();
			Output += FString::Printf(TEXT(" [%.3fs - %.3fs]"),
				Keys[0].Time, Keys.Last().Time);
		}

		Output += TEXT("\n");

		if (bFullDetail && KeyCount > 0)
		{
			const TArray<FRichCurveKey>& Keys = RichCurve.GetConstRefOfKeys();
			// Show up to 20 keys for readability
			const int32 MaxDisplay = FMath::Min(KeyCount, 20);
			for (int32 k = 0; k < MaxDisplay; ++k)
			{
				Output += FString::Printf(TEXT("      t=%.3f v=%.4f\n"), Keys[k].Time, Keys[k].Value);
			}
			if (KeyCount > MaxDisplay)
			{
				Output += FString::Printf(TEXT("      ... +%d more keys\n"), KeyCount - MaxDisplay);
			}
		}
	}

	return Output;
}

FString ClaireonAnimHelpers::FormatSyncMarkers(const UAnimSequence* AnimSeq)
{
	if (!AnimSeq)
	{
		return FString();
	}

	FString Output;
	Output += TEXT("=== Sync Markers ===\n");

	const TArray<FAnimSyncMarker>& Markers = AnimSeq->AuthoredSyncMarkers;

	if (Markers.Num() == 0)
	{
		Output += TEXT("  (none)\n");
		return Output;
	}

	for (int32 i = 0; i < Markers.Num(); ++i)
	{
		const FAnimSyncMarker& Marker = Markers[i];
		Output += FString::Printf(TEXT("  [%d] \"%s\" @ %.3fs [Track %d]\n"),
			i, *Marker.MarkerName.ToString(), Marker.Time, Marker.TrackIndex);
	}

	return Output;
}

FString ClaireonAnimHelpers::FormatMontageSections(const UAnimMontage* Montage)
{
	if (!Montage)
	{
		return FString();
	}

	FString Output;
	Output += TEXT("=== Sections ===\n");

	const TArray<FCompositeSection>& Sections = Montage->CompositeSections;

	if (Sections.Num() == 0)
	{
		Output += TEXT("  (none)\n");
		return Output;
	}

	for (int32 i = 0; i < Sections.Num(); ++i)
	{
		const FCompositeSection& Section = Sections[i];
		const float StartTime = Section.GetTime();
		const FString NextSection = Section.NextSectionName.IsNone() ? TEXT("(end)") : Section.NextSectionName.ToString();

		Output += FString::Printf(TEXT("  [%d] \"%s\" @ %.3fs -> %s\n"),
			i, *Section.SectionName.ToString(), StartTime, *NextSection);
	}

	return Output;
}

FString ClaireonAnimHelpers::FormatMontageSlots(const UAnimMontage* Montage)
{
	if (!Montage)
	{
		return FString();
	}

	FString Output;
	Output += TEXT("=== Slots ===\n");

	const TArray<FSlotAnimationTrack>& SlotTracks = Montage->SlotAnimTracks;

	if (SlotTracks.Num() == 0)
	{
		Output += TEXT("  (none)\n");
		return Output;
	}

	for (int32 i = 0; i < SlotTracks.Num(); ++i)
	{
		const FSlotAnimationTrack& Slot = SlotTracks[i];
		Output += FString::Printf(TEXT("  [%d] Slot: %s (%d segments)\n"),
			i, *Slot.SlotName.ToString(), Slot.AnimTrack.AnimSegments.Num());

		for (int32 j = 0; j < Slot.AnimTrack.AnimSegments.Num(); ++j)
		{
			const FAnimSegment& Seg = Slot.AnimTrack.AnimSegments[j];
			const FString AnimName = Seg.GetAnimReference() ? Seg.GetAnimReference()->GetName() : TEXT("None");
			Output += FString::Printf(TEXT("    [%d] %s @ %.3fs (%.3fs)\n"),
				j, *AnimName, Seg.StartPos, Seg.GetLength());
			Output += FString::Printf(TEXT("        AnimRange: %.3fs - %.3fs | PlayRate: %.1f | Loops: %d\n"),
				Seg.AnimStartTime, Seg.AnimEndTime, Seg.AnimPlayRate, Seg.LoopingCount);

			// Show notifies linked to this segment
			TArray<FString> SegNotifies;
			for (int32 n = 0; n < Montage->Notifies.Num(); ++n)
			{
				const FAnimNotifyEvent& Notify = Montage->Notifies[n];
				if (Notify.GetSlotIndex() == i && Notify.GetSegmentIndex() == j)
				{
					const FString NotifyName = Notify.NotifyName.ToString();
					if (Notify.NotifyStateClass)
					{
						SegNotifies.Add(FString::Printf(TEXT("[%d] %s @ %.3fs-%.3fs"), n, *NotifyName, Notify.GetTime(), Notify.GetTime() + Notify.GetDuration()));
					}
					else
					{
						SegNotifies.Add(FString::Printf(TEXT("[%d] %s @ %.3fs"), n, *NotifyName, Notify.GetTime()));
					}
				}
			}
			if (SegNotifies.Num() > 0)
			{
				Output += FString::Printf(TEXT("        Notifies: %s\n"), *FString::Join(SegNotifies, TEXT(", ")));
			}
		}
	}

	return Output;
}

FString ClaireonAnimHelpers::FormatMontageBlendSettings(const UAnimMontage* Montage)
{
	if (!Montage)
	{
		return FString();
	}

	FString Output;
	Output += TEXT("=== Blend Settings ===\n");

	// Blend In
	const FAlphaBlendArgs& BlendIn = Montage->BlendIn;
	Output += FString::Printf(TEXT("  BlendIn: %.3fs"), BlendIn.BlendTime);

	// Blend mode
	if (Montage->BlendModeIn == EMontageBlendMode::Standard)
	{
		Output += TEXT(" (Standard)");
	}
	else if (Montage->BlendModeIn == EMontageBlendMode::Inertialization)
	{
		Output += TEXT(" (Inertialization)");
	}
	Output += TEXT("\n");

	// Blend Out
	const FAlphaBlendArgs& BlendOut = Montage->BlendOut;
	Output += FString::Printf(TEXT("  BlendOut: %.3fs"), BlendOut.BlendTime);

	if (Montage->BlendModeOut == EMontageBlendMode::Standard)
	{
		Output += TEXT(" (Standard)");
	}
	else if (Montage->BlendModeOut == EMontageBlendMode::Inertialization)
	{
		Output += TEXT(" (Inertialization)");
	}
	Output += TEXT("\n");

	// Blend out trigger time
	Output += FString::Printf(TEXT("  BlendOutTriggerTime: %.3fs\n"), Montage->BlendOutTriggerTime);

	return Output;
}

FString ClaireonAnimHelpers::FormatModifiers(const UAnimSequence* AnimSeq, bool bFullDetail)
{
	if (!AnimSeq)
	{
		return FString();
	}

	FString Output;
	Output += TEXT("=== Modifiers ===\n");

	const UAnimationModifiersAssetUserData* ModUserData = const_cast<UAnimSequence*>(AnimSeq)->GetAssetUserData<UAnimationModifiersAssetUserData>();
	const TArray<UAnimationModifier*>& Modifiers = ModUserData ? ModUserData->GetAnimationModifierInstances() : TArray<UAnimationModifier*>();

	if (Modifiers.Num() == 0)
	{
		Output += TEXT("  (none)\n");
		return Output;
	}

	for (int32 i = 0; i < Modifiers.Num(); ++i)
	{
		const UAnimationModifier* Modifier = Modifiers[i];
		if (!Modifier)
		{
			Output += FString::Printf(TEXT("  [%d] (null)\n"), i);
			continue;
		}

		const FString ClassName = Modifier->GetClass()->GetName();
		Output += FString::Printf(TEXT("  [%d] %s\n"), i, *ClassName);

		if (bFullDetail)
		{
			FString Props = FormatNotifySubObjectProperties(Modifier, TEXT("      "));
			if (!Props.IsEmpty())
			{
				Output += Props;
			}
		}
	}

	return Output;
}

FString ClaireonAnimHelpers::FormatMetadata(const UAnimationAsset* Asset, bool bFullDetail)
{
	if (!Asset)
	{
		return FString();
	}

	FString Output;
	Output += TEXT("=== Metadata ===\n");

	const TArray<UAnimMetaData*>& MetaDataArray = Asset->GetMetaData();

	if (MetaDataArray.Num() == 0)
	{
		Output += TEXT("  (none)\n");
		return Output;
	}

	for (int32 i = 0; i < MetaDataArray.Num(); ++i)
	{
		const UAnimMetaData* Meta = MetaDataArray[i];
		if (!Meta)
		{
			Output += FString::Printf(TEXT("  [%d] (null)\n"), i);
			continue;
		}

		const FString ClassName = Meta->GetClass()->GetName();
		Output += FString::Printf(TEXT("  [%d] %s\n"), i, *ClassName);

		if (bFullDetail)
		{
			FString Props = FormatNotifySubObjectProperties(Meta, TEXT("      "));
			if (!Props.IsEmpty())
			{
				Output += Props;
			}
		}
	}

	return Output;
}

FString ClaireonAnimHelpers::FormatAnimStructure(const UAnimSequenceBase* Anim, const FString& AssetType, bool bFullDetail, const FString& FocusSection)
{
	if (!Anim)
	{
		return TEXT("(null animation asset)");
	}

	FString Output;

	// Header
	Output += FString::Printf(TEXT("=== %s: %s ===\n"), *AssetType, *Anim->GetName());
	Output += FString::Printf(TEXT("Path: %s\n"), *Anim->GetPathName());

	// Basic metrics
	const float Length = Anim->GetPlayLength();
	int32 NumFrames = 0;
	double FrameRate = 0.0;
	if (const IAnimationDataModel* DataModel = Anim->GetDataModel())
	{
		NumFrames = DataModel->GetNumberOfFrames();
		FrameRate = DataModel->GetFrameRate().AsDecimal();
	}
	Output += FString::Printf(TEXT("Length: %.3fs | Frames: %d @ %.0ffps | Rate: %.1fx\n"),
		Length, NumFrames, FrameRate, Anim->RateScale);

	// Root motion and skeleton
	bool bHasRootMotion = false;
	bool bIsAdditive = false;
	if (const UAnimSequence* AnimSeq = Cast<UAnimSequence>(Anim))
	{
		bHasRootMotion = AnimSeq->bEnableRootMotion;
		bIsAdditive = (AnimSeq->AdditiveAnimType != AAT_None);
	}
	Output += FString::Printf(TEXT("Root Motion: %s | Additive: %s\n"),
		bHasRootMotion ? TEXT("Yes") : TEXT("No"),
		bIsAdditive ? TEXT("Yes") : TEXT("No"));

	const FString SkeletonPath = Anim->GetSkeleton() ? Anim->GetSkeleton()->GetPathName() : TEXT("None");
	Output += FString::Printf(TEXT("Skeleton: %s\n"), *SkeletonPath);
	Output += TEXT("\n");

	// If a focus section is specified, show only that section
	const bool bShowAll = FocusSection.IsEmpty();

	if (bShowAll || FocusSection == TEXT("notifies"))
	{
		Output += FormatNotifies(Anim, bFullDetail);
		Output += TEXT("\n");
	}

	if (bShowAll || FocusSection == TEXT("curves"))
	{
		Output += FormatCurves(Anim, bFullDetail);
		Output += TEXT("\n");
	}

	// AnimSequence-specific sections
	if (const UAnimSequence* AnimSeq = Cast<UAnimSequence>(Anim))
	{
		if (bShowAll || FocusSection == TEXT("sync_markers"))
		{
			Output += FormatSyncMarkers(AnimSeq);
			Output += TEXT("\n");
		}

		if (bShowAll || FocusSection == TEXT("modifiers"))
		{
			Output += FormatModifiers(AnimSeq, bFullDetail);
			Output += TEXT("\n");
		}
	}

	// Montage-specific sections
	if (const UAnimMontage* Montage = Cast<UAnimMontage>(Anim))
	{
		if (bShowAll || FocusSection == TEXT("sections"))
		{
			Output += FormatMontageSections(Montage);
			Output += TEXT("\n");
		}

		if (bShowAll || FocusSection == TEXT("slots"))
		{
			Output += FormatMontageSlots(Montage);
			Output += TEXT("\n");
		}

		if (bShowAll || FocusSection == TEXT("blend") || FocusSection == TEXT("blend_settings"))
		{
			Output += FormatMontageBlendSettings(Montage);
			Output += TEXT("\n");
		}
	}

	// Metadata (available on all AnimSequenceBase types)
	if (bShowAll || FocusSection == TEXT("metadata"))
	{
		Output += FormatMetadata(Anim, bFullDetail);
		Output += TEXT("\n");
	}

	// Properties focus: dump all UPROPERTYs on the asset itself
	if (FocusSection == TEXT("properties"))
	{
		Output += TEXT("=== Asset Properties ===\n");
		FString Props = FormatNotifySubObjectProperties(Anim, TEXT("  "));
		if (!Props.IsEmpty())
		{
			Output += Props;
		}
		else
		{
			Output += TEXT("  (no non-default properties)\n");
		}
		Output += TEXT("\n");
	}

	return Output;
}

// ============================================================================
// Notify Helpers
// ============================================================================

UClass* ClaireonAnimHelpers::ResolveNotifyClass(const FString& ClassName, bool bIsState, FString& OutError)
{
	UClass* BaseClass = bIsState ? UAnimNotifyState::StaticClass() : UAnimNotify::StaticClass();

	// Try the core resolver first
	ClaireonNameResolver::FNameResolveResult NameResult;
	UClass* FoundClass = ClaireonNameResolver::ResolveClassName(ClassName, BaseClass, NameResult);
	if (FoundClass)
	{
		return FoundClass;
	}

	// Search asset registry as a fallback for Blueprint notify classes
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprintGeneratedClass::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	for (const FAssetData& Asset : AssetList)
	{
		if (Asset.AssetName.ToString().Contains(ClassName, ESearchCase::IgnoreCase))
		{
			UClass* BPClass = Cast<UClass>(Asset.GetAsset());
			if (BPClass && BPClass->IsChildOf(BaseClass))
			{
				return BPClass;
			}
		}
	}

	OutError = FString::Printf(TEXT("Could not resolve %s class: %s. Use the full class name (e.g., AnimNotify_PlaySound) or a short name matching a loaded class."),
		bIsState ? TEXT("notify state") : TEXT("notify"), *ClassName);
	return nullptr;
}

namespace ClaireonAnimHelpersInternal
{

/**
 * Ensure enough notify tracks exist on the animation to accommodate the given track index.
 * Creates new tracks with auto-numbered names as needed.
 */
void EnsureNotifyTrackExists(UAnimSequenceBase* Anim, int32 TrackIndex)
{
	while (Anim->AnimNotifyTracks.Num() <= TrackIndex)
	{
		FAnimNotifyTrack NewTrack;
		NewTrack.TrackName = FName(*FString::Printf(TEXT("%d"), Anim->AnimNotifyTracks.Num()));
		Anim->AnimNotifyTracks.Add(NewTrack);
	}
}

}  // namespace ClaireonAnimHelpersInternal

int32 ClaireonAnimHelpers::AddSkeletonNotify(UAnimSequenceBase* Anim, const FString& NotifyName, float Time, int32 TrackIndex, FString& OutError)
{
	if (!Anim)
	{
		OutError = TEXT("Animation is null");
		return -1;
	}

	USkeleton* Skeleton = Anim->GetSkeleton();
	if (!Skeleton)
	{
		OutError = TEXT("Animation has no skeleton");
		return -1;
	}

	// Validate time is within animation range
	if (Time < 0.0f || Time > Anim->GetPlayLength())
	{
		OutError = FString::Printf(TEXT("Time %.3f is out of range [0, %.3f]"), Time, Anim->GetPlayLength());
		return -1;
	}

	// Ensure track exists
	ClaireonAnimHelpersInternal::EnsureNotifyTrackExists(Anim,TrackIndex);

	// Register name on skeleton if needed
	if (!Skeleton->AnimationNotifies.Contains(FName(*NotifyName)))
	{
		Skeleton->AddNewAnimationNotify(FName(*NotifyName));
		Skeleton->MarkPackageDirty();
	}

	// Create FAnimNotifyEvent with NO sub-object (skeleton-style)
	FAnimNotifyEvent NewEvent;
	NewEvent.NotifyName = FName(*NotifyName);
	NewEvent.Notify = nullptr;
	NewEvent.NotifyStateClass = nullptr;
	NewEvent.SetTime(Time);
	NewEvent.TrackIndex = TrackIndex;
	NewEvent.Guid = FGuid::NewGuid();

	int32 Index = Anim->Notifies.Add(NewEvent);
	Anim->RefreshCacheData();
	Anim->MarkPackageDirty();
	return Index;
}

int32 ClaireonAnimHelpers::AddClassNotify(UAnimSequenceBase* Anim, UClass* NotifyClass, float Time, float Duration, int32 TrackIndex, FString& OutError)
{
	if (!Anim)
	{
		OutError = TEXT("Animation is null");
		return -1;
	}

	if (!NotifyClass)
	{
		OutError = TEXT("Notify class is null");
		return -1;
	}

	// Validate time
	if (Time < 0.0f || Time > Anim->GetPlayLength())
	{
		OutError = FString::Printf(TEXT("Time %.3f is out of range [0, %.3f]"), Time, Anim->GetPlayLength());
		return -1;
	}

	// Ensure track exists
	ClaireonAnimHelpersInternal::EnsureNotifyTrackExists(Anim,TrackIndex);

	const bool bIsState = NotifyClass->IsChildOf(UAnimNotifyState::StaticClass());
	const bool bIsNotify = NotifyClass->IsChildOf(UAnimNotify::StaticClass());

	if (!bIsState && !bIsNotify)
	{
		OutError = FString::Printf(TEXT("Class %s is not a subclass of UAnimNotify or UAnimNotifyState"), *NotifyClass->GetName());
		return -1;
	}

	FAnimNotifyEvent NewEvent;
	NewEvent.TrackIndex = TrackIndex;
	NewEvent.Guid = FGuid::NewGuid();
	NewEvent.SetTime(Time);

	if (bIsState)
	{
		// Create state notify sub-object
		UAnimNotifyState* NewState = NewObject<UAnimNotifyState>(Anim, NotifyClass, NAME_None, RF_Transactional);
		NewEvent.NotifyStateClass = NewState;
		NewEvent.Notify = nullptr;
		NewEvent.NotifyName = FName(*NotifyClass->GetName());
		NewEvent.SetDuration((Duration > 0.0f) ? Duration : 0.5f);
		NewEvent.EndTriggerTimeOffset = GetTriggerTimeOffsetForType(EAnimEventTriggerOffsets::OffsetBefore);
	}
	else
	{
		// Create instant notify sub-object
		UAnimNotify* NewNotify = NewObject<UAnimNotify>(Anim, NotifyClass, NAME_None, RF_Transactional);
		NewEvent.Notify = NewNotify;
		NewEvent.NotifyStateClass = nullptr;
		NewEvent.NotifyName = FName(*NotifyClass->GetName());
	}

	int32 Index = Anim->Notifies.Add(NewEvent);
	Anim->RefreshCacheData();
	Anim->MarkPackageDirty();
	return Index;
}

bool ClaireonAnimHelpers::RemoveNotify(UAnimSequenceBase* Anim, int32 NotifyIndex, FString& OutError)
{
	if (!Anim)
	{
		OutError = TEXT("Animation is null");
		return false;
	}

	if (NotifyIndex < 0 || NotifyIndex >= Anim->Notifies.Num())
	{
		OutError = FString::Printf(TEXT("Notify index %d out of range (asset has %d notifies)"), NotifyIndex, Anim->Notifies.Num());
		return false;
	}

	Anim->Notifies.RemoveAt(NotifyIndex);
	Anim->RefreshCacheData();
	Anim->MarkPackageDirty();
	return true;
}

bool ClaireonAnimHelpers::MoveNotify(UAnimSequenceBase* Anim, int32 NotifyIndex, float NewTime, float NewDuration, int32 NewTrackIndex, FString& OutError)
{
	if (!Anim)
	{
		OutError = TEXT("Animation is null");
		return false;
	}

	if (NotifyIndex < 0 || NotifyIndex >= Anim->Notifies.Num())
	{
		OutError = FString::Printf(TEXT("Notify index %d out of range (asset has %d notifies)"), NotifyIndex, Anim->Notifies.Num());
		return false;
	}

	FAnimNotifyEvent& Event = Anim->Notifies[NotifyIndex];

	// Update time if requested (pass < 0 to leave unchanged)
	if (NewTime >= 0.0f)
	{
		if (NewTime > Anim->GetPlayLength())
		{
			OutError = FString::Printf(TEXT("New time %.3f exceeds animation length %.3f"), NewTime, Anim->GetPlayLength());
			return false;
		}
		Event.SetTime(NewTime);
	}

	// Update duration if requested (only valid for state notifies)
	if (NewDuration >= 0.0f)
	{
		if (Event.NotifyStateClass != nullptr)
		{
			Event.SetDuration(NewDuration);
		}
		else
		{
			UE_LOG(LogClaireon, Warning, TEXT("MoveNotify: Duration ignored for non-state notify at index %d"), NotifyIndex);
		}
	}

	// Update track if requested
	if (NewTrackIndex >= 0)
	{
		ClaireonAnimHelpersInternal::EnsureNotifyTrackExists(Anim,NewTrackIndex);
		Event.TrackIndex = NewTrackIndex;
	}

	Anim->RefreshCacheData();
	Anim->MarkPackageDirty();
	return true;
}

bool ClaireonAnimHelpers::SetNotifyProperty(UAnimSequenceBase* Anim, int32 NotifyIndex, const FString& PropertyName, const FString& PropertyValue, FString& OutError)
{
	if (!Anim)
	{
		OutError = TEXT("Animation is null");
		return false;
	}

	if (NotifyIndex < 0 || NotifyIndex >= Anim->Notifies.Num())
	{
		OutError = FString::Printf(TEXT("Notify index %d out of range (asset has %d notifies)"), NotifyIndex, Anim->Notifies.Num());
		return false;
	}

	const FAnimNotifyEvent& Event = Anim->Notifies[NotifyIndex];

	// Get the sub-object (notify or state)
	UObject* SubObject = Event.Notify ? static_cast<UObject*>(Event.Notify) : static_cast<UObject*>(Event.NotifyStateClass);
	if (!SubObject)
	{
		OutError = FString::Printf(TEXT("Notify at index %d is a skeleton notify (no sub-object). Cannot set properties on skeleton notifies."), NotifyIndex);
		return false;
	}

	const bool bResult = ClaireonPropertyUtils::WritePropertyByPath(SubObject, PropertyName, PropertyValue, OutError);
	if (bResult)
	{
		Anim->MarkPackageDirty();
	}
	return bResult;
}

FString ClaireonAnimHelpers::GetNotifyProperty(const UAnimSequenceBase* Anim, int32 NotifyIndex, const FString& PropertyName, FString& OutError)
{
	if (!Anim)
	{
		OutError = TEXT("Animation is null");
		return FString();
	}

	if (NotifyIndex < 0 || NotifyIndex >= Anim->Notifies.Num())
	{
		OutError = FString::Printf(TEXT("Notify index %d out of range (asset has %d notifies)"), NotifyIndex, Anim->Notifies.Num());
		return FString();
	}

	const FAnimNotifyEvent& Event = Anim->Notifies[NotifyIndex];

	// Get the sub-object (notify or state)
	const UObject* SubObject = Event.Notify ? static_cast<const UObject*>(Event.Notify) : static_cast<const UObject*>(Event.NotifyStateClass);
	if (!SubObject)
	{
		OutError = FString::Printf(TEXT("Notify at index %d is a skeleton notify (no sub-object). Cannot read properties on skeleton notifies."), NotifyIndex);
		return FString();
	}

	return ClaireonPropertyUtils::ReadPropertyByPath(const_cast<UObject*>(SubObject), PropertyName, OutError);
}

// ============================================================================
// Curve Helpers
// ============================================================================

bool ClaireonAnimHelpers::AddCurve(UAnimSequenceBase* Anim, const FString& CurveName, FString& OutError)
{
	if (!Anim)
	{
		OutError = TEXT("Animation is null");
		return false;
	}

	USkeleton* Skeleton = Anim->GetSkeleton();
	if (!Skeleton)
	{
		OutError = TEXT("Animation has no skeleton");
		return false;
	}

	IAnimationDataController& Controller = Anim->GetController();
	const FAnimationCurveIdentifier CurveId = UAnimationCurveIdentifierExtensions::GetCurveIdentifier(
		Skeleton, FName(*CurveName), ERawCurveTrackTypes::RCT_Float);

	if (!Controller.AddCurve(CurveId))
	{
		OutError = FString::Printf(TEXT("Failed to add curve '%s'. It may already exist."), *CurveName);
		return false;
	}

	Anim->MarkPackageDirty();
	return true;
}

bool ClaireonAnimHelpers::RemoveCurve(UAnimSequenceBase* Anim, const FString& CurveName, FString& OutError)
{
	if (!Anim)
	{
		OutError = TEXT("Animation is null");
		return false;
	}

	USkeleton* Skeleton = Anim->GetSkeleton();
	if (!Skeleton)
	{
		OutError = TEXT("Animation has no skeleton");
		return false;
	}

	IAnimationDataController& Controller = Anim->GetController();
	const FAnimationCurveIdentifier CurveId = UAnimationCurveIdentifierExtensions::GetCurveIdentifier(
		Skeleton, FName(*CurveName), ERawCurveTrackTypes::RCT_Float);

	if (!Controller.RemoveCurve(CurveId))
	{
		OutError = FString::Printf(TEXT("Failed to remove curve '%s'. It may not exist."), *CurveName);
		return false;
	}

	Anim->MarkPackageDirty();
	return true;
}

bool ClaireonAnimHelpers::AddCurveKey(UAnimSequenceBase* Anim, const FString& CurveName, const FRichCurveKey& Key, FString& OutError)
{
	if (!Anim)
	{
		OutError = TEXT("Animation is null");
		return false;
	}

	USkeleton* Skeleton = Anim->GetSkeleton();
	if (!Skeleton)
	{
		OutError = TEXT("Animation has no skeleton");
		return false;
	}

	// Validate time
	if (Key.Time < 0.0f || Key.Time > Anim->GetPlayLength())
	{
		OutError = FString::Printf(TEXT("Time %.3f is out of range [0, %.3f]"), Key.Time, Anim->GetPlayLength());
		return false;
	}

	IAnimationDataController& Controller = Anim->GetController();
	const FAnimationCurveIdentifier CurveId = UAnimationCurveIdentifierExtensions::GetCurveIdentifier(
		Skeleton, FName(*CurveName), ERawCurveTrackTypes::RCT_Float);

	if (!Controller.SetCurveKey(CurveId, Key))
	{
		OutError = FString::Printf(TEXT("Failed to add key at time %.3f to curve '%s'. Verify the curve exists."), Key.Time, *CurveName);
		return false;
	}

	Anim->MarkPackageDirty();
	return true;
}

bool ClaireonAnimHelpers::RemoveCurveKey(UAnimSequenceBase* Anim, const FString& CurveName, float Time, FString& OutError)
{
	if (!Anim)
	{
		OutError = TEXT("Animation is null");
		return false;
	}

	USkeleton* Skeleton = Anim->GetSkeleton();
	if (!Skeleton)
	{
		OutError = TEXT("Animation has no skeleton");
		return false;
	}

	IAnimationDataController& Controller = Anim->GetController();
	const FAnimationCurveIdentifier CurveId = UAnimationCurveIdentifierExtensions::GetCurveIdentifier(
		Skeleton, FName(*CurveName), ERawCurveTrackTypes::RCT_Float);

	// Snap to nearest key within tolerance to handle floating point imprecision
	// (e.g., frame 8 at 30fps = 0.26666... which displays as 0.267)
	const IAnimationDataModel* Model = Anim->GetDataModel();
	if (Model)
	{
		const FRichCurve* RichCurve = Model->FindRichCurve(CurveId);
		if (RichCurve)
		{
			constexpr float KeyTimeTolerance = 0.002f; // ~half a frame at 240fps
			float BestTime = Time;
			float BestDelta = KeyTimeTolerance + 1.0f;
			for (const FRichCurveKey& Key : RichCurve->Keys)
			{
				const float Delta = FMath::Abs(Key.Time - Time);
				if (Delta < BestDelta)
				{
					BestDelta = Delta;
					BestTime = Key.Time;
				}
			}
			if (BestDelta <= KeyTimeTolerance)
			{
				Time = BestTime;
			}
		}
	}

	if (!Controller.RemoveCurveKey(CurveId, Time))
	{
		OutError = FString::Printf(TEXT("Failed to remove key at time %.3f from curve '%s'. Verify the curve and key exist."), Time, *CurveName);
		return false;
	}

	Anim->MarkPackageDirty();
	return true;
}

const FRichCurveKey* ClaireonAnimHelpers::FindCurveKey(const UAnimSequenceBase* Anim, const FString& CurveName, float Time, float& OutSnappedTime, FString& OutError)
{
	if (!Anim)
	{
		OutError = TEXT("Animation is null");
		return nullptr;
	}

	const USkeleton* Skeleton = Anim->GetSkeleton();
	if (!Skeleton)
	{
		OutError = TEXT("Animation has no skeleton");
		return nullptr;
	}

	const FAnimationCurveIdentifier CurveId = UAnimationCurveIdentifierExtensions::GetCurveIdentifier(
		const_cast<USkeleton*>(Skeleton), FName(*CurveName), ERawCurveTrackTypes::RCT_Float);

	const IAnimationDataModel* Model = Anim->GetDataModel();
	if (!Model)
	{
		OutError = TEXT("Animation has no data model");
		return nullptr;
	}

	const FRichCurve* RichCurve = Model->FindRichCurve(CurveId);
	if (!RichCurve)
	{
		OutError = FString::Printf(TEXT("Curve '%s' not found"), *CurveName);
		return nullptr;
	}

	constexpr float KeyTimeTolerance = 0.002f;
	const FRichCurveKey* BestKey = nullptr;
	float BestDelta = KeyTimeTolerance + 1.0f;
	for (const FRichCurveKey& Key : RichCurve->Keys)
	{
		const float Delta = FMath::Abs(Key.Time - Time);
		if (Delta < BestDelta)
		{
			BestDelta = Delta;
			BestKey = &Key;
		}
	}

	if (!BestKey || BestDelta > KeyTimeTolerance)
	{
		OutError = FString::Printf(TEXT("No key found near time %.3f on curve '%s'"), Time, *CurveName);
		return nullptr;
	}

	OutSnappedTime = BestKey->Time;
	return BestKey;
}

namespace ClaireonAnimHelpersInternal
{

ERichCurveInterpMode ParseInterpMode(const FString& Value, bool& bSuccess)
{
	bSuccess = true;
	FString Lower = Value.ToLower();
	if (Lower == TEXT("linear"))   return RCIM_Linear;
	if (Lower == TEXT("cubic"))    return RCIM_Cubic;
	if (Lower == TEXT("constant")) return RCIM_Constant;
	if (Lower == TEXT("none"))     return RCIM_None;
	bSuccess = false;
	return RCIM_Linear;
}

ERichCurveTangentMode ParseTangentMode(const FString& Value, bool& bSuccess)
{
	bSuccess = true;
	FString Lower = Value.ToLower();
	if (Lower == TEXT("auto"))  return RCTM_Auto;
	if (Lower == TEXT("user"))  return RCTM_User;
	if (Lower == TEXT("break")) return RCTM_Break;
	if (Lower == TEXT("none"))  return RCTM_None;
	bSuccess = false;
	return RCTM_Auto;
}

ERichCurveTangentWeightMode ParseTangentWeightMode(const FString& Value, bool& bSuccess)
{
	bSuccess = true;
	FString Lower = Value.ToLower();
	if (Lower == TEXT("none"))   return RCTWM_WeightedNone;
	if (Lower == TEXT("arrive")) return RCTWM_WeightedArrive;
	if (Lower == TEXT("leave"))  return RCTWM_WeightedLeave;
	if (Lower == TEXT("both"))   return RCTWM_WeightedBoth;
	bSuccess = false;
	return RCTWM_WeightedNone;
}

}  // namespace ClaireonAnimHelpersInternal

bool ClaireonAnimHelpers::SetCurveKeyProperty(UAnimSequenceBase* Anim, const FString& CurveName, float Time, const FString& PropertyName, const FString& Value, FString& OutError)
{
	if (!Anim)
	{
		OutError = TEXT("Animation is null");
		return false;
	}

	// Find the key (with tolerance snapping)
	float SnappedTime = Time;
	const FRichCurveKey* FoundKey = FindCurveKey(Anim, CurveName, Time, SnappedTime, OutError);
	if (!FoundKey)
	{
		return false;
	}

	// Make a mutable copy to modify
	FRichCurveKey ModifiedKey = *FoundKey;

	FString PropLower = PropertyName.ToLower();
	bool bSuccess = true;

	if (PropLower == TEXT("interp_mode"))
	{
		ModifiedKey.InterpMode = ClaireonAnimHelpersInternal::ParseInterpMode(Value, bSuccess);
		if (!bSuccess) { OutError = FString::Printf(TEXT("Invalid interp_mode '%s'. Expected: linear, cubic, constant, none"), *Value); return false; }
	}
	else if (PropLower == TEXT("tangent_mode"))
	{
		ModifiedKey.TangentMode = ClaireonAnimHelpersInternal::ParseTangentMode(Value, bSuccess);
		if (!bSuccess) { OutError = FString::Printf(TEXT("Invalid tangent_mode '%s'. Expected: auto, user, break, none"), *Value); return false; }
	}
	else if (PropLower == TEXT("tangent_weight_mode"))
	{
		ModifiedKey.TangentWeightMode = ClaireonAnimHelpersInternal::ParseTangentWeightMode(Value, bSuccess);
		if (!bSuccess) { OutError = FString::Printf(TEXT("Invalid tangent_weight_mode '%s'. Expected: none, arrive, leave, both"), *Value); return false; }
	}
	else if (PropLower == TEXT("arrive_tangent"))
	{
		ModifiedKey.ArriveTangent = FCString::Atof(*Value);
	}
	else if (PropLower == TEXT("leave_tangent"))
	{
		ModifiedKey.LeaveTangent = FCString::Atof(*Value);
	}
	else if (PropLower == TEXT("arrive_tangent_weight"))
	{
		ModifiedKey.ArriveTangentWeight = FCString::Atof(*Value);
	}
	else if (PropLower == TEXT("leave_tangent_weight"))
	{
		ModifiedKey.LeaveTangentWeight = FCString::Atof(*Value);
	}
	else
	{
		OutError = FString::Printf(TEXT("Unknown curve key property '%s'. Supported: interp_mode, tangent_mode, tangent_weight_mode, arrive_tangent, leave_tangent, arrive_tangent_weight, leave_tangent_weight"), *PropertyName);
		return false;
	}

	// Apply the modified key via the controller
	USkeleton* Skeleton = Anim->GetSkeleton();
	IAnimationDataController& Controller = Anim->GetController();
	const FAnimationCurveIdentifier CurveId = UAnimationCurveIdentifierExtensions::GetCurveIdentifier(
		Skeleton, FName(*CurveName), ERawCurveTrackTypes::RCT_Float);

	if (!Controller.SetCurveKey(CurveId, ModifiedKey))
	{
		OutError = FString::Printf(TEXT("Failed to update key at time %.3f on curve '%s'"), SnappedTime, *CurveName);
		return false;
	}

	Anim->MarkPackageDirty();
	return true;
}

// ============================================================================
// Montage Section Helpers
// ============================================================================

bool ClaireonAnimHelpers::AddMontageSection(UAnimMontage* Montage, const FString& SectionName, float StartTime, FString& OutError)
{
	if (!Montage)
	{
		OutError = TEXT("Montage is null");
		return false;
	}

	// Check for duplicate section name
	for (const FCompositeSection& Existing : Montage->CompositeSections)
	{
		if (Existing.SectionName == FName(*SectionName))
		{
			OutError = FString::Printf(TEXT("Section '%s' already exists in montage"), *SectionName);
			return false;
		}
	}

	// Validate time
	if (StartTime < 0.0f || StartTime > Montage->GetPlayLength())
	{
		OutError = FString::Printf(TEXT("Start time %.3f is out of range [0, %.3f]"), StartTime, Montage->GetPlayLength());
		return false;
	}

	int32 NewIndex = Montage->AddAnimCompositeSection(FName(*SectionName), StartTime);
	if (NewIndex == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("Failed to add section '%s' — name may already exist"), *SectionName);
		return false;
	}

	Montage->MarkPackageDirty();
	return true;
}

bool ClaireonAnimHelpers::RemoveMontageSection(UAnimMontage* Montage, const FString& SectionName, FString& OutError)
{
	if (!Montage)
	{
		OutError = TEXT("Montage is null");
		return false;
	}

	const FName SectionFName(*SectionName);
	int32 RemoveIndex = INDEX_NONE;

	for (int32 i = 0; i < Montage->CompositeSections.Num(); ++i)
	{
		if (Montage->CompositeSections[i].SectionName == SectionFName)
		{
			RemoveIndex = i;
			break;
		}
	}

	if (RemoveIndex == INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("Section '%s' not found in montage"), *SectionName);
		return false;
	}

	// Clear any links pointing to the removed section
	for (FCompositeSection& Section : Montage->CompositeSections)
	{
		if (Section.NextSectionName == SectionFName)
		{
			Section.NextSectionName = NAME_None;
		}
	}

	Montage->DeleteAnimCompositeSection(RemoveIndex);
	Montage->MarkPackageDirty();
	return true;
}

bool ClaireonAnimHelpers::SetMontageSectionLink(UAnimMontage* Montage, const FString& SectionName, const FString& NextSectionName, FString& OutError)
{
	if (!Montage)
	{
		OutError = TEXT("Montage is null");
		return false;
	}

	const FName SectionFName(*SectionName);
	FCompositeSection* FoundSection = nullptr;

	for (FCompositeSection& Section : Montage->CompositeSections)
	{
		if (Section.SectionName == SectionFName)
		{
			FoundSection = &Section;
			break;
		}
	}

	if (!FoundSection)
	{
		OutError = FString::Printf(TEXT("Section '%s' not found in montage"), *SectionName);
		return false;
	}

	// Validate the next section exists (unless it is empty/None, meaning "end")
	if (!NextSectionName.IsEmpty() && NextSectionName != TEXT("None"))
	{
		const FName NextFName(*NextSectionName);
		bool bFound = false;
		for (const FCompositeSection& Section : Montage->CompositeSections)
		{
			if (Section.SectionName == NextFName)
			{
				bFound = true;
				break;
			}
		}

		if (!bFound)
		{
			OutError = FString::Printf(TEXT("Next section '%s' not found in montage. Available sections: "), *NextSectionName);
			for (const FCompositeSection& Section : Montage->CompositeSections)
			{
				OutError += Section.SectionName.ToString() + TEXT(" ");
			}
			return false;
		}

		FoundSection->NextSectionName = NextFName;
	}
	else
	{
		FoundSection->NextSectionName = NAME_None;
	}

	Montage->MarkPackageDirty();
	return true;
}
