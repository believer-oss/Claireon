// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Misc/FrameNumber.h"

class ULevelSequence;
class UMovieScene;
class UMovieSceneTrack;
class UMovieSceneSection;
struct FSequenceEditToolData;

/**
 * Shared module-private helpers for claireon.level_sequence_* decomposed tools.
 * Lifted verbatim from the pre-decomposition ClaireonTool_SequenceEdit.cpp.
 */
namespace ClaireonLevelSequenceInternal
{
	void MarkMutated(ULevelSequence* Sequence);

	bool FindBindingByLabelOrGuid(
		UMovieScene* MovieScene,
		const FString& Label,
		const FString& GuidStr,
		int32& OutIndex,
		FGuid& OutGuid,
		FString& OutError);

	UMovieSceneTrack* ResolveFocusedTrack(
		UMovieScene* MovieScene,
		int32 FocusedBinding,
		int32 FocusedTrack,
		FString& OutError);

	UMovieSceneSection* ResolveFocusedSection(
		FSequenceEditToolData* Data,
		int32 SectionIndex,
		FString& OutError);

	ULevelSequence* CreateLevelSequenceAtPath(const FString& PackagePath, FString& OutError);
}
