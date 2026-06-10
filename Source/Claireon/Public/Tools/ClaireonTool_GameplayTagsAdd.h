// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool that appends one or more gameplay tags to a configured ini source
 * and triggers a single coalesced in-memory refresh, mirroring the
 * Project Settings > Gameplay Tags > Add Tag flow.
 *
 * Wire shape: claireon.gameplay_tags_add(tags=[{tag, dev_comment?}], tag_source?)
 */
class ClaireonTool_GameplayTagsAdd : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual EClaireonToolSessionMode GetSessionMode() const override { return EClaireonToolSessionMode::EditorWide; }
	virtual bool RequiresNoPIE() const override { return false; }
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
