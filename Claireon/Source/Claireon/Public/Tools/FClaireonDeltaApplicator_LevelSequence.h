// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/FClaireonDeltaApplicatorBase.h"
#include "UObject/WeakObjectPtr.h"

class ULevelSequence;

/**
 * LevelSequence apply_delta applicator. D1: remove + create only (no disconnect/connect).
 *
 * Phase 2 (remove) entries: composite-id refs {binding_label, track_name?, row_index?, start_frame?}.
 *   M4: entries are sorted DEEPEST-FIRST (more-specific keys first) before applying.
 * Phase 3 (create) entries: {kind: "binding", id, label, object_class}.
 *
 * Session model: M5 fail-on-missing.
 */
class CLAIREON_API FClaireonDeltaApplicator_LevelSequence : public FClaireonDeltaApplicatorBase
{
protected:
	virtual FString GetFamilyName() const override { return TEXT("level_sequence"); }
	virtual bool SupportsPhase1Disconnect() const override { return false; }
	virtual bool SupportsPhase4Connect() const override { return false; }

	virtual bool ValidateArgs(const TSharedPtr<FJsonObject>& Args, TArray<FString>& OutErrors) override;
	virtual bool OpenOrReuseSession(const TSharedPtr<FJsonObject>& Args, FString& OutSessionId, FString& OutError) override;
	virtual bool ApplyPhase1_Disconnect(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries) override { (void)SessionId; (void)Entries; return true; }
	virtual bool ApplyPhase2_Remove(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries) override;
	virtual bool ApplyPhase3_Create(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries) override;
	virtual bool ApplyPhase4_Connect(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries) override { (void)SessionId; (void)Entries; return true; }
	virtual void FinalizeSession(const FString& SessionId) override;
	virtual void CloseSessionIfOwned(const FString& SessionId) override;
	virtual void Phase3CleanupOnFailure(const FString& SessionId) override;

private:
	TWeakObjectPtr<ULevelSequence> CachedSequence;

	// Track created bindings (by GUID string) for rollback (M2).
	TArray<FGuid> CreatedBindingsThisCall;
};
