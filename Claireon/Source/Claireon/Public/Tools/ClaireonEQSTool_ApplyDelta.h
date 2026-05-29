// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * EQS apply_delta MCP tool. Thin wrapper around FClaireonDeltaApplicator_EQS.
 * Remove + Create only; no disconnect/connect phases.
 */
class CLAIREON_API FClaireonEQSTool_ApplyDelta : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
	virtual EClaireonToolSessionMode GetSessionMode() const override { return EClaireonToolSessionMode::RequiresSession; }
	virtual bool RequiresNoPIE() const override { return true; }
};
