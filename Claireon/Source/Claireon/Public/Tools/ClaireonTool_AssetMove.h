// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Move/rename an asset by wrapping IAssetTools::RenameAssets with a single
 * FAssetRenameData. Optionally fixes up redirectors. Non-session.
 *
 * Two call shapes:
 *   - asset_move(old_path, new_path)
 *   - asset_move(asset_path, new_name)  (same-package rename)
 */
class CLAIREON_API ClaireonTool_AssetMove : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	virtual bool RequiresNoPIE() const override { return true; }
};
