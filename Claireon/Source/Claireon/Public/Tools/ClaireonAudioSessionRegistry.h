// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class UObject;
struct FMCPSessionClosedInfo;

/** Cohort-discriminator for session entries. Graph cohorts only - reflection cohorts have no sessions. */
enum class ESoundCohort : uint8
{
	SoundCue,
	MetaSound
};

/**
 * Per-session state shared by all per-op tools in a cohort. Replaces the bundled tool's
 * FAudioEditToolData; migrated 1:1 with an added MetaSoundBuilderHandle field for the
 * MetaSound cohort (M1 in AUDIO_DECOMPOSE_SESSION_REGISTRY.md).
 */
struct FAudioEditToolData
{
	TWeakObjectPtr<UObject> Asset;          // USoundCue or UMetaSoundSource
	FString AssetPath;
	FString LastOperationStatus;
	bool bSuppressOutput = false;

	/** Index into USoundCue::AllNodes of the currently focused node (SoundCue cohort only).
	 *  Used by add_node layout default (offset +300, 0 from focused node). INDEX_NONE means no focus. */
	int32 FocusedNodeIndex = INDEX_NONE;

	bool bDirty = false;

	/** MetaSound-only opaque builder pointer; cast inside ClaireonAudioSessionRegistry.cpp / per-op tools. */
	void* MetaSoundBuilderHandle = nullptr;

	bool IsValid() const { return Asset.IsValid(); }
};

namespace ClaireonAudioSessionRegistry
{
	/** Register the session-closed delegate exactly once. Called by every cohort _open. Idempotent. */
	void EnsureDelegateRegistered();

	/** Create a new session entry. Returns the new entry's pointer; never returns nullptr on success. */
	FAudioEditToolData* CreateSession(const FString& SessionId, ESoundCohort Cohort);

	/** Lookup by id and cohort. Returns nullptr if not found OR cohort mismatch. */
	FAudioEditToolData* FindSession(const FString& SessionId, ESoundCohort Cohort);

	/** Drop a session entry. Safe to call on already-released entries. */
	void ReleaseSession(const FString& SessionId, ESoundCohort Cohort);

	/** Internal: removes any session whose ToolName == "audio_edit" when the manager closes it. */
	void HandleSessionClosed(const FMCPSessionClosedInfo& Info);
}
