// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonStateTreeEditToolBase.h"

class CLAIREON_API ClaireonStateTreeTool_Open : public ClaireonStateTreeEditToolBase
{
public:
	FString GetOperation() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	// synonym/abbreviation keywords for search ranking
	virtual TArray<FString> GetSearchKeywords() const override;
};
