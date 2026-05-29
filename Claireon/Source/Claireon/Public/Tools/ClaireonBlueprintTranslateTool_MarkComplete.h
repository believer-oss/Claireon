// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool: declare a translation session 'complete'. Walks every blueprint's node map,
 * checks for residual TODO markers, and updates the session status. Read/write on the
 * session JSON only; does not mutate source files.
 */
class ClaireonBlueprintTranslateTool_MarkComplete : public IClaireonTool
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
