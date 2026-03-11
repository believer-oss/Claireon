// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool for reading the structure of a Behavior Tree asset.
 * Displays the full node hierarchy: composites, tasks, decorators, services,
 * and blackboard key references. Read-only inspection tool.
 */
class ClaireonTool_BehaviorTreeInspect : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetCategory() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
