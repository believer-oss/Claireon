// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Stateless tool: retarget every reference to a struct type across a Blueprint.
 *
 * Primary consumer: struct-migration workflows (e.g., BP user-defined struct
 * S_ChooserOutputs → native FFSLocoChooserOutputs) where a variable has been
 * retyped via set_variable_type and every downstream Make/Break node and
 * literal struct pin must be updated in bulk.
 *
 * Handled surfaces:
 *   - UK2Node_MakeStruct / UK2Node_BreakStruct whose StructType matches from_struct
 *   - Any pin anywhere whose PinCategory == PC_Struct and PinSubCategoryObject == from_struct
 *
 * Pin-connection preservation: when a Make/Break node is retargeted, connections are
 * snapshotted by pin name, the node is reconstructed against the new struct, and
 * connections whose pin names survive (directly or via fuzzy match / field_map) are
 * reattached. Unmatched pins are reported as warnings and left disconnected.
 *
 * Works on both UBlueprint and UAnimBlueprint assets.
 */
class CLAIREON_API ClaireonTool_ReplaceStructUsage : public IClaireonTool
{
public:
	FString GetName() const override;
	FString GetDescription() const override;
	FString GetCategory() const override { return TEXT("bp"); }
	bool RequiresNoPIE() const override { return true; }
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
