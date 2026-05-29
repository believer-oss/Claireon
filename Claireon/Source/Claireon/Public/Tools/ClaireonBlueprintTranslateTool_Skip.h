// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool: mark a //[BP] tagged node 'skipped' in the translation session, optionally
 * rewriting the in-file TODO marker to a SKIPPED note (with reason from 'code' field).
 */
class ClaireonBlueprintTranslateTool_Skip : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual EClaireonToolSessionMode GetSessionMode() const override { return EClaireonToolSessionMode::RequiresSession; }
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
