// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Probe whether an asset editor is currently open for a given asset.
 * Companion to claireon.open_asset, which is deferred-to-next-tick; callers that
 * need to know when the open landed can poll this probe after wait_seconds.
 */
class ClaireonTool_IsAssetEditorOpen : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
