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
 * - nodes[].graph: optional per-node target graph name (default: the Blueprint's EventGraph)
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

	/**
	 * Spec-id -> the graph that node was actually created in.
	 *
	 * Pass 2 resolves node references with FindNodeByGuid, which searches a single
	 * graph and has no cross-graph fallback. Without this, any node placed in a
	 * non-default graph by nodes[].graph would be invisible to Pass 2, and its
	 * connections / pin_defaults would be dropped with only a warning.
	 */
	TMap<FString, TWeakObjectPtr<UEdGraph>> IdToGraph;
};
