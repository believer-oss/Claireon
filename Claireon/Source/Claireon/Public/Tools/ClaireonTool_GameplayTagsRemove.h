// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool that deletes one or more gameplay tags from a configured ini source
 * via IGameplayTagsEditorModule::DeleteTagsFromINI, which coalesces the
 * in-memory refresh for the whole batch.
 *
 * Wire shape: claireon.gameplay_tags_remove(tags=[string], tag_source?)
 */
class ClaireonTool_GameplayTagsRemove : public IClaireonTool
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
