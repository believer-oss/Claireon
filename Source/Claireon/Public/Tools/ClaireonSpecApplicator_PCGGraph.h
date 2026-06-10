// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/FClaireonSpecApplicatorBase.h"

class UPCGGraph;

/**
 * apply_spec applicator for PCG Graph assets.
 *
 * Spec schema (graph tool pattern):
 * - nodes[] with id, type, properties
 * - connections[] with source_node, source_pin, target_node, target_pin
 *
 * Pass 1: Create all PCG nodes via ClaireonPCGGraphHelpers
 * Pass 2: Wire edge connections, set node properties
 */
class CLAIREON_API FClaireonSpecApplicator_PCGGraph : public FClaireonSpecApplicatorBase
{
protected:
	virtual bool ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors) override;
	virtual bool OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError) override;
	virtual bool ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool CompileAsset(const FString& SessionId, FString& OutError) override;
	virtual bool SaveAsset(const FString& SessionId, FString& OutError) override;
	virtual void CloseSession(const FString& SessionId) override;
	virtual FString GetToolName() const override { return TEXT("PCGGraph"); }

private:
	/** Local reference to the PCG Graph being edited. */
	TWeakObjectPtr<UPCGGraph> PCGGraph;
};
