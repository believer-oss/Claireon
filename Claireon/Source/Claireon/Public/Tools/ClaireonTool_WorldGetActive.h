// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Canonical "give me the world that's actually live" probe.
 * EditorLevelLibrary.get_editor_world() returns null during PIE; callers need an
 * unconditional probe that picks the right world by context (PIE if active,
 * editor otherwise) and surfaces it as a structured `{world_path, world_type, is_pie}`.
 */
class ClaireonTool_WorldGetActive : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
