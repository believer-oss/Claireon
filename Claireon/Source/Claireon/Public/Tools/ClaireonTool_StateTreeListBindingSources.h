// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Read-only MCP tool that enumerates binding sources for a State Tree asset.
 * Walks UStateTreeEditorData::VisitGlobalNodes (always) and optionally
 * UStateTreeEditorData::VisitStateNodes(state_id) -- including the StateEvent
 * record for states with bHasRequiredEventToEnter = true.
 *
 * Stateless: never mutates and requires no open session.
 */
class ClaireonTool_StateTreeListBindingSources : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
