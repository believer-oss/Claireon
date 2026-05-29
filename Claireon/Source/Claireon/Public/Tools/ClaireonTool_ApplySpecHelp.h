// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Meta tool that returns per-tool spec entry shapes for the sixteen apply_spec
 * supporting tools (after bp namespace collapse):
 *   attenuation, behaviortree, blackboard, bp, concurrency, eqs,
 *   level_sequence, material, metasound, niagara, pcg, soundclass,
 *   soundcue, soundmix, statetree, widgetbp.
 * Stateless read-only; no parameters required.
 *
 * Stage 001 stub: returns "not yet implemented" until Stage 003 lands.
 * If Stage 003 has since shipped, remove this note and verify the implementation.
 */
class ClaireonTool_ApplySpecHelp : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
