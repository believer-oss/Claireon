// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

class CLAIREON_API ClaireonTool_ChooserEdit : public IClaireonTool
{
public:
	FString GetName() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	bool RequiresNoPIE() const override { return true; }
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};

class CLAIREON_API ClaireonTool_ChooserAddRow : public IClaireonTool
{
public:
	FString GetName() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	bool RequiresNoPIE() const override { return true; }
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};

class CLAIREON_API ClaireonTool_ChooserRemoveRow : public IClaireonTool
{
public:
	FString GetName() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	bool RequiresNoPIE() const override { return true; }
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};

class CLAIREON_API ClaireonTool_ChooserSetRowResult : public IClaireonTool
{
public:
	FString GetName() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	bool RequiresNoPIE() const override { return true; }
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};

class CLAIREON_API ClaireonTool_ChooserSetColumnValue : public IClaireonTool
{
public:
	FString GetName() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	bool RequiresNoPIE() const override { return true; }
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};

class CLAIREON_API ClaireonTool_ChooserAddColumn : public IClaireonTool
{
public:
	FString GetName() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	bool RequiresNoPIE() const override { return true; }
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};

class CLAIREON_API ClaireonTool_ChooserRemoveColumn : public IClaireonTool
{
public:
	FString GetName() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	bool RequiresNoPIE() const override { return true; }
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
