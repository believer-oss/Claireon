// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/FClaireonDeltaApplicatorBase.h"
#include "UObject/WeakObjectPtr.h"

class UPCGGraph;
class UPCGNode;

/**
 * PCG apply_delta applicator. Full four-phase support.
 *
 * Phase 1/4 entries: {source_node, source_pin, target_node, target_pin} (pin names EXACT match).
 * Phase 2 entries: node id strings or {id|name} objects.
 * Phase 3 entries: {id, type, properties?}.
 *
 * Session model: M5 fail-on-missing.
 */
class CLAIREON_API FClaireonDeltaApplicator_PCGGraph : public FClaireonDeltaApplicatorBase
{
protected:
	virtual FString GetFamilyName() const override { return TEXT("pcg"); }

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
	/** Resolve a node ref via id_map -> identifier (index or name). Returns nullptr and sets error on failure. */
	UPCGNode* ResolveNodeRef(UPCGGraph* Graph, const FString& Ref, FString& OutError) const;

	TArray<TWeakObjectPtr<UPCGNode>> CreatedNodesThisCall;
	TWeakObjectPtr<UPCGGraph> CachedGraph;
};
