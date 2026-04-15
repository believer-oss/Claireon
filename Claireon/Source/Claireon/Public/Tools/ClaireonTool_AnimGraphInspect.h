// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool: claireon.animgraph_inspect
 * Blueprint-level overview of an animation blueprint.
 * Returns class settings, interfaces, variables, functions, graph list, and warnings.
 */
class CLAIREON_API ClaireonTool_AnimGraphInspect : public IClaireonTool
{
public:
	FString GetName() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
