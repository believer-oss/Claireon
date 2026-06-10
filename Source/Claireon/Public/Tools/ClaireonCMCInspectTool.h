// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Tools/IClaireonTool.h"

class CLAIREON_API ClaireonTool_CMCInspectState : public IClaireonTool
{
public:
	virtual FString GetCategory() const override { return TEXT("cmc"); }
	virtual FString GetOperation() const override { return TEXT("inspect_state"); }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
	virtual FString GetExampleUsage() const override;
	virtual TSharedPtr<FJsonObject> GetParameterTooltips() const override;
	virtual TArray<FString> GetSearchKeywords() const override;
};

class CLAIREON_API ClaireonTool_CMCInspectRootMotion : public IClaireonTool
{
public:
	virtual FString GetCategory() const override { return TEXT("cmc"); }
	virtual FString GetOperation() const override { return TEXT("inspect_root_motion"); }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
	virtual FString GetExampleUsage() const override;
	virtual TSharedPtr<FJsonObject> GetParameterTooltips() const override;
	virtual TArray<FString> GetSearchKeywords() const override;
};

class CLAIREON_API ClaireonTool_CMCInspectPredictionData : public IClaireonTool
{
public:
	virtual FString GetCategory() const override { return TEXT("cmc"); }
	virtual FString GetOperation() const override { return TEXT("inspect_prediction_data"); }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
	virtual FString GetExampleUsage() const override;
	virtual TSharedPtr<FJsonObject> GetParameterTooltips() const override;
	virtual TArray<FString> GetSearchKeywords() const override;
};
