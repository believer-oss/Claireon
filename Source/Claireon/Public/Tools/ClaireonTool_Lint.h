// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"
#include "ClaireonLintTypes.h"

/**
 * Report Blueprint hygiene, topology, geometry, and variable-policy findings without mutation.
 * Layout is opt-in; omitted scope selects variables, graph, functions, and hygiene.
 * Exec topology is shared with bp_get_graph through ClaireonExecTopology.
 */
class ClaireonTool_Lint : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
	virtual TArray<FString> GetSearchKeywords() const override;

private:
	/** Resolve scopes: omitted excludes layout; "all" includes it. Unknown names fail. */
	static bool ResolveScopes(const TSharedPtr<FJsonObject>& Arguments,
	                          TSet<EClaireonLintScope>& OutScopes,
	                          FString& OutError);

	static TSharedPtr<FJsonObject> FindingToJson(const FClaireonLintFinding& Finding);
};
