// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool: animbp_get_node
 * Deep inspection of a single node by GUID within an animation graph.
 * Returns ALL properties, pins, bindings, fast path analysis, bound events, and sub-graph references.
 */
class CLAIREON_API ClaireonTool_AnimGraphGetNode : public IClaireonTool
{
public:
	FString GetCategory() const override;
	FString GetOperation() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
