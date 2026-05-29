// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool: apply a UMaterial or UMaterialInstanceConstant to a UMeshComponent SCS
 * node on a blueprint. Targeting is by blueprint path + component name. Compiles
 * the blueprint after the change and reports child blueprints that may need attention.
 *
 * Stateless -- no session required. Allowed during PIE (blueprints compile via FKismetEditorUtilities).
 */
class ClaireonMaterialTool_ApplyToBlueprint : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual EClaireonToolSessionMode GetSessionMode() const override { return EClaireonToolSessionMode::RequiresSession; }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
