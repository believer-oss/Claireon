// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonBlueprintGraphEditToolBase.h"

// get_state is read-only; allowed during PIE.
DECLARE_BPGRAPH_TOOL_PIE_OK(ClaireonBlueprintGraphTool_GetState);
