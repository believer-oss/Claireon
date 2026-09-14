// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonBlueprintGraphEditToolBase.h"

#include "ClaireonGraphIslands.h"

class UBlueprint;
class UEdGraph;

/**
 * Stack graph islands in one column using rigid translation; preserve intra-island layout.
 * Operates on node positions without Slate or BlueprintAssist.
 */
class CLAIREON_API ClaireonBlueprintGraphTool_StackIslands : public ClaireonBlueprintGraphEditToolBase
{
public:
	FString GetOperation() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	// synonym/abbreviation keywords for search ranking
	virtual TArray<FString> GetSearchKeywords() const override;
};

/** Stacking operations shared by the tool and headless graph tests. */
namespace ClaireonStackIslands
{
	/** Default graph-unit gap, shared with event extraction. */
	inline constexpr double DefaultGutter = ClaireonGraphIslands::DefaultStackGutter;

	/** In which order the to-stack islands are laid down the column. */
	enum class EStackOrder : uint8
	{
		/** Ascending Island.Box.MinY, ties by Representative. The order a reader already sees. */
		ByCurrentY,

		/** Ascending primary-entry name, islands without one last, ties by Representative. */
		ByEntryName,

		/** Exactly the order given in island_order, which must be a permutation of the to-stack set. */
		Explicit,
	};

	/** Wire spelling of an order, and its inverse. Empty/unknown input fails the parse. */
	CLAIREON_API const TCHAR* ToWireString(EStackOrder Order);
	CLAIREON_API bool ParseOrder(const FString& Wire, EStackOrder& OutOrder);

	/** What the caller asked for, with every default already resolved except the rail. */
	struct CLAIREON_API FStackRequest
	{
		/** Column x; ignored unless bRailXExplicit. Otherwise use ResolveRailX(Graph). */
		double RailX = 0.0;
		bool bRailXExplicit = false;

		double Gutter = DefaultGutter;

		EStackOrder Order = EStackOrder::ByCurrentY;

		/** Representative GUIDs, in the order to stack. Required for, and only legal with, Explicit. */
		TArray<FString> IslandOrder;

		/** Representative GUIDs that must not move. Parked clusters are real; see the tool description. */
		TArray<FString> ExcludeIslands;

		/** Skip disconnected single nodes by default to preserve parked scratch nodes. */
		bool bIncludeSingletons = false;

		bool bCarryComments = true;
	};

	/** An island that was NOT moved, and why. Never silently dropped from the report. */
	struct CLAIREON_API FSkippedIsland
	{
		FString Representative;
		int32 NodeCount = 0;

		/** Wire vocabulary: see PolicySingleton() and friends below. */
		FString Reason;
	};

	/** An island that WAS moved, with the two x values that prove the rail alignment held. */
	struct CLAIREON_API FPlacedIsland
	{
		FString Representative;
		int32 NodeCount = 0;

		/** The integer delta applied to every member. One per island, by construction. */
		int32 DeltaX = 0;
		int32 DeltaY = 0;

		/** Entry x before and after translation. EntryXAfter must equal the requested rail. */
		double EntryXBefore = 0.0;
		double EntryXAfter = 0.0;
	};

	struct CLAIREON_API FCarriedComment
	{
		FString CommentGuid;

		/** Representative of the one island whose delta this comment received. */
		FString Island;
	};

	/** A comment that was left where it was, and why. */
	struct CLAIREON_API FSkippedComment
	{
		FString CommentGuid;
		FString PolicyReason;

		/** Representatives of every island the comment holds. Empty only for NoIslandMember. */
		TArray<FString> SpansIslands;
	};

	/** Anchor-space box, matching ClaireonGraphIslands::FIslandBox. bValid false means "no content". */
	struct CLAIREON_API FStackBounds
	{
		bool bValid = false;
		double MinX = 0.0;
		double MinY = 0.0;
		double MaxX = 0.0;
		double MaxY = 0.0;
	};

	/** Report moved and skipped islands, including calls that move nothing. */
	struct CLAIREON_API FStackReport
	{
		/** The values actually used, resolved defaults included. */
		double RailX = 0.0;
		double Gutter = DefaultGutter;
		EStackOrder Order = EStackOrder::ByCurrentY;

		/** Echo policy so callers can distinguish disabled behavior from empty results. */
		bool bCarryComments = true;
		bool bIncludeSingletons = false;

		int32 IslandsTotal = 0;
		int32 NodesMoved = 0;

		TArray<FPlacedIsland> IslandsPlaced;
		TArray<FSkippedIsland> IslandsSkipped;

		TArray<FCarriedComment> CommentsCarried;
		TArray<FSkippedComment> CommentsSkipped;

		/** Union over the TO-STACK islands' members only, so before and after are comparable. */
		FStackBounds BoundsBefore;
		FStackBounds BoundsAfter;

		/**
		 * Whether ResolveRailX still returns RailX after moving. An explicit rail may
		 * differ from the next default rail, so a later default-rail call may move the column.
		 */
		bool bRailStable = true;

		/** What a DEFAULT-rail call would resolve now. Equals RailX whenever bRailStable. */
		double RailNextDefault = 0.0;

		/** Human sentence, always populated, and the only place the "moved nothing" case is stated. */
		FString Summary;

		/** Non-empty on refusal; validation finishes before mutation begins. */
		FString Error;

		/** bp_format's refusal vocabulary, reused verbatim: `bad_argument` or `no_islands`. */
		FString RefusalReason;
	};

	/** Why policy left an island alone. Wire strings; `singleton` matches bp_format's. */
	CLAIREON_API const TCHAR* PolicySingleton();
	CLAIREON_API const TCHAR* PolicyExcludedByRequest();

	/** Wire reasons for leaving comments in place; shared vocabulary with bp_format. */
	CLAIREON_API const TCHAR* PolicySpanningComment();
	CLAIREON_API const TCHAR* PolicyNoIslandMember();
	CLAIREON_API const TCHAR* PolicyIslandNotStacked();

	/**
	 * Stack islands in one transaction when there is movement. Return false on refusal,
	 * with OutReport.Error populated and the graph unchanged.
	 */
	CLAIREON_API bool Apply(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const FStackRequest& Request,
		FStackReport& OutReport);

	/** The report as the snake_case payload both tool paths return. Never null. */
	CLAIREON_API TSharedPtr<FJsonObject> ToJson(const FStackReport& Report);
}
