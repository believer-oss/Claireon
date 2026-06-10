// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/FClaireonDeltaApplicatorBase.h"
#include "UObject/WeakObjectPtr.h"

class UWidgetBlueprint;
class UWidget;

/**
 * WidgetBP apply_delta applicator. D5: phase 4 connect = reparent (widgets
 * always have exactly one parent, so "disconnect without reparent" has no
 * meaning).
 *
 * Phase 1 (disconnect): NOT SUPPORTED.
 * Phase 2 (remove)    : widget ids (strings) or {id|name} objects (widget names).
 * Phase 3 (create)    : {id, type, parent_id, parent_slot_id?, properties?, slot_properties?}.
 * Phase 4 (connect = reparent): {widget_id, new_parent_id, new_parent_slot_id?}.
 *   Delegates to ClaireonWidgetHelpers::MoveWidget (H4 -- shared with the discrete
 *   move_widget tool, so typed-slot handling is consistent).
 *
 * See Docs/llm/apply-delta-all-families/09_WIDGETBP.md.
 */
class CLAIREON_API FClaireonDeltaApplicator_WidgetBP : public FClaireonDeltaApplicatorBase
{
protected:
	virtual FString GetFamilyName() const override { return TEXT("widgetbp"); }
	virtual bool SupportsPhase1Disconnect() const override { return false; }

	virtual bool ValidateArgs(const TSharedPtr<FJsonObject>& Args, TArray<FString>& OutErrors) override;
	virtual bool OpenOrReuseSession(const TSharedPtr<FJsonObject>& Args, FString& OutSessionId, FString& OutError) override;
	virtual bool ApplyPhase1_Disconnect(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries) override { (void)SessionId; (void)Entries; return true; }
	virtual bool ApplyPhase2_Remove(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries) override;
	virtual bool ApplyPhase3_Create(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries) override;
	virtual bool ApplyPhase4_Connect(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries) override;
	virtual void FinalizeSession(const FString& SessionId) override;
	virtual void CloseSessionIfOwned(const FString& SessionId) override;
	virtual void Phase3CleanupOnFailure(const FString& SessionId) override;

private:
	TWeakObjectPtr<UWidgetBlueprint> CachedWBP;

	/** M2 safety net: widgets created during phase 3 of THIS call, removed on rollback. */
	TArray<TWeakObjectPtr<UWidget>> CreatedWidgetsThisCall;
};
