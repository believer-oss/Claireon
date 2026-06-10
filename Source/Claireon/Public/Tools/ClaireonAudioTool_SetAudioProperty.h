// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool: reflection-write a property on an existing audio actor or component.
 * Targeting is by actor_name OR component_path ("<actor_label>.<component_name>"), exclusively.
 * RequiresNoPIE AND RequiresEditorWorld.
 */
class FClaireonAudioTool_SetAudioProperty : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual bool RequiresEditorWorld() const override { return true; }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
