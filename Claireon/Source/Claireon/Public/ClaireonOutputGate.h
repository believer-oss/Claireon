// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Tools/IClaireonTool.h"

/**
 * Size-based routing for tool results.
 * Small results are returned directly; large results are indexed for later retrieval.
 */
class FClaireonOutputGate
{
public:
	/** Route a tool result — returns direct JSON or an index reference */
	static FString RouteResult(const IClaireonTool::FToolResult& Result);

	/** Route log output — returns logs directly or an index reference */
	static FString RouteLogs(const FString& Logs);
};
