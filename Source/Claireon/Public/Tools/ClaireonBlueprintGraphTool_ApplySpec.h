// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonBlueprintGraphEditToolBase.h"

// apply_spec drops the static PIE block. Execute() checks IsPlaySessionInProgress
// at runtime and rejects non-dry-run during PIE; dry_run=true is honored at any time.
DECLARE_BPGRAPH_TOOL_PIE_OK(ClaireonBlueprintGraphTool_ApplySpec);
