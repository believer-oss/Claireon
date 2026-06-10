// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Tools/IClaireonTool.h"

class CLAIREON_API ClaireonTool_ComponentTickInspect : public IClaireonTool
{
public:
	virtual FString GetCategory() const override { return TEXT("component"); }
	virtual FString GetOperation() const override { return TEXT("tick_inspect"); }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
	virtual FString GetExampleUsage() const override;
	virtual TSharedPtr<FJsonObject> GetParameterTooltips() const override;
	virtual TArray<FString> GetSearchKeywords() const override;
};
