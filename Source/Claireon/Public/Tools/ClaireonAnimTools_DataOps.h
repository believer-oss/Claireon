// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonAnimEditToolBase.h"

// Modifier Operations (AnimSequence only)
DECLARE_ANIM_TOOL(ClaireonAnimTool_ListModifiers);
DECLARE_ANIM_TOOL(ClaireonAnimTool_AddModifier);
DECLARE_ANIM_TOOL(ClaireonAnimTool_RemoveModifier);
DECLARE_ANIM_TOOL(ClaireonAnimTool_ApplyModifier);
DECLARE_ANIM_TOOL(ClaireonAnimTool_RevertModifier);

// Metadata Operations
DECLARE_ANIM_TOOL(ClaireonAnimTool_ListMetadata);
DECLARE_ANIM_TOOL(ClaireonAnimTool_AddMetadata);
DECLARE_ANIM_TOOL(ClaireonAnimTool_RemoveMetadata);
DECLARE_ANIM_TOOL(ClaireonAnimTool_SetMetadataProperty);

// Property Operations
DECLARE_ANIM_TOOL(ClaireonAnimTool_SetProperty);
