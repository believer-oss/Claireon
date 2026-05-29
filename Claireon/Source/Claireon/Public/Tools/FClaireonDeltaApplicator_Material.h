// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/FClaireonDeltaApplicatorBase.h"
#include "UObject/WeakObjectPtr.h"

class UMaterial;
class UMaterialExpression;

/**
 * Material apply_delta applicator. Full four-phase support.
 *
 * Phase 1 (disconnect): {from_expr_id, from_output, to_expr_id, to_input} -- selectively
 *   break one wire.
 * Phase 2 (remove)    : expression ids (strings) or {id|name} objects.
 * Phase 3 (create)    : {id, type, x?, y?, parameter_name?, properties?} -- expression
 *   nodes (type is expression class path or short name).
 * Phase 4 (connect)   : two sub-shapes -- expression-to-expression
 *   {from, from_output, to, to_input}, or expression-to-material-attribute
 *   {from, from_output, attribute}. Dispatch on presence of `attribute` field.
 *
 * D4 binding: UMaterial graph-shaped; UMaterialInstanceConstant is excluded
 * (handled separately by material_instance_* tools).
 *
 */
class CLAIREON_API FClaireonDeltaApplicator_Material : public FClaireonDeltaApplicatorBase
{
protected:
	virtual FString GetFamilyName() const override { return TEXT("material"); }

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
	TWeakObjectPtr<UMaterial> CachedMaterial;

	/** M2 safety net: expressions created during phase 3 of THIS call, removed on rollback. */
	TArray<TWeakObjectPtr<UMaterialExpression>> CreatedExpressionsThisCall;
};
