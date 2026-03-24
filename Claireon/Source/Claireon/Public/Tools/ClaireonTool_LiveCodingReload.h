// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

class ClaireonTool_LiveCodingReload : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual FString GetCategory() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	/** Execute the deferred Live Coding reload (called from the post-execution hook). */
	static void ExecuteDeferredLiveCodingReload(const FString& Payload);

private:
	/** Runs git diff to detect header file changes. Returns true if any .h/.hpp/.inl files changed. */
	bool DetectHeaderChanges(TArray<FString>& OutChangedHeaders) const;
};
