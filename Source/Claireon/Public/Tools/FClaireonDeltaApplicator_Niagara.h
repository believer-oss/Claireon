// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/FClaireonDeltaApplicatorBase.h"
#include "UObject/WeakObjectPtr.h"

class UNiagaraSystem;

/**
 * Niagara apply_delta applicator. D3: parameters-only. Phase 1 and Phase 4
 * are not supported.
 *
 * Per AR3, phase-3 entries use the generic `nodes[]` key (not `parameters[]`);
 * each entry has the niagara-specific shape:
 *   {id, name, type, value?, kind?}
 *
 *   - kind: defaults to "parameter"; emitter/module/renderer return the L4
 *     stub-string error and short-circuit.
 *   - name: bare ("Color") or namespaced ("User.Color").
 *   - type: Float, Vector, Color, LinearColor, Bool, Int.
 *
 * Phase 2 (`remove_nodes`) accepts strings or {id|name} objects, where the
 * value may be a local-id (resolved via the IdMap minted by phase 3) OR a
 * literal `User.<Name>` (mirrors AR6).
 *
 * All parameter writes route through ClaireonNiagaraHelpers::AddOrUpdateUserParameter
 * and ClaireonNiagaraHelpers::RemoveUserParameter (AR7 / H2 shared-impl invariant).
 *
 * See Docs/llm/apply-delta-all-families/06_NIAGARA.md.
 */
class CLAIREON_API FClaireonDeltaApplicator_Niagara : public FClaireonDeltaApplicatorBase
{
protected:
	virtual FString GetFamilyName() const override { return TEXT("niagara"); }
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
	TWeakObjectPtr<UNiagaraSystem> CachedSystem;

	/** M2 safety net: parameter names created during phase 3 of THIS call, removed on rollback. */
	TArray<FString> CreatedParameterNamesThisCall;
};
