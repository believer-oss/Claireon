// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/** Cancel the active transaction group and undo all operations within it. */
class ClaireonTool_TransactionRollbackGroup : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FString GetCategory() const override { return TEXT("transaction"); }
	virtual TArray<FString> GetSearchKeywords() const override { return {TEXT("group"), TEXT("rollback"), TEXT("cancel"), TEXT("revert")}; }
};
