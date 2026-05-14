// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonWidgetAnimationHandlers.h"

#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

#include "Tools/ClaireonSequenceHelpers.h"

// ============================================================================
// #0000 widget-animation Apply handlers -- free functions used by the per-op
// ClaireonWidgetBPTool_* tools (stages 004-011) and by FClaireonSpecApplicator_WidgetBP
// (stage 015). Single definition per TU.
// ============================================================================

UWidgetAnimation* FindWidgetAnimationByName(UWidgetBlueprint* WBP, const FString& AnimationName)
{
	if (!WBP || AnimationName.IsEmpty())
	{
		return nullptr;
	}
	const FName Target(*AnimationName);
	for (UWidgetAnimation* Anim : WBP->Animations)
	{
		if (Anim && Anim->GetFName() == Target)
		{
			return Anim;
		}
	}
	return nullptr;
}

bool ApplyCreateAnimation(UWidgetBlueprint* WBP, const FString& AnimationName, float Duration,
	const FString& DisplayLabel, UWidgetAnimation*& OutAnim, FString& OutError)
{
	OutAnim = nullptr;
	OutError.Reset();
	if (!WBP)
	{
		OutError = TEXT("widget blueprint is null");
		return false;
	}
	if (AnimationName.IsEmpty())
	{
		OutError = TEXT("animation_name is required");
		return false;
	}
	if (FindWidgetAnimationByName(WBP, AnimationName) != nullptr)
	{
		OutError = FString::Printf(TEXT("animation '%s' already exists on %s"), *AnimationName, *WBP->GetName());
		return false;
	}
	const float ClampedDuration = FMath::Max(Duration, 0.01f);

	// Engine pattern mirrors AnimationTabSummoner.cpp:258-268 commit path.
	UWidgetAnimation* NewAnim = NewObject<UWidgetAnimation>(WBP, UWidgetAnimation::StaticClass(), NAME_None, RF_Transactional);
	if (!NewAnim)
	{
		OutError = TEXT("NewObject<UWidgetAnimation> returned null");
		return false;
	}
	const FString Label = DisplayLabel.IsEmpty() ? AnimationName : DisplayLabel;
	NewAnim->SetDisplayLabel(Label);
	NewAnim->Rename(*AnimationName, WBP, REN_DontCreateRedirectors);

	UMovieScene* MS = NewObject<UMovieScene>(NewAnim, FName(*AnimationName), RF_Transactional);
	NewAnim->MovieScene = MS;
	MS->SetDisplayRate(FFrameRate(20, 1));
	const FFrameRate TickResolution = MS->GetTickResolution();
	const FFrameNumber InFrame = FFrameNumber(0);
	const FFrameNumber OutFrame = (ClampedDuration * TickResolution).FloorToFrame();
	MS->SetPlaybackRange(TRange<FFrameNumber>(InFrame, OutFrame + FFrameNumber(1)));
#if WITH_EDITORONLY_DATA
	FMovieSceneEditorData& EditorData = MS->GetEditorData();
	EditorData.WorkStart = 0.0;
	EditorData.WorkEnd = ClampedDuration;
#endif

	WBP->Modify();
	WBP->Animations.Add(NewAnim);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);

	OutAnim = NewAnim;
	return true;
}

bool ApplyDeleteAnimation(UWidgetBlueprint* WBP, const FString& AnimationName, FString& OutError)
{
	OutError.Reset();
	if (!WBP)
	{
		OutError = TEXT("widget blueprint is null");
		return false;
	}
	UWidgetAnimation* Anim = FindWidgetAnimationByName(WBP, AnimationName);
	if (!Anim)
	{
		OutError = FString::Printf(TEXT("animation '%s' not found on %s"), *AnimationName, *WBP->GetName());
		return false;
	}
	WBP->Modify();
	// Engine pattern from AnimationTabSummoner::OnDeleteAnimation: reparent to
	// transient package so the WBP no longer owns the asset. Avoids future name
	// collisions; GC sweep reclaims it.
	Anim->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
	WBP->Animations.Remove(Anim);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	return true;
}

