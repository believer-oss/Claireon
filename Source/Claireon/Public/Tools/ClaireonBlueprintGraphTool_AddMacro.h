// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonBlueprintGraphEditToolBase.h"

/**
 * Create a macro graph on a Blueprint, with its entry/exit tunnel pair and the
 * requested input/output pins.
 *
 * Closes the authoring loop that previously had no create step: bp_add_macro makes
 * the macro, the existing Tunnel node_type finds its terminators to wire the body,
 * and MacroInstance instantiates it elsewhere.
 */
class CLAIREON_API ClaireonBlueprintGraphTool_AddMacro : public ClaireonBlueprintGraphEditToolBase
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
