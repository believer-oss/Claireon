// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonBlueprintGraphEditToolBase.h"

// Slate node selection for the session-bound graph widget, separate from the cursor.
// Requires an editor window and is not transactional; undo cannot restore it.

DECLARE_BPGRAPH_TOOL(ClaireonBlueprintGraphTool_SelectionGet);
DECLARE_BPGRAPH_TOOL(ClaireonBlueprintGraphTool_SelectionSet);
DECLARE_BPGRAPH_TOOL(ClaireonBlueprintGraphTool_SelectionClear);
