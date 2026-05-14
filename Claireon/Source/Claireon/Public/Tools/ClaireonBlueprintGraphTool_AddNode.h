// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonBlueprintGraphEditToolBase.h"

class CLAIREON_API ClaireonBlueprintGraphTool_AddNode : public ClaireonBlueprintGraphEditToolBase
{
public:
	FString GetName() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	// P1: hot-path metadata enrichment
	virtual FString GetFullDescription() const override;
	virtual FString GetExampleUsage() const override;
	virtual TSharedPtr<FJsonObject> GetParameterTooltips() const override;

	// P3: synonym/abbreviation keywords for tools_search ranking
	virtual TArray<FString> GetSearchKeywords() const override;

private:
	FToolResult AddNode_Impl(
		const FString& SessionId,
		FBlueprintEditToolData* Data,
		const TSharedPtr<FJsonObject>& Params);
};
