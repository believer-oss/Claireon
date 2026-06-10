// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/FClaireonSpecApplicatorBase.h"

class UMaterialInstanceConstant;

/**
 * apply_spec applicator for UMaterialInstanceConstant assets.
 *
 * Spec schema:
 * - parent? : string  (new parent UMaterialInterface asset path)
 * - parameters[] : id, name, type ("scalar"|"vector"|"texture"|"static_switch"|"static_component_mask"), value, override (default true)
 * - clear_overrides[] : name, type
 * - save_at_end? : bool (default false)
 *
 * Pass 1: Apply parent change (structural).
 * Pass 2: Apply parameter overrides and clears.
 * SaveAsset honors save_at_end.
 */
class CLAIREON_API FClaireonSpecApplicator_MaterialInstance : public FClaireonSpecApplicatorBase
{
protected:
	virtual bool ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors) override;
	virtual bool OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError) override;
	virtual bool ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool CompileAsset(const FString& SessionId, FString& OutError) override;
	virtual bool SaveAsset(const FString& SessionId, FString& OutError) override;
	virtual void CloseSession(const FString& SessionId) override;
	virtual FString GetToolName() const override { return TEXT("MaterialInstance"); }

private:
	/** Local reference to the MIC being edited. */
	TWeakObjectPtr<UMaterialInstanceConstant> Instance;

	/** Stashed in OpenOrCreateAsset so SaveAsset can read save_at_end. */
	TSharedPtr<FJsonObject> CachedSpec;
};
