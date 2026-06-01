// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

class ClaireonTool_MapDuplicate : public IClaireonTool
{
public:
	virtual FString GetOperation() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FString GetDescription() const override;
	virtual FString GetCategory() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	/** Execute the deferred duplicate+open (called from post-execution hook). */
	static void ExecuteDeferredDuplicateAndOpenMap(const FString& Payload);
};
