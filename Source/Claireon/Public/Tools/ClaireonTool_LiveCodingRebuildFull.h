// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Live-coding "header changed" recovery in one shot. Spawns a detached PowerShell
 * helper that kills the editor process, runs Invoke-EditorBuild.ps1, then relaunches
 * the editor via Invoke-EditorBuildAndLaunch.ps1 -UseMCPProxy -SkipBuild. The proxy
 * preserves the Claireon connection across the restart.
 *
 * NOTE: this tool returns immediately after scheduling the helper; the editor process
 * itself goes away before the response is delivered, so callers should not chain
 * subsequent MCP calls until they have observed the editor's ready flag.
 */
class CLAIREON_API ClaireonTool_LiveCodingRebuildFull : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	virtual bool RequiresNoPIE() const override { return true; }
};
