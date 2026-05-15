// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

// Stateless asset lifecycle tools — no session required

class CLAIREON_API ClaireonAnimGraphTool_Create : public IClaireonTool
{
public:
	FString GetName() const override;
	FString GetDescription() const override;
	FString GetCategory() const override { return TEXT("animgraph"); }
	bool RequiresNoPIE() const override { return true; }
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};

class CLAIREON_API ClaireonAnimGraphTool_CreateChild : public IClaireonTool
{
public:
	FString GetName() const override;
	FString GetDescription() const override;
	FString GetCategory() const override { return TEXT("animgraph"); }
	bool RequiresNoPIE() const override { return true; }
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};

class CLAIREON_API ClaireonAnimGraphTool_Duplicate : public IClaireonTool
{
public:
	FString GetName() const override;
	FString GetDescription() const override;
	FString GetCategory() const override { return TEXT("animgraph"); }
	bool RequiresNoPIE() const override { return true; }
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
