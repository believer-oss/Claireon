// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Stateless tool: inspect a USTRUCT's schema (native C++ or Blueprint user-defined).
 * Reports field list with names, types, flags, metadata, and optional default values.
 * Primary consumer: chooser / blueprint migration workflows that need to compare struct
 * shapes (e.g., S_ChooserOutputs vs FNativeChooserOutputs).
 */
class CLAIREON_API ClaireonTool_StructInspect : public IClaireonTool
{
public:
	FString GetOperation() const override;
	FString GetDescription() const override;
	FString GetCategory() const override { return TEXT("struct"); }
	bool RequiresNoPIE() const override { return false; }
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
