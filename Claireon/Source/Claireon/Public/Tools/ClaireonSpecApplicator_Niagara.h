// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/FClaireonSpecApplicatorBase.h"

class UNiagaraSystem;

/**
 * apply_spec applicator for Niagara System assets.
 *
 * Spec schema (stack tool pattern):
 * - emitters[] with id, template, name, enabled, modules{}, renderers[]
 * - parameters[] with id, name, type, value
 *
 * Pass 1: Create emitters, add parameters
 * Pass 2: Add modules, renderers, set module inputs and renderer properties, set parameter values
 */
class CLAIREON_API FClaireonSpecApplicator_Niagara : public FClaireonSpecApplicatorBase
{
protected:
	virtual bool ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors) override;
	virtual bool OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError) override;
	virtual bool ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) override;
	virtual bool CompileAsset(const FString& SessionId, FString& OutError) override;
	virtual bool SaveAsset(const FString& SessionId, FString& OutError) override;
	virtual void CloseSession(const FString& SessionId) override;
	virtual FString GetToolName() const override { return TEXT("Niagara"); }

private:
	/** Local reference to the Niagara System being edited. */
	TWeakObjectPtr<UNiagaraSystem> System;
};
