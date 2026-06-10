// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonBlueprintGraphEditToolBase.h"

class CLAIREON_API ClaireonBlueprintGraphTool_SetNodeProperty : public ClaireonBlueprintGraphEditToolBase
{
public:
	FString GetOperation() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	virtual FString GetFullDescription() const override;
	virtual FString GetExampleUsage() const override;
	virtual TSharedPtr<FJsonObject> GetParameterTooltips() const override;
	virtual TArray<FString> GetSearchKeywords() const override;

private:
	FToolResult SetNodeProperty_Impl(
		const FString& SessionId,
		FBlueprintEditToolData* Data,
		const TSharedPtr<FJsonObject>& Params);
};
