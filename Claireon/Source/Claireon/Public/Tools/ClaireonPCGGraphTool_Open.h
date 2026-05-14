// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonPCGGraphEditToolBase.h"

class CLAIREON_API ClaireonPCGGraphTool_Open : public ClaireonPCGGraphEditToolBase
{
public:
	FString GetName() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	// P3: synonym/abbreviation keywords for tools_search ranking
	virtual TArray<FString> GetSearchKeywords() const override;
};