bool ApplyRenameAnimation(UWidgetBlueprint* WBP, const FString& OldName, const FString& NewName, FString& OutError)
{
	OutError.Reset();
	if (!WBP)
	{
		OutError = TEXT("widget blueprint is null");
		return false;
	}
	if (NewName.IsEmpty())
	{
		OutError = TEXT("new_name is required");
		return false;
	}
	UWidgetAnimation* Anim = FindWidgetAnimationByName(WBP, OldName);
	if (!Anim)
	{
		OutError = FString::Printf(TEXT("animation '%s' not found on %s"), *OldName, *WBP->GetName());
		return false;
	}
	if (FindWidgetAnimationByName(WBP, NewName) != nullptr)
	{
		OutError = FString::Printf(TEXT("animation '%s' already exists on %s"), *NewName, *WBP->GetName());
		return false;
	}
	const FName OldFName = Anim->GetFName();
	const FName NewFName(*NewName);

	WBP->Modify();
	Anim->Modify();
	if (UMovieScene* MS = Anim->GetMovieScene())
	{
		MS->Modify();
		MS->Rename(*NewName, nullptr, REN_DontCreateRedirectors);
	}
	Anim->SetDisplayLabel(NewName);
	Anim->Rename(*NewName, nullptr, REN_DontCreateRedirectors);

	// Widget animations are exposed as BP variables -- fix up references.
	FBlueprintEditorUtils::ReplaceVariableReferences(WBP, OldFName, NewFName);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	return true;
}

bool ApplyAddAnimationBinding(UWidgetAnimation* Anim, UWidget* Widget, const FString& SlotWidgetName,
	FGuid& OutGuid, FString& OutError)
{
	OutError.Reset();
	OutGuid.Invalidate();
	if (!Anim)
	{
		OutError = TEXT("animation is null");
		return false;
	}
	if (!Widget)
	{
		OutError = TEXT("widget is null");
		return false;
	}
	UMovieScene* MS = Anim->GetMovieScene();
	if (!MS)
	{
		OutError = TEXT("animation has no MovieScene");
		return false;
	}
	const FGuid NewGuid = MS->AddPossessable(Widget->GetName(), Widget->GetClass());
	if (!NewGuid.IsValid())
	{
		OutError = TEXT("AddPossessable failed");
		return false;
	}

	FWidgetAnimationBinding Binding;
	Binding.WidgetName = Widget->GetFName();
	Binding.SlotWidgetName = SlotWidgetName.IsEmpty() ? NAME_None : FName(*SlotWidgetName);
	Binding.AnimationGuid = NewGuid;
	Binding.bIsRootWidget = false;
	Anim->AnimationBindings.Add(Binding);

	OutGuid = NewGuid;
	return true;
}

bool ApplyAddAnimationTrack(UWidgetAnimation* Anim, const FGuid& BindingGuid, const FString& TrackType,
	const FString& PropertyName, UMovieSceneTrack*& OutTrack, FString& OutError)
{
	OutError.Reset();
	OutTrack = nullptr;
	if (!Anim)
	{
		OutError = TEXT("animation is null");
		return false;
	}
	UMovieScene* MS = Anim->GetMovieScene();
	if (!MS)
	{
		OutError = TEXT("animation has no MovieScene");
		return false;
	}
	const FString ResolvedType = TrackType.IsEmpty() ? FString(TEXT("float")) : TrackType;
	UClass* TrackClass = FClaireonSequenceHelpers::ResolveTrackClass(ResolvedType);
	if (!TrackClass)
	{
		OutError = FString::Printf(
			TEXT("unknown track type '%s'; known: float, double, bool, visibility, transform, 2d_transform, margin, widget_material, color, event, audio"),
			*ResolvedType);
		return false;
	}
	UMovieSceneTrack* Track = MS->AddTrack(TrackClass, BindingGuid);
	if (!Track)
	{
		OutError = FString::Printf(TEXT("track type '%s' not supported on widget animations"), *ResolvedType);
		return false;
	}
	if (UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track))
	{
		if (!PropertyName.IsEmpty())
		{
			PropertyTrack->SetPropertyNameAndPath(FName(*PropertyName), PropertyName);
		}
	}
	if (UMovieSceneSection* Section = Track->CreateNewSection())
	{
		Section->SetRange(MS->GetPlaybackRange());
		Track->AddSection(*Section);
	}
	OutTrack = Track;
	return true;
}
