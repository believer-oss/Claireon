// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Read effective variable properties. Compare normalized flags, not the original
 * flags/clear_flags tokens supplied to the setter.
 */
class ClaireonTool_GetVariableProperties : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
	virtual TArray<FString> GetSearchKeywords() const override;

private:
	/** Emit one variable's full property set. Shared by the member and local-variable paths. */
	/**
	 * OwningBlueprint supplies generated/skeleton classes for dispatcher-signature
	 * fallback when the pin type's member reference is empty.
	 */
	static TSharedPtr<FJsonObject> DescribeVariable(const struct FBPVariableDescription& Var,
		class UBlueprint* OwningBlueprint = nullptr);
};
