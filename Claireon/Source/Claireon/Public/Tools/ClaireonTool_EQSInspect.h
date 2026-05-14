// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool for reading an Environment Query System (EQS) asset.
 * Displays options (generator + tests), context references, scoring functions,
 * and filter settings. Critical for identifying blackboard-based vs custom-context contexts.
 */
class ClaireonTool_EQSInspect : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
