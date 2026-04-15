// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool: claireon.animgraph_get_state_machine
 * Inspect state machine topology: entry state, states, transitions with blend settings, conduits.
 */
class CLAIREON_API ClaireonTool_AnimGraphGetStateMachine : public IClaireonTool
{
public:
	FString GetName() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
