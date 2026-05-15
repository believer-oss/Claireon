// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool that forces UGameplayTagsManager to reload DefaultGameplayTags.ini
 * from disk via GConfig->LoadFile and rebuild its in-memory tag tree via
 * EditorRefreshGameplayTagTree. No-arg recovery / out-of-band-edit trigger.
 *
 * Wire shape: claireon.gameplay_tags_reload()
 */
class ClaireonTool_GameplayTagsReload : public IClaireonTool
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
