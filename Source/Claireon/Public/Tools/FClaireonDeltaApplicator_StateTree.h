// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/FClaireonDeltaApplicatorBase.h"
#include "UObject/WeakObjectPtr.h"
#include "Misc/Guid.h"

class UStateTree;
class UStateTreeEditorData;

/**
 * StateTree apply_delta applicator. Full four-phase support.
 *
 * Phase 1 (disconnect): array of transition ids (strings) -- removes the named transitions.
 * Phase 2 (remove): {kind: "state"|"evaluator"|"global_task", id}; "transition" is rejected.
 * Phase 3 (create): {kind, id, ...kind-specific-fields}.
 * Phase 4 (connect): transitions {id?, from_state, to_state, trigger?}.
 *
 * AR4: top-level transitions[] and phase-4 connections[] are unioned; ties resolve
 * in favor of top-level (transitions[]). Dedup by id first, then by from_state -> to_state pair.
 *
 * Session model: M5 fail-on-missing.
 * See Docs/llm/apply-delta-all-families/08_STATETREE.md.
 */
class CLAIREON_API FClaireonDeltaApplicator_StateTree : public FClaireonDeltaApplicatorBase
{
protected:
	virtual FString GetFamilyName() const override { return TEXT("statetree"); }

	virtual bool ValidateArgs(const TSharedPtr<FJsonObject>& Args, TArray<FString>& OutErrors) override;
	virtual bool OpenOrReuseSession(const TSharedPtr<FJsonObject>& Args, FString& OutSessionId, FString& OutError) override;
	virtual bool ApplyPhase1_Disconnect(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries) override;
	virtual bool ApplyPhase2_Remove(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries) override;
	virtual bool ApplyPhase3_Create(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries) override;
	virtual bool ApplyPhase4_Connect(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries) override;
	virtual void FinalizeSession(const FString& SessionId) override;
	virtual void CloseSessionIfOwned(const FString& SessionId) override;
	virtual void Phase3CleanupOnFailure(const FString& SessionId) override;

private:
	/**
	 * AR4 helper: returns the union of top-level transitions[] (from Args) and
	 * phase-4 connections[] (Entries), deduped per M3 (dedupe by id, then by
	 * from_state -> to_state pair; top-level wins on tie).
	 */
	TArray<TSharedPtr<FJsonValue>> UnionAndDedupeTransitions(
		const TSharedPtr<FJsonObject>& Args,
		const TArray<TSharedPtr<FJsonValue>>& Phase4Entries) const;

	TWeakObjectPtr<UStateTree> CachedStateTree;
	TWeakObjectPtr<UStateTreeEditorData> CachedEditorData;

	// M2 safety net: created entity GUIDs, per kind, for rollback cleanup.
	TArray<FGuid> CreatedStateIdsThisCall;
	TArray<FGuid> CreatedTransitionIdsThisCall;
	TArray<FGuid> CreatedEvaluatorIdsThisCall;
	TArray<FGuid> CreatedGlobalTaskIdsThisCall;
};
