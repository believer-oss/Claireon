// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonBlueprintGraphEditToolBase.h"

class CLAIREON_API ClaireonBlueprintGraphTool_SetPinValue : public ClaireonBlueprintGraphEditToolBase
{
public:
	FString GetOperation() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	// P1: hot-path metadata enrichment
	virtual FString GetFullDescription() const override;
	virtual FString GetExampleUsage() const override;
	virtual TSharedPtr<FJsonObject> GetParameterTooltips() const override;

	// P3: synonym/abbreviation keywords for search ranking
	virtual TArray<FString> GetSearchKeywords() const override;

private:
	FToolResult SetPinValue_Impl(
		const FString& SessionId,
		FBlueprintEditToolData* Data,
		const TSharedPtr<FJsonObject>& Params);
};
