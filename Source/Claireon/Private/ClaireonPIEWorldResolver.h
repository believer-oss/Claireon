// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Misc/Optional.h"
#include "Templates/SharedPointer.h"

class FJsonObject;
class UWorld;

/**
 * Shared PIE world resolution for MCP tools (WI-13).
 *
 * Historically every PIE-facing tool picked the FIRST WorldType==PIE context
 * in GEngine->GetWorldContexts(). Under Play-as-Client that is always the
 * server world, making client-side actors unreachable. This helper honors two
 * optional schema parameters:
 *
 *   - pie_instance (integer): target a specific PIE instance index when
 *     multiple PIE world contexts exist (multi-client PIE).
 *   - net_mode ('server' or 'client'): target the world by network role.
 *
 * With neither parameter provided, the first live PIE context is returned,
 * preserving the historical default behavior.
 */
namespace ClaireonPIEWorldResolver
{
	/** Caller request parsed from tool arguments. Unset fields mean "no filter". */
	struct FPIEWorldRequest
	{
		TOptional<int32> PIEInstance;

		// Validated by ParseRequest to be exactly "server" or "client".
		TOptional<FString> NetMode;
	};

	/**
	 * A lightweight, engine-independent view of one PIE world context, used so
	 * the selection logic can be unit-tested against fabricated context arrays
	 * without creating real UWorlds.
	 */
	struct FPIEContextCandidate
	{
		int32 PIEInstance = INDEX_NONE;
		bool bHasWorld = false;
		bool bIsServer = false;
		bool bIsClient = false;
	};

	/**
	 * Parse the optional 'pie_instance' and 'net_mode' fields from tool
	 * arguments. Returns false with OutError set when a provided value is
	 * malformed (wrong JSON type, or net_mode outside 'server'/'client').
	 * A null/absent Arguments object parses to an empty request.
	 */
	bool ParseRequest(const TSharedPtr<FJsonObject>& Arguments, FPIEWorldRequest& OutRequest, FString& OutError);

	/**
	 * Pure selection core: pick the index of the candidate matching Request,
	 * or INDEX_NONE with OutError set (error names the filter value that
	 * failed and lists the available PIE contexts). With an empty Request the
	 * first live candidate wins (historical behavior).
	 */
	int32 SelectPIEContextIndex(TArrayView<const FPIEContextCandidate> Candidates, const FPIEWorldRequest& Request, FString& OutError);

	/**
	 * Engine-facing entry point: enumerate GEngine's PIE world contexts,
	 * apply the request parsed from Arguments, and return the selected world.
	 * Returns nullptr with OutError set on any failure (no PIE session, bad
	 * parameter, or no context matching the filters).
	 */
	UWorld* ResolvePIEWorld(const TSharedPtr<FJsonObject>& Arguments, FString& OutError);

	/**
	 * Add the shared 'pie_instance' and 'net_mode' property declarations to a
	 * tool schema's "properties" object.
	 */
	void AddSchemaParams(const TSharedPtr<FJsonObject>& Properties);

	/**
	 * Test-only seam: while set to a non-null world, ResolvePIEWorld returns
	 * that world unconditionally (before any parsing or enumeration). Pass
	 * nullptr to clear. Must be balanced by the test that sets it.
	 */
	void SetPIEWorldOverrideForTesting(UWorld* World);
}
