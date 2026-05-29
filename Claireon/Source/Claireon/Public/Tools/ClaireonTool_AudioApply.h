// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool for level-scoped audio application operations (e.g. placing
 * ambient sound actors, audio volumes, or pushing sound mix state into the
 * current editor world). RequiresNoPIE=true AND RequiresEditorWorld=true.
 */
class FClaireonTool_AudioApply : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual bool RequiresEditorWorld() const override { return true; }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
