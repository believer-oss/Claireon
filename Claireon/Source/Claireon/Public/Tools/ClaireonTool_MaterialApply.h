// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool for applying a UMaterial or UMaterialInstanceConstant to a level
 * actor or to a blueprint's UMeshComponent SCS node.
 *
 * Stateless -- no session required. Allowed during PIE (actors are editor
 * objects; blueprints compile via FKismetEditorUtilities).
 */
class ClaireonTool_MaterialApply : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual EClaireonToolSessionMode GetSessionMode() const override { return EClaireonToolSessionMode::RequiresSession; }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
