// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonAnimEditToolBase.h"

// Asset Lifecycle
DECLARE_BLENDSPACE_TOOL(ClaireonAnimTool_BlendSpaceCreate);
DECLARE_BLENDSPACE_TOOL(ClaireonAnimTool_BlendSpaceDuplicate);
DECLARE_BLENDSPACE_TOOL(ClaireonAnimTool_BlendSpaceDelete);

// Inspection
DECLARE_BLENDSPACE_TOOL(ClaireonAnimTool_BlendSpaceInspect);

// Sample Operations
DECLARE_BLENDSPACE_TOOL(ClaireonAnimTool_BlendSpaceAddSample);
DECLARE_BLENDSPACE_TOOL(ClaireonAnimTool_BlendSpaceRemoveSample);
DECLARE_BLENDSPACE_TOOL(ClaireonAnimTool_BlendSpaceEditSample);

// Configuration
DECLARE_BLENDSPACE_TOOL(ClaireonAnimTool_BlendSpaceSetAxis);
DECLARE_BLENDSPACE_TOOL(ClaireonAnimTool_BlendSpaceSetInterpolation);
DECLARE_BLENDSPACE_TOOL(ClaireonAnimTool_BlendSpaceSetProperty);

// Metadata
DECLARE_BLENDSPACE_TOOL(ClaireonAnimTool_BlendSpaceAddMetadata);
DECLARE_BLENDSPACE_TOOL(ClaireonAnimTool_BlendSpaceRemoveMetadata);
