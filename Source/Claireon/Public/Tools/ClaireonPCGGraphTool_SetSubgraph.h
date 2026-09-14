// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonPCGGraphEditToolBase.h"

/**
 * Assign a subgraph through SetSubgraph so recursion checks, callbacks, and parameter
 * refresh run. Writing the nested graph reference directly bypasses those steps.
 */
DECLARE_PCG_TOOL(ClaireonPCGGraphTool_SetSubgraph);
