// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/FClaireonDeltaApplicatorBase.h"
#include "UObject/WeakObjectPtr.h"

class UBehaviorTree;
class UBehaviorTreeGraph;
class UBehaviorTreeGraphNode;

/**
 * BehaviorTree apply_delta applicator. Full four-phase support.
 *
 * Phase 1 (disconnect) entries: {parent_id, child_id} -- detach child from parent.
 * Phase 2 (remove) entries: node id strings OR {id|name} objects.
 * Phase 3 (create) entries: {id, class, parent_id?|parent_local_id?, properties?}.
 * Phase 4 (connect) entries: {parent_id, child_id, child_index?}.
 *
 * Session model: M5 fail-on-missing (the BT asset must already exist).
 */
class CLAIREON_API FClaireonDeltaApplicator_BehaviorTree : public FClaireonDeltaApplicatorBase
{
protected:
	virtual FString GetFamilyName() const override { return TEXT("behaviortree"); }

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
	/** Resolve a node reference: id_map -> GUID -> node name. Returns nullptr and sets error on ambiguous. */
	UBehaviorTreeGraphNode* ResolveNodeRef(UBehaviorTreeGraph* Graph, const FString& Ref, FString& OutError) const;

	/** M2 safety net: nodes created during this call. Removed on failure before transaction cancel. */
	TArray<TWeakObjectPtr<UBehaviorTreeGraphNode>> CreatedNodesThisCall;
	TWeakObjectPtr<UBehaviorTree> CachedBT;
	TWeakObjectPtr<UBehaviorTreeGraph> CachedGraph;
};
