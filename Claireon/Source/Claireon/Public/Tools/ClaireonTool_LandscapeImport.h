// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool for importing heightmaps and weightmaps from external files (R16, PNG, RAW).
 * Stateless -- no session required. RequiresNoPIE because it modifies landscape data.
 */
class ClaireonTool_LandscapeImport : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
