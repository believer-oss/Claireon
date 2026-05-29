// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Reload an asset from disk, discarding in-memory edits.
 * Stateless / non-session, but mutates editor state.
 */
class CLAIREON_API ClaireonTool_AssetReload : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	virtual bool RequiresNoPIE() const override { return true; }
};
