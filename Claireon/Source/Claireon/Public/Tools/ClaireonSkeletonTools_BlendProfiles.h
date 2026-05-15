// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonSkeletonHelpers.h"

// Blend profiles
DECLARE_SKELETON_TOOL(ClaireonSkeletonTool_AddBlendProfile);
DECLARE_SKELETON_TOOL(ClaireonSkeletonTool_RemoveBlendProfile);
DECLARE_SKELETON_TOOL(ClaireonSkeletonTool_RenameBlendProfile);
DECLARE_SKELETON_TOOL(ClaireonSkeletonTool_SetBlendProfileMode);
DECLARE_SKELETON_TOOL(ClaireonSkeletonTool_SetBlendProfileBoneScale);
DECLARE_SKELETON_TOOL(ClaireonSkeletonTool_ClearBlendProfileBoneScale);

// Blend masks (same UBlendProfile class but Mode = BlendMask).
// NOTE: no RemoveBlendMask exists — there is a stub below that always errors due to an engine bug.
DECLARE_SKELETON_TOOL(ClaireonSkeletonTool_AddBlendMask);
DECLARE_SKELETON_TOOL(ClaireonSkeletonTool_RenameBlendMask);
DECLARE_SKELETON_TOOL(ClaireonSkeletonTool_SetBlendMaskBoneWeight);
DECLARE_SKELETON_TOOL(ClaireonSkeletonTool_ClearBlendMaskBoneWeight);
DECLARE_SKELETON_TOOL(ClaireonSkeletonTool_RemoveBlendMask); // Intentional stub — always errors
