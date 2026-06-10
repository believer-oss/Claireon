// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonWidgetBPEditToolBase.h"

class CLAIREON_API ClaireonWidgetBPTool_RenameAnimation : public ClaireonWidgetBPEditToolBase
{
public:
	FString GetOperation() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
	TArray<FString> GetSearchKeywords() const override;
};
