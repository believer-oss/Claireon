// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonAnimEditToolBase.h"

// Asset Lifecycle
DECLARE_ANIM_TOOL(ClaireonAnimTool_BlendSpaceCreate);
DECLARE_ANIM_TOOL(ClaireonAnimTool_BlendSpaceDuplicate);
DECLARE_ANIM_TOOL(ClaireonAnimTool_BlendSpaceDelete);

// Inspection
DECLARE_ANIM_TOOL(ClaireonAnimTool_BlendSpaceInspect);

// Sample Operations
DECLARE_ANIM_TOOL(ClaireonAnimTool_BlendSpaceAddSample);
DECLARE_ANIM_TOOL(ClaireonAnimTool_BlendSpaceRemoveSample);
DECLARE_ANIM_TOOL(ClaireonAnimTool_BlendSpaceEditSample);

// Configuration
DECLARE_ANIM_TOOL(ClaireonAnimTool_BlendSpaceSetAxis);
DECLARE_ANIM_TOOL(ClaireonAnimTool_BlendSpaceSetInterpolation);
DECLARE_ANIM_TOOL(ClaireonAnimTool_BlendSpaceSetProperty);

// Metadata
DECLARE_ANIM_TOOL(ClaireonAnimTool_BlendSpaceAddMetadata);
DECLARE_ANIM_TOOL(ClaireonAnimTool_BlendSpaceRemoveMetadata);
