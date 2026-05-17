// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"

class UWidget;
class UWidgetAnimation;
class UWidgetBlueprint;
class UMovieSceneTrack;

/**
 * Widget-animation Apply-handler surface used by the per-op
 * ClaireonWidgetBPTool_* tools and by FClaireonSpecApplicator_WidgetBP. Definitions
 * live in ClaireonWidgetAnimationHandlers.cpp.
 */

namespace Claireon::WidgetAnimation
{

bool ApplyCreateAnimation(
	UWidgetBlueprint* WBP,
	const FString& AnimationName,
	float Duration,
	const FString& DisplayLabel,
	UWidgetAnimation*& OutAnim,
	FString& OutError);

bool ApplyDeleteAnimation(
	UWidgetBlueprint* WBP,
	const FString& AnimationName,
	FString& OutError);

bool ApplyRenameAnimation(
	UWidgetBlueprint* WBP,
	const FString& OldName,
	const FString& NewName,
	FString& OutError);

bool ApplyAddAnimationBinding(
	UWidgetAnimation* Anim,
	UWidget* Widget,
	const FString& SlotWidgetName,
	FGuid& OutGuid,
	FString& OutError);

bool ApplyAddAnimationTrack(
	UWidgetAnimation* Anim,
	const FGuid& BindingGuid,
	const FString& TrackType,
	const FString& PropertyName,
	UMovieSceneTrack*& OutTrack,
	FString& OutError);

/** Helper: find a UWidgetAnimation on the WBP by case-sensitive FName match. */
UWidgetAnimation* FindWidgetAnimationByName(UWidgetBlueprint* WBP, const FString& AnimationName);

}  // namespace Claireon::WidgetAnimation
