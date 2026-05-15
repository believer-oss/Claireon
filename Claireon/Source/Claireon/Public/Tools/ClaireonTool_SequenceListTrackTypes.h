// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool for listing the track types available for Level Sequence authoring (F3).
 * Stateless -- no session required.
 */
class ClaireonTool_SequenceListTrackTypes : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual FString GetFullDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual bool RequiresNoPIE() const override { return false; }
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
