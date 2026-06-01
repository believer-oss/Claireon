// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonLevelSequenceEditToolBase.h"

class CLAIREON_API ClaireonLevelSequenceTool_RebindActor : public ClaireonLevelSequenceEditToolBase
{
public:
	FString GetOperation() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	// Synonym/abbreviation keywords for search ranking.
	virtual TArray<FString> GetSearchKeywords() const override;
};
