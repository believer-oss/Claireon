// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool that performs State Tree structural comparison: states, tasks,
 * conditions, transitions, evaluators, global tasks, and bindings.
 * Supports loading State Trees from the current editor state or from git revisions.
 */
class ClaireonTool_StateTreeDiff : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual FString GetCategory() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
