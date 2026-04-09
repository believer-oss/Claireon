// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool for reading the structure of animation assets (AnimSequence, AnimMontage, AnimComposite).
 * Auto-detects asset type. Displays notifies, curves, sync markers, and montage-specific data
 * (sections, slots, blend settings). Stateless -- no session required.
 */
class ClaireonTool_AnimInspect : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
