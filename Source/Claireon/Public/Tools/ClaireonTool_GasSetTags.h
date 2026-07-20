// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * gas_set_tags -- add or remove loose gameplay tags on a live actor's ASC, to
 * simulate a trigger/state condition (e.g. add a state tag so an aspect's
 * HitModifier condition fires). Engine GAS API only.
 */
class ClaireonTool_GasSetTags : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual EClaireonToolSessionMode GetSessionMode() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
