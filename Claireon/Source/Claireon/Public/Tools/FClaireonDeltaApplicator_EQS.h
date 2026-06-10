// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/FClaireonDeltaApplicatorBase.h"
#include "UObject/WeakObjectPtr.h"

class UEnvQuery;
class UEnvQueryOption;

/**
 * EQS apply_delta applicator. Remove + Create only; no disconnect/connect phases.
 *
 * Phase 2 entries: option ids (strings or {id|name} objects).
 * Phase 3 entries: kind-tagged union -- {kind: "option", id, generator, tests?} or
 *                  {kind: "test", id, option_id, class}.
 *
 * Session model: M5 fail-on-missing.
 * See Docs/llm/apply-delta-all-families/03_EQS.md.
 */
class CLAIREON_API FClaireonDeltaApplicator_EQS : public FClaireonDeltaApplicatorBase
{
protected:
	virtual FString GetFamilyName() const override { return TEXT("eqs"); }
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
	TWeakObjectPtr<UEnvQuery> CachedQuery;
	TArray<TWeakObjectPtr<UEnvQueryOption>> CreatedOptionsThisCall;
};
