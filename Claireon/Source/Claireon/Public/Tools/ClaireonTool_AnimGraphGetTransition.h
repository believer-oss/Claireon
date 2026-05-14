// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool: animgraph_get_transition
 * Detailed inspection of a specific state machine transition.
 * Returns blend settings, crossfade mode/duration, condition graph, and custom blend graph.
 */
class CLAIREON_API ClaireonTool_AnimGraphGetTransition : public IClaireonTool
{
public:
	FString GetCategory() const override;
	FString GetOperation() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
