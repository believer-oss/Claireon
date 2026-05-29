// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonNiagaraEditToolBase.h"

class CLAIREON_API ClaireonNiagaraTool_Open : public ClaireonNiagaraEditToolBase
{
public:
	FString GetOperation() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	// synonym/abbreviation keywords for search ranking
	virtual TArray<FString> GetSearchKeywords() const override;
};
