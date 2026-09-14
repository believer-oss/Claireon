// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonPCGGraphEditToolBase.h"

/**
 * Override a parameter for one subgraph instance. UpdatePropertyOverride must record
 * the property GUID as well as the bag value, or RefreshParameters reverts it.
 */
DECLARE_PCG_TOOL(ClaireonPCGGraphTool_SetSubgraphOverride);
