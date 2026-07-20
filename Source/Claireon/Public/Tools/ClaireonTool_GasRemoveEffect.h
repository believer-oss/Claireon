// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * gas_remove_effect -- remove an applied GameplayEffect from a live actor's ASC,
 * addressed either by effect_handle_id (from gas_runtime_inspect / gas_apply_effect) or
 * by effect class (removes all matching). Defaults to the server world. Engine
 * GAS API only.
 */
class ClaireonTool_GasRemoveEffect : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual EClaireonToolSessionMode GetSessionMode() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
