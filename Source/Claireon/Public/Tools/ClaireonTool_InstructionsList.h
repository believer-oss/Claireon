// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * List instructions from the live server registries, including retained entries
 * whose files failed to parse during reload.
 */
class ClaireonTool_InstructionsList : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual FString GetFullDescription() const override;
	virtual FString GetExampleUsage() const override;
	virtual FString GetPatterns() const override;
	virtual TArray<FString> GetSearchKeywords() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	/** ReadOnly: touches no asset, world or transaction state, and unlike mcp_reload_content
	 *  it does not write the server's registries either. */
	virtual EClaireonToolSessionMode GetSessionMode() const override { return EClaireonToolSessionMode::ReadOnly; }
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
