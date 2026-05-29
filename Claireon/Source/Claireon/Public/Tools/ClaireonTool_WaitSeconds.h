// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Wall-clock sleep that keeps the editor ticking. Python's
 * time.sleep is intercepted with a RuntimeWarning because it freezes the editor
 * tick; this tool yields to FTSTicker while waiting so deferred actions, async
 * loads, and the editor UI remain responsive.
 */
class ClaireonTool_WaitSeconds : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
