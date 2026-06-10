// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Stateless / read-only MCP tool that returns a Blueprint's GeneratedClass
 * path by reading the field directly off the loaded UBlueprint.
 *
 * Avoids the SEH 0xC0000005 access-violation seen on
 * BlueprintEditorLibrary.generated_class after add_variable + compile
 * (O6). That call routes through the access-checked editor-property
 * path; reading TObjectPtr<UClass> via C++ field access bypasses it.
 *
 * Pair with the "use claireon.* instead of raw get_editor_property"
 * guidance surfaced by python_execute.GetFullDescription.
 */
class ClaireonTool_BlueprintGetGeneratedClass : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
