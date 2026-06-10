// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Rename a UMaterial parameter expression's ParameterName (Scalar/Vector/Texture/
 * StaticSwitch/StaticComponentMask + RuntimeVirtualTexture). Walks expressions, finds
 * the one with matching old_name via HasAParameterName(), and rewrites the field.
 */
class CLAIREON_API ClaireonTool_MaterialRenameParameter : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	virtual bool RequiresNoPIE() const override { return true; }
};
