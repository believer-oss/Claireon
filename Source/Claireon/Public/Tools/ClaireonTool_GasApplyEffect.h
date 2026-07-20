// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * gas_apply_effect -- apply a GameplayEffect to a live actor's ASC (the
 * promoted "jank" aspect test: point it at an aspect's GE_*_C and watch the
 * mechanic take effect). Supports level, SetByCaller magnitudes, and a
 * duration override. Defaults to the server world. Engine GAS API only.
 */
class ClaireonTool_GasApplyEffect : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual EClaireonToolSessionMode GetSessionMode() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
