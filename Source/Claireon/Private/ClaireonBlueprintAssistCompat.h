// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#pragma once

// BlueprintAssist 4.5.x / 4.9.x compatibility. BAFormatRequest.h selects the newer API.
// Caller line references to BlueprintAssistGraphHandler.cpp refer to 4.5.2.

#if WITH_BLUEPRINT_ASSIST

#include "BlueprintAssistGraphHandler.h"
#include "BlueprintAssistInputProcessor.h"
#include "BlueprintAssistActions/BlueprintAssistNodeActions.h"

#if __has_include("BAGraphHandler/BAFormatRequest.h")
#include "BAGraphHandler/BAFormatRequest.h"
#define CLAIREON_BA_HAS_FORMAT_REQUEST 1
#else
#define CLAIREON_BA_HAS_FORMAT_REQUEST 0
#endif

namespace ClaireonBA
{
	/** True while BlueprintAssist still has nodes whose Slate size it has not cached. */
	inline bool IsCalculatingNodeSize(const FBAGraphHandler& Handler)
	{
#if CLAIREON_BA_HAS_FORMAT_REQUEST
		return Handler.GetNumberOfPendingNodesToCache() > 0;
#else
		return Handler.IsCalculatingNodeSize();
#endif
	}

	/** The delegate BlueprintAssist broadcasts once a formatting pass has been applied. */
	inline FOnPostFormatting& OnPostFormatting(FBAGraphHandler& Handler)
	{
#if CLAIREON_BA_HAS_FORMAT_REQUEST
		return Handler.GetFormatRequest().OnPostFormatting;
#else
		return Handler.OnPostFormatting;
#endif
	}

	/** Queue a format-all over the handler's focused graph (the Format All Events command). */
	inline void FormatAllEvents(FBAGraphHandler& Handler)
	{
#if CLAIREON_BA_HAS_FORMAT_REQUEST
		Handler.RequestFormatAll();
#else
		Handler.FormatAllEvents();
#endif
	}

	/** Format the active handler's selected nodes (the Format Nodes Selectively command). */
	inline void FormatNodesSelectively()
	{
#if CLAIREON_BA_HAS_FORMAT_REQUEST
		FBAInputProcessor::Get().NodeActions->FormatNodesSelectively();
#else
		FBAInputProcessor::Get().NodeActions.FormatNodesSelectively();
#endif
	}
}

#endif // WITH_BLUEPRINT_ASSIST
