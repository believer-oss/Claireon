// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/** Reload served prompts and resources from Content/MCP, retaining prior entries on parse failure. */
class ClaireonTool_ReloadMCPContent : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual FString GetPatterns() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	/** Changes server content registries only; no asset, world, or transaction mutation. */
	virtual EClaireonToolSessionMode GetSessionMode() const override { return EClaireonToolSessionMode::ReadOnly; }
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
