// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool: replace a //[BP] tagged node region with user-supplied C++ code. Hash-guarded;
 * rejects edits if the file has been modified outside the translator. Marks the node
 * 'implemented' in the session.
 */
class ClaireonBlueprintTranslateTool_Implement : public IClaireonTool
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
