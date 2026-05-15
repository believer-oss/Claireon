// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool for reading the structure of a Level Sequence asset (F1).
 * Displays bindings, tracks, sections, and keyframes.
 * Stateless -- no session required.
 */
class ClaireonTool_SequenceInspect : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual FString GetFullDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual bool RequiresNoPIE() const override { return false; }
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
