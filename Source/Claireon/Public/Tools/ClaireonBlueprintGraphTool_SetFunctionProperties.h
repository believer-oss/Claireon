// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonBlueprintGraphEditToolBase.h"

/**
 * Update existing function properties after validating the complete proposed state,
 * including omitted properties. Reject invalid or unverified combinations before mutation.
 * Net-mode results verify flags and compiler diagnostics, not runtime RPC routing;
 * validate routing in a client/server session before relying on it for non-synthetic assets.
 */
DECLARE_BPGRAPH_TOOL(ClaireonBlueprintGraphTool_SetFunctionProperties);
