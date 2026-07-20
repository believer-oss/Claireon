// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * gas_activate_ability -- actually activate an already-granted ability on a live
 * actor's ASC (unlike pie_test_ability, which only dry-run checks
 * CanActivateAbility). Address it by spec_handle_id or ability_name. Engine GAS
 * API only.
 */
class ClaireonTool_GasActivateAbility : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual EClaireonToolSessionMode GetSessionMode() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
