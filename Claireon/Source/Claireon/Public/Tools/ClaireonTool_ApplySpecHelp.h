// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Meta tool that returns per-tool spec entry shapes for the eight apply_spec
 * supporting tools (behaviortree, blackboard, blueprint_graph, eqs, niagara,
 * pcg, statetree, widgetbp). Stateless read-only; no parameters required.
 *
 * Stage 001 stub: returns "not yet implemented" until Stage 003 lands.
 */
class ClaireonTool_ApplySpecHelp : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
