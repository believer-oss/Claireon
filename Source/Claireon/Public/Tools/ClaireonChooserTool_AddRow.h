// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

class CLAIREON_API ClaireonTool_ChooserAddRow : public IClaireonTool
{
public:
	FString GetCategory() const override;
	FString GetOperation() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	bool RequiresNoPIE() const override { return true; }
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
