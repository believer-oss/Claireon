// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/FClaireonSpecApplicatorBase.h"

class UBehaviorTree;
class UBehaviorTreeGraph;

/**
 * apply_spec applicator for Behavior Tree assets.
 *
 * Spec schema (tree tool pattern):
 * - nodes[] with id, type, parent, children, decorators, services, properties
 *
 * Pass 1: Create all nodes (composites, tasks) via ClaireonBehaviorTreeHelpers
 * Pass 2: Connect parent-child relationships, add decorators/services, set properties
 */
class CLAIREON_API FClaireonSpecApplicator_BehaviorTree : public FClaireonSpecApplicatorBase
{
protected:
	virtual bool ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors) override;
	virtual bool OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError) override;
	virtual bool ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool CompileAsset(const FString& SessionId, FString& OutError) override;
	virtual bool SaveAsset(const FString& SessionId, FString& OutError) override;
	virtual void CloseSession(const FString& SessionId) override;
	virtual FString GetToolName() const override { return TEXT("BehaviorTree"); }

private:
	/** Local reference to the BT being edited (not stored in tool's static ToolData map). */
	TWeakObjectPtr<UBehaviorTree> BehaviorTree;

	/** Cached reference to the graph. */
	TWeakObjectPtr<UBehaviorTreeGraph> BTGraph;
};
