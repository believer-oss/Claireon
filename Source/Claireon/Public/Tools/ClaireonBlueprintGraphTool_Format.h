// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonBlueprintGraphEditToolBase.h"

/**
 * Fault keys indexed by zero-based position in the eligible, representative-GUID-sorted
 * island list. Allows failures after earlier islands have changed.
 * Result failed_phase values retain the unindexed EClaireonBPFormatPhase wire strings.
 */
namespace ClaireonBPFormatFaultSeam
{
	/** Arm to fail the IslandIndex'th island's dispatch, as if BA never formatted it. */
	inline FString Dispatch(int32 IslandIndex)
	{
		return FString::Printf(TEXT("format_selective_dispatch:%d"), IslandIndex);
	}

	/** Arm to fail the IslandIndex'th island's settle, as if positions never quiesced. */
	inline FString Settle(int32 IslandIndex)
	{
		return FString::Printf(TEXT("format_settle:%d"), IslandIndex);
	}

	/** Arm to fail the IslandIndex'th island's post-mutation invariant check. */
	inline FString InvariantValidation(int32 IslandIndex)
	{
		return FString::Printf(TEXT("format_invariant_validation:%d"), IslandIndex);
	}
}

class CLAIREON_API ClaireonBlueprintGraphTool_Format : public ClaireonBlueprintGraphEditToolBase
{
public:
	FString GetOperation() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	// hot-path metadata enrichment
	virtual FString GetFullDescription() const override;
	virtual FString GetExampleUsage() const override;

	// synonym/abbreviation keywords for search ranking
	virtual TArray<FString> GetSearchKeywords() const override;

	virtual bool GetSummaryAggregationSpec(TArray<FClaireonFieldAggregation>& OutSpec) const override;
};
