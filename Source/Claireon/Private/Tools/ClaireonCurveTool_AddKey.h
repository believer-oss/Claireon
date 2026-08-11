// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/** curve_* family tool. See ClaireonCurveTool_AddKey.cpp for behavior. */
class FClaireonCurveTool_AddKey : public IClaireonTool
{
public:
	FString GetCategory() const override;
	FString GetOperation() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	EClaireonToolSessionMode GetSessionMode() const override { return EClaireonToolSessionMode::RequiresSession; }
	bool RequiresNoPIE() const override { return true; }

	FString GetFullDescription() const override;
	FString GetExampleUsage() const override;
	TSharedPtr<FJsonObject> GetParameterTooltips() const override;
	TArray<FString> GetSearchKeywords() const override;
};
