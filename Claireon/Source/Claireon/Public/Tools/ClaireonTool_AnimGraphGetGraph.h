// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool: animbp_get_graph
 * Graph-level node inspection of a specific graph within an animation blueprint.
 * Returns all nodes with types, pins, pose connections, property bindings, and fast path status.
 */
class CLAIREON_API ClaireonTool_AnimGraphGetGraph : public IClaireonTool
{
public:
	FString GetCategory() const override;
	FString GetOperation() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
