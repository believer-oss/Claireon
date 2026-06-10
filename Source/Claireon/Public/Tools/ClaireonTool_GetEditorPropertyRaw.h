// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Read a UPROPERTY value without coercing through Python's TSubclassOf / enum
 * binding layer. Returns the raw int / FName / asset-path string for the property,
 * tolerating stale enum values (post-EnumRedirects) that would make
 * unreal.get_editor_property raise.
 */
class CLAIREON_API ClaireonTool_GetEditorPropertyRaw : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
