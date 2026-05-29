// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Atomic batch Blueprint graph editor — K2 counterpart to animbp_apply_delta.
 *
 * Shape mirrors the anim batch tool: a single call disconnects links, removes nodes,
 * creates new nodes (referenced by local IDs within the same call), and connects pins,
 * all inside one FScopedTransaction.
 *
 * Session-based: requires an existing session opened via bp_open or bp_create.
 * Sessions are shared across the bp_* tools.
 *
 * Node-type surface matches the factory (ClaireonBlueprintNodeFactory) — CallFunction,
 * VariableGet/Set, Branch/Sequence/Cast/CustomEvent/Knot/Comment, Select, MakeArray/Set/Map,
 * GetArrayItem, MakeStruct/BreakStruct, Switch{Integer/String/Name/Enum},
 * ForEachElementInEnum, DoOnceMultiInput, Macro + named aliases, and Generic with class_name.
 * Rare types (Timeline, Delegate variants, EventOverride) are handled by the incremental
 * bp_add_node tool until the factory's typed dispatch is extended.
 */
class CLAIREON_API ClaireonTool_ApplyBlueprintDelta : public IClaireonTool
{
public:
	FString GetOperation() const override;
	FString GetDescription() const override;
	FString GetCategory() const override { return TEXT("bp"); }
	bool RequiresNoPIE() const override { return true; }
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
