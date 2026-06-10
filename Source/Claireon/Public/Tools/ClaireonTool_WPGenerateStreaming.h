// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Diagnostic tool: runs UWorldPartition::GenerateStreaming on the editor world's
 * WorldPartition and inspects the resulting runtime cells.
 *
 * In editor (non-PIE) mode, UWorldPartition::RuntimeStreamingData is empty
 * because runtime cells are only built during PIE or cook. This tool triggers a
 * one-shot build of the runtime cells using the same code path that cook uses,
 * then walks each cell and returns its per-cell info -- specifically the
 * DataLayers (FName instance names) and the actor-package paths it would load.
 *
 * Pairs with wp_actor_desc_inspect: descs answer "is the actor correctly
 * tagged?", this tool answers "did the cell builder put the tagged actor in a
 * cell that respects the tag, or in an empty-DataLayers always-loaded cell?".
 *
 * After cells are inspected, FlushStreaming() is called so the editor returns
 * to its normal non-PIE state.
 *
 * Use the actor_filter to find the cell(s) holding a particular actor by
 * package-name substring (e.g. 'BP_Onboarding_FlowManager' or a uasset GUID
 * like 'TGAH2PS6PG0WR3AXGOY5ET').
 */
class CLAIREON_API ClaireonTool_WPGenerateStreaming : public IClaireonTool
{
public:
	FString GetCategory() const override { return TEXT("wp"); }
	FString GetOperation() const override { return TEXT("generate_streaming"); }
	FString GetDescription() const override;
	TArray<FString> GetSearchKeywords() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
