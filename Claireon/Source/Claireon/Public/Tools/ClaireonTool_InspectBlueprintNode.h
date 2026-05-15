// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Stateless MCP tool that returns a single Blueprint node in full fidelity.
 * Shares ClaireonBlueprintNodeSerializer with the session-based
 * claireon.blueprint_edit_graph inspect_node op.
 *
 * For AnimGraph nodes, returns an error directing callers to
 * claireon.animgraph_get_node rather than emitting a degraded payload.
 */
class ClaireonTool_InspectBlueprintNode : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual FString GetFullDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
