// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonBlueprintGraphEditToolBase.h"

/**
 * Compile+save+close every open bp session in one call.
 *
 * bp_close releases a single session without saving; this is the "I am done editing
 * Blueprints, flush everything" counterpart, and the way to clear the locks that gate
 * the EditorWide tools (see bp_open's blocking_scope).
 */
class CLAIREON_API ClaireonBlueprintGraphTool_CloseAll : public ClaireonBlueprintGraphEditToolBase
{
public:
	FString GetOperation() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	// hot-path metadata enrichment
	virtual FString GetFullDescription() const override;
	virtual FString GetExampleUsage() const override;

	// synonym/abbreviation keywords for search ranking
	virtual TArray<FString> GetSearchKeywords() const override;
};
