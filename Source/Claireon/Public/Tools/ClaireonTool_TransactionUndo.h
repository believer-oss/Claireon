// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/** Undo the last N transactions in the editor undo buffer. */
class ClaireonTool_TransactionUndo : public IClaireonTool
{
public:
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FString GetCategory() const override { return TEXT("transaction"); }
	virtual TArray<FString> GetSearchKeywords() const override { return {TEXT("undo"), TEXT("revert"), TEXT("history")}; }
};
