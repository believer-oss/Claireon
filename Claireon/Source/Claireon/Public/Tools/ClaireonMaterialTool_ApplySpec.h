// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Apply a declarative JSON spec to a UMaterial (expressions, connections,
 * attribute_connections, parameter_defaults, shading_model, blend_mode).
 *
 * Stateless (session-less): opens its own session if one is not provided.
 */
class CLAIREON_API ClaireonMaterialTool_ApplySpec : public IClaireonTool
{
public:
	FString GetOperation() const override;
	FString GetDescription() const override;
	bool RequiresNoPIE() const override { return true; }
	FString GetCategory() const override { return TEXT("material"); }
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
