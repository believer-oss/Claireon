// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool that queries asset dependencies and referencers from the Asset Registry.
 * Supports both hard and soft references, with optional recursive traversal.
 */
class ClaireonTool_AssetReferences : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

private:
	/** Recursively gather dependencies with cycle detection and depth limiting */
	void GatherDependenciesRecursive(
		class IAssetRegistry& AssetRegistry,
		FName PackageName,
		bool bIncludeSoft,
		TSet<FName>& Visited,
		TArray<FName>& OutDependencies,
		int32 CurrentDepth,
		int32 MaxDepth);

	/** Format a human-readable size string from bytes */
	static FString FormatDiskSize(int64 SizeBytes);
};
