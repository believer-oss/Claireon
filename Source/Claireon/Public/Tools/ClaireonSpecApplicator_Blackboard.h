// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/FClaireonSpecApplicatorBase.h"

class UBlackboardData;

/**
 * apply_spec applicator for Blackboard Data assets.
 *
 * Spec schema (flat list pattern):
 * - parent_blackboard: optional asset path
 * - keys[] with id, name, type
 *
 * Pass 1: Set parent if specified, create all blackboard keys
 * Pass 2: No-op (no relationships to wire)
 */
class CLAIREON_API FClaireonSpecApplicator_Blackboard : public FClaireonSpecApplicatorBase
{
protected:
	virtual bool ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors) override;
	virtual bool OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError) override;
	virtual bool ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool CompileAsset(const FString& SessionId, FString& OutError) override;
	virtual bool SaveAsset(const FString& SessionId, FString& OutError) override;
	virtual void CloseSession(const FString& SessionId) override;
	virtual FString GetToolName() const override { return TEXT("Blackboard"); }

private:
	/** Local reference to the Blackboard being edited. */
	TWeakObjectPtr<UBlackboardData> BlackboardData;
};
