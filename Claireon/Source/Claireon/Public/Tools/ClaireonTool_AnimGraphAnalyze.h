// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool: animgraph_analyze
 * Analysis of an animation blueprint: fast path compliance, thread safety, and warning aggregation.
 */
class CLAIREON_API ClaireonTool_AnimGraphAnalyze : public IClaireonTool
{
public:
	FString GetCategory() const override;
	FString GetOperation() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
