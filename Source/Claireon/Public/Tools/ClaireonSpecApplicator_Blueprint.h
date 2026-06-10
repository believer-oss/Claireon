// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/FClaireonSpecApplicatorBase.h"

class UBlueprint;
class UEdGraph;

/**
 * apply_spec applicator for Blueprint graph assets.
 *
 * Spec schema (graph tool pattern):
 * - graph: target graph name (default "EventGraph")
 * - nodes[] with id, type, function, position, pin_defaults
 * - connections[] with source_node, source_pin, target_node, target_pin
 * - variables[] with id, name, type, default_value, flags
 *
 * Pass 1: Create all nodes and variables
 * Pass 2: Wire pin connections, set pin default values
 */
class CLAIREON_API FClaireonSpecApplicator_Blueprint : public FClaireonSpecApplicatorBase
{
protected:
	virtual bool ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors) override;
	virtual bool OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError) override;
	virtual bool ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool CompileAsset(const FString& SessionId, FString& OutError) override;
	virtual bool SaveAsset(const FString& SessionId, FString& OutError) override;
	virtual void CloseSession(const FString& SessionId) override;
	virtual FString GetToolName() const override { return TEXT("Blueprint"); }

private:
	/** Local reference to the Blueprint being edited. */
	TWeakObjectPtr<UBlueprint> Blueprint;

	/** Cached reference to the active graph. */
	TWeakObjectPtr<UEdGraph> ActiveGraph;
};
