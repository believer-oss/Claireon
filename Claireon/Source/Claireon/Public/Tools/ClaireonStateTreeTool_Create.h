// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Stateless MCP tool that creates a new UStateTree asset with a non-default
 * schema. UStateTreeFactory exposes SetSchemaClass() in C++ but does not mark
 * StateTreeSchemaClass as EditAnywhere, so Python callers cannot reach it via
 * set_editor_property -- they need this C++-side wrapper instead.
 *
 * Returns the new asset path plus the resolved schema class so the caller can
 * confirm what was applied without a follow-up statetree_get_schema call.
 *
 * Stateless: no session opened, no editor focus changes. Callers should follow
 * this with statetree_open to start editing.
 */
class ClaireonStateTreeTool_Create : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
