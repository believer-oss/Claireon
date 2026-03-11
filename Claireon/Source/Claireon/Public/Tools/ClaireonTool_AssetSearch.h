// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool that searches for assets in the Asset Registry by name, class, and path.
 * Results are scored by match quality and returned in ranked order.
 */
class ClaireonTool_AssetSearch : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual FString GetCategory() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

private:
	/** Score an asset against the search query by name. Higher = better match. */
	static int32 ScoreAssetByName(const FString& AssetName, const FString& PackagePath, const FString& Query);

	/** Score an asset against the search query by class name. Higher = better match. */
	static int32 ScoreAssetByClass(const FString& ClassName, const FString& Query);

	/** Score an asset against the search query by package path. Higher = better match. */
	static int32 ScoreAssetByPath(const FString& PackagePath, const FString& Query);

	/** Check if a package path is under any of the allowed search roots */
	static bool IsUnderSearchRoots(const FString& PackagePath, const TArray<FString>& SearchRoots);

	/** Format a human-readable size string from bytes */
	static FString FormatDiskSize(int64 SizeBytes);
};
