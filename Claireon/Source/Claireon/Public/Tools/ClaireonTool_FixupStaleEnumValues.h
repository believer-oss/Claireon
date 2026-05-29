// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Walk all assets and re-save any whose serialized state references a stale
 * enum value (post-EnumRedirects rename or value change). Companion to the engine's
 * CoreRedirects/EnumRedirects mechanism, which fires only on Save -- this tool forces
 * the resave so the redirect lands on disk.
 *
 * Dry-run mode emits the list of would-be-changed assets without saving.
 */
class CLAIREON_API ClaireonTool_FixupStaleEnumValues : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	virtual bool RequiresNoPIE() const override { return true; }
};
