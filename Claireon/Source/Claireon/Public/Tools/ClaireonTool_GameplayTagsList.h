// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool that enumerates registered gameplay tags via UGameplayTagsManager.
 * Supports an optional case-sensitive prefix filter and an include_source toggle.
 */
class ClaireonTool_GameplayTagsList : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
