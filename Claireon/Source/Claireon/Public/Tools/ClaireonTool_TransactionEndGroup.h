// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/** End the active transaction group. The group appears as a single entry in undo history. */
class ClaireonTool_TransactionEndGroup : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FString GetCategory() const override { return TEXT("transaction"); }
	virtual TArray<FString> GetSearchKeywords() const override { return {TEXT("group"), TEXT("batch"), TEXT("commit")}; }
};
