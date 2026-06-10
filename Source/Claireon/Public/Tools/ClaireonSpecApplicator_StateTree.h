// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/FClaireonSpecApplicatorBase.h"

class UStateTree;
class UStateTreeEditorData;

/**
 * apply_spec applicator for State Tree assets.
 *
 * Spec schema (tree tool pattern):
 * - states[] with id, name, parent, type, selection_behavior, tasks, enter_conditions, transitions
 * - evaluators[] with id, type, properties
 * - global_tasks[] with id, type, properties
 *
 * Pass 1: Create all states (parent-first order), evaluators, global tasks
 * Pass 2: Add tasks, conditions, transitions, set properties
 */
class CLAIREON_API FClaireonSpecApplicator_StateTree : public FClaireonSpecApplicatorBase
{
protected:
	virtual bool ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors) override;
	virtual bool OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError) override;
	virtual bool ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool CompileAsset(const FString& SessionId, FString& OutError) override;
	virtual bool SaveAsset(const FString& SessionId, FString& OutError) override;
	virtual void CloseSession(const FString& SessionId) override;
	virtual FString GetToolName() const override { return TEXT("StateTree"); }

private:
	/** Local reference to the StateTree being edited. */
	TWeakObjectPtr<UStateTree> StateTree;

	/** Cached reference to the editor data. */
	TWeakObjectPtr<UStateTreeEditorData> EditorData;
};
