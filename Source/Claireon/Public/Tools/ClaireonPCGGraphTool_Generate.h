// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Run PCG generation and wait for completion or timeout via GenerateAndWait.
 * The wait advances the PCG scheduler without pumping the world tick.
 */
class CLAIREON_API ClaireonPCGGraphTool_Generate : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	/** Hard ceiling on components touched by one call, mirroring pcg_refresh. */
	static constexpr int32 MaxComponentsPerCall = 32;
};
