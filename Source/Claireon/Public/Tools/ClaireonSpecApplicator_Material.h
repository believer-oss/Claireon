// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/FClaireonSpecApplicatorBase.h"

class UMaterial;

/**
 * apply_spec applicator for UMaterial assets.
 *
 * Spec schema:
 * - expressions[]: id, class, x?, y?, parameter_name?, properties?{}
 * - connections[]: from_id, from_output, to_id, to_input  (expression-to-expression)
 * - attribute_connections[]: from_id, from_output, attribute  (expression-to-material-output)
 * - parameter_defaults[]: id, parameter_name, parameter_type, value
 * - shading_model? : string
 * - blend_mode? : string
 * - compile_at_end? : bool (default true)
 * - save_at_end? : bool (default false)
 *
 * Pass 1: Create all expressions referenced in expressions[].
 * Pass 2: Apply connections, attribute connections, parameter defaults, shading/blend mode.
 * Compile + save in CompileAsset / SaveAsset.
 */
class CLAIREON_API FClaireonSpecApplicator_Material : public FClaireonSpecApplicatorBase
{
protected:
	virtual bool ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors) override;
	virtual bool OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError) override;
	virtual bool ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool CompileAsset(const FString& SessionId, FString& OutError) override;
	virtual bool SaveAsset(const FString& SessionId, FString& OutError) override;
	virtual void CloseSession(const FString& SessionId) override;
	virtual FString GetToolName() const override { return TEXT("Material"); }

private:
	/** Local reference to the Material being edited. */
	TWeakObjectPtr<UMaterial> Material;

	/** Stashed in OpenOrCreateAsset so later passes can read compile_at_end / save_at_end. */
	TSharedPtr<FJsonObject> CachedSpec;
};
