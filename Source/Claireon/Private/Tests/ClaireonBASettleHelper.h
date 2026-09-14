// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

// Test-only BlueprintAssist settlement.
// Prepare the editor in one call and settle in a later call: handler creation needs a real engine frame.
// Pump Slate, editor timers, and BA; never recursively tick the core ticker from MCP.
// Acquire and settle the handler before adding nodes, or its initial snapshot will treat them as existing.

#include "CoreMinimal.h"

#if WITH_CLAIREON_TESTS

class UEdGraph;

namespace ClaireonBASettleHelper
{
	/** Timing and behavior knobs. Defaults match the bounds bp_format settled on. */
	struct FSettleOptions
	{
		/** How long to wait for BA to build a graph handler for the target tab. */
		double HandlerTimeoutSeconds = 5.0;

		/** How long to wait for BA's Slate-driven node-size cache to finish. */
		double NodeSizeTimeoutSeconds = 10.0;

		/** How long to wait for node positions to stop moving. */
		double QuiesceTimeoutSeconds = 20.0;

		/** Require multiple quiet iterations because formatting can pause between batches. */
		int32 RequiredStableRounds = 4;

		/** Call FormatAllEvents after acquiring the handler; SmartFormatAll needs additional state and forces Smart style. */
		bool bFormatAllEvents = false;
	};

	/** Observed settlement state. */
	struct FSettleReport
	{
		/** WITH_BLUEPRINT_ASSIST was 1 for this build. */
		bool bBlueprintAssistCompiledIn = false;

		/** FSlateApplication::IsInitialized(). False in a commandlet. */
		bool bSlateAvailable = false;

		/** A dock tab whose graph editor holds the requested graph was found and activated. */
		bool bTabActivated = false;

		/** FBATabHandler::GetActiveGraphHandler() returned a valid handler. */
		bool bHandlerAcquired = false;

		/** Whether the active handler is focused on the requested graph. */
		bool bIntendedGraphConfirmed = false;

		/** IsCalculatingNodeSize() went false within NodeSizeTimeoutSeconds. */
		bool bNodeSizesSettled = false;

		/** Positions held still for RequiredStableRounds within QuiesceTimeoutSeconds. */
		bool bPositionsSettled = false;

		/** FormatAllEvents() was actually called (bFormatAllEvents and handler confirmed). */
		bool bFormatRequested = false;

		/** Pump iterations spent across all phases. Zero means nothing was driven. */
		int32 PumpIterations = 0;

		/** Nodes in the graph before and after, so a caller can see BA's reroutes. */
		int32 NodesBefore = 0;
		int32 NodesAfter = 0;

		/** How many nodes changed position over the settle. */
		int32 NodesMoved = 0;

		/** Transaction queue lengths, or INDEX_NONE when no buffer is available. */
		int32 TransactionQueueBefore = INDEX_NONE;
		int32 TransactionQueueAfter = INDEX_NONE;

		/** Human-readable trace of what was found, for a failure message worth reading. */
		FString Diagnostics;

		/** The full success condition. Deliberately includes the intended-graph proof. */
		bool Settled() const
		{
			return bIntendedGraphConfirmed && bNodeSizesSettled && bPositionsSettled;
		}
	};

	/** True when a settle can even be attempted: BA compiled in and Slate initialized. */
	bool IsAvailable();

	/**
	 * Activate the graph tab and drive an existing BA handler until node sizes and positions settle.
	 * The editor must already be open. Timeout, wrong-graph, and missing-capability outcomes are not settled.
	 */
	FSettleReport Settle(UEdGraph* Graph, const FSettleOptions& Options = FSettleOptions());

	/** Pump Slate, editor timers, and BA without ticking the core ticker. */
	void PumpOnce(float DeltaSeconds);
}

#endif // WITH_CLAIREON_TESTS
