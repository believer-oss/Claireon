// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/FClaireonSpecApplicatorBase.h"

class UEnvQuery;

/**
 * apply_spec applicator for EQS (Environment Query System) assets.
 *
 * Spec schema (stack tool pattern):
 * - options[] with id, generator {type, properties}, tests[] {id, type, properties}
 *
 * Pass 1: Create all options and set generators
 * Pass 2: Add tests, set properties on generators and tests
 */
class CLAIREON_API FClaireonSpecApplicator_EQS : public FClaireonSpecApplicatorBase
{
protected:
	virtual bool ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors) override;
	virtual bool OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError) override;
	virtual bool ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool CompileAsset(const FString& SessionId, FString& OutError) override;
	virtual bool SaveAsset(const FString& SessionId, FString& OutError) override;
	virtual void CloseSession(const FString& SessionId) override;
	virtual FString GetToolName() const override { return TEXT("EQS"); }

private:
	/** Local reference to the EQS query being edited. */
	TWeakObjectPtr<UEnvQuery> Query;
};
