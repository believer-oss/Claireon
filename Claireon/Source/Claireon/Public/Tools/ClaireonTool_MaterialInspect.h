// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool for reading the structure of a UMaterial asset.
 * Displays shading model, blend mode, expressions, parameters, and attribute
 * connections.
 *
 * Stateless -- no session required. Allowed during PIE.
 */
class ClaireonTool_MaterialInspect : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
