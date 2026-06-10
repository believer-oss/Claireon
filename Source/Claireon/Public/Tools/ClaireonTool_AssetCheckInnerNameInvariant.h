// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Stateless read-only audit that walks the asset registry and reports
 * packages whose on-disk short-name disagrees with the inner top-level
 * UObject's name (the UE package-name / inner-name invariant). Such
 * mismatches manifest as duplicate editor tabs that share state and as
 * silent load_asset failures from Python / Blueprint scripts.
 *
 * Wire name: asset_check_inner_name_invariant
 * Stateless / read-only / non-session: never mutates and requires no
 * open session. Accepts optional contentPath (default /Game) and
 * includePlugins (default false) arguments mirroring asset_list.
 */
class CLAIREON_API ClaireonTool_AssetCheckInnerNameInvariant : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
