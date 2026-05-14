// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Tools/IClaireonTool.h"

class CLAIREON_API ClaireonTool_AnimInspectMontages : public IClaireonTool
{
public:
	virtual FString GetCategory() const override { return TEXT("anim"); }
	virtual FString GetOperation() const override { return TEXT("inspect_montages"); }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
	virtual FString GetExampleUsage() const override;
	virtual TSharedPtr<FJsonObject> GetParameterTooltips() const override;
	virtual TArray<FString> GetSearchKeywords() const override;
};

class CLAIREON_API ClaireonTool_AnimInspectMotionWarping : public IClaireonTool
{
public:
	virtual FString GetCategory() const override { return TEXT("anim"); }
	virtual FString GetOperation() const override { return TEXT("inspect_motion_warping"); }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
	virtual FString GetExampleUsage() const override;
	virtual TSharedPtr<FJsonObject> GetParameterTooltips() const override;
	virtual TArray<FString> GetSearchKeywords() const override;
};
