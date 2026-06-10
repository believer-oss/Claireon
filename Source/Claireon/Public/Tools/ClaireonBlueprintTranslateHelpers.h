// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Tools/IClaireonTool.h"

/**
 * Shared helpers used by the decomposed Claireon blueprint_translate_implement tools
 * (inspect / implement / force_implement / skip / mark_complete).
 *
 * The dispatcher in ClaireonTool_BlueprintTranslateImplement has been split into per-action
 * tools per the "one Claireon tool does one verb" invariant; these helpers preserve the
 * common file-region parsing across the new tools.
 */
namespace ClaireonBlueprintTranslateHelpers
{
	/**
	 * Locate a //[BP] tagged region in file content for a given node GUID.
	 * Returns the start and end positions of the region content (after tag line,
	 * before next tag or scope end). Returns false if the GUID's tag line cannot
	 * be located.
	 */
	bool FindBPTagRegion(const FString& FileContent, const FString& NodeGuid,
		int32& OutRegionStart, int32& OutRegionEnd, int32& OutTagLineStart, int32& OutTagLineEnd);

	/**
	 * Shared body for the implement / force_implement family. Performs (optional) hash
	 * check, file-region replacement, session update, and session save. ToolName is
	 * used for FClaireonScopedAssetLock identification. Returns a FToolResult ready to
	 * return from the caller's Execute().
	 */
	IClaireonTool::FToolResult DoImplement(
		const TSharedPtr<FJsonObject>& Arguments,
		const FString& ToolName,
		bool bEnforceHashCheck);
}
