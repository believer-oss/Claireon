// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool that lists actors in the currently loaded map.
 * Supports filtering by class name and label wildcard pattern.
 */
class ClaireonTool_ListActors : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual bool RequiresEditorWorld() const override { return true; }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
