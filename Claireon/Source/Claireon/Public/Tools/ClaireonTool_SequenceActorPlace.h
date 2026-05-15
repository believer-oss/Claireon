// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool for placing an ALevelSequenceActor into the current level (F4).
 * Stateless -- no session required. Requires no-PIE to mutate the editor world.
 *
 * Parallels claireon.place_actor: spawns via UEditorActorSubsystem, sets the
 * actor label, binds a ULevelSequence via SetSequence, applies playback_settings
 * via FProperty::ImportText_Direct, and marks the map package dirty. Opt-in
 * save_map param calls UEditorLoadingAndSavingUtils::SaveMap.
 */
class ClaireonTool_SequenceActorPlace : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual FString GetFullDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual bool RequiresEditorWorld() const override { return true; }
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
