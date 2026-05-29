// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Write a UPROPERTY on a Blueprint's CDO via FProperty reflection, bypassing the
 * Python TSubclassOf<T> binding bug that intermittently raises
 * "Cannot nativize 'BlueprintGeneratedClass' as 'Class'".
 *
 * value_kind selects how the JSON `value` argument is interpreted:
 *   - "auto"   -- introspect the property type; class properties get LoadClass, object
 *                  properties get LoadObject, primitives get ImportText.
 *   - "class"  -- force LoadClass<UObject>(value).
 *   - "object" -- force LoadObject<UObject>(value).
 *   - "text"   -- pass the string through FProperty::ImportText_Direct.
 */
class CLAIREON_API ClaireonTool_BlueprintSetCdoProperty : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	virtual bool RequiresNoPIE() const override { return true; }
};
