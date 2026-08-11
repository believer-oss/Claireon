// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Set the PIE pause state explicitly.
 *
 * Exists because `console_execute(command='pause')` TOGGLES: issuing it twice returns to the
 * original state, and issuing it once from unknown state leaves you unsure which state you are
 * in. A paused world then reads as "nothing is happening", which is a genuinely misleading
 * diagnosis to draw. This sets rather than toggles, and reports the state it moved from.
 */
class ClaireonTool_PIESetPaused : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
