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
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	// P3: synonym/abbreviation keywords for tools_search ranking
	virtual TArray<FString> GetSearchKeywords() const override;

private:
	/** Load a Blueprint from an asset path and validate it */
	class UBlueprint* LoadBlueprintFromPath(const FString& AssetPath, FString& OutError);

	/** Format Blueprint components section */
	FString FormatComponents(const class UBlueprint* Blueprint);

	/** Format Blueprint implemented interfaces section */
	FString FormatInterfaces(const class UBlueprint* Blueprint);

	/** Format Blueprint graphs summary (event/function graphs with node counts) */
	FString FormatGraphSummary(const class UBlueprint* Blueprint);

	/** Get the Blueprint type name (Normal, AnimBlueprint, WidgetBlueprint, etc.) */
	static FString GetBlueprintTypeName(const class UBlueprint* Blueprint);

	/** Format a variable type as a human-readable string */
	static FString FormatVariableType(const struct FEdGraphPinType& PinType);
};
