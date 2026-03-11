// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool that reads a Blueprint asset's public interface: functions, variables,
 * components, parent class, and implemented interfaces.
 *
 * Works with standard Blueprints, Animation Blueprints, Widget Blueprints,
 * and Blueprint Function Libraries.
 */
class ClaireonTool_GetBlueprintProperties : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual FString GetCategory() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

private:
	/** Load a Blueprint from an asset path and validate it */
	class UBlueprint* LoadBlueprintFromPath(const FString& AssetPath, FString& OutError);

	/** Validate that an asset path starts with /Game/ */
	static bool ValidateAssetPath(const FString& AssetPath, FString& OutError);

	/** Format Blueprint variables section */
	FString FormatVariables(const class UBlueprint* Blueprint, bool bIncludeInherited);

	/** Format Blueprint functions section */
	FString FormatFunctions(const class UBlueprint* Blueprint, bool bIncludeInherited);

	/** Format Blueprint components section */
	FString FormatComponents(const class UBlueprint* Blueprint);

	/** Format Blueprint implemented interfaces section */
	FString FormatInterfaces(const class UBlueprint* Blueprint);

	/** Format Blueprint graphs summary (event/function graphs with node counts) */
	FString FormatGraphSummary(const class UBlueprint* Blueprint);

	/** Get the Blueprint type name (Normal, AnimBlueprint, WidgetBlueprint, etc.) */
	static FString GetBlueprintTypeName(const class UBlueprint* Blueprint);

	/** Get property flags as a string array */
	static FString FormatPropertyFlags(uint64 PropertyFlags);

	/** Format a variable type as a human-readable string */
	static FString FormatVariableType(const struct FEdGraphPinType& PinType);
};
