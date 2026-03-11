// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool for reading the full structure of a State Tree asset as structured text.
 * Displays states, tasks, conditions, transitions, evaluators, considerations, and property bindings.
 */
class ClaireonTool_StateTreeInspect : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetCategory() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
