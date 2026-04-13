// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonAnimEditToolBase.h"

// Section Operations
DECLARE_ANIM_TOOL(ClaireonAnimTool_AddSection);
DECLARE_ANIM_TOOL(ClaireonAnimTool_RemoveSection);
DECLARE_ANIM_TOOL(ClaireonAnimTool_SetSectionLink);
DECLARE_ANIM_TOOL(ClaireonAnimTool_SetSectionLinkMethod);

// Slot/Segment Operations
DECLARE_ANIM_TOOL(ClaireonAnimTool_AddSegment);
DECLARE_ANIM_TOOL(ClaireonAnimTool_RemoveSegment);
DECLARE_ANIM_TOOL(ClaireonAnimTool_SetSegmentProperty);
DECLARE_ANIM_TOOL(ClaireonAnimTool_AddSlot);
DECLARE_ANIM_TOOL(ClaireonAnimTool_RemoveSlot);
DECLARE_ANIM_TOOL(ClaireonAnimTool_SetSlotProperty);
DECLARE_ANIM_TOOL(ClaireonAnimTool_InspectSegment);
DECLARE_ANIM_TOOL(ClaireonAnimTool_RetimeSegment);
DECLARE_ANIM_TOOL(ClaireonAnimTool_BatchRetimeAnimation);
