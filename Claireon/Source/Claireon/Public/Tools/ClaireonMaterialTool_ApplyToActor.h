// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool: apply a UMaterial or UMaterialInstanceConstant to a UMeshComponent on
 * a live actor in the editor world. Targeting is by actor name + component name.
 *
 * Stateless -- no session required. Allowed during PIE (actors are editor objects).
 */
class ClaireonMaterialTool_ApplyToActor : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual EClaireonToolSessionMode GetSessionMode() const override { return EClaireonToolSessionMode::RequiresSession; }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
