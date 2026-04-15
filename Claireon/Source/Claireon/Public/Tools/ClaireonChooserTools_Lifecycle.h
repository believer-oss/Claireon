// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

class CLAIREON_API ClaireonTool_ChooserCreate : public IClaireonTool
{
public:
	FString GetName() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	bool RequiresNoPIE() const override { return true; }
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};

class CLAIREON_API ClaireonTool_ChooserDuplicate : public IClaireonTool
{
public:
	FString GetName() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	bool RequiresNoPIE() const override { return true; }
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
