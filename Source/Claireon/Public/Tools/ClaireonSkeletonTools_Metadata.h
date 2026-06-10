// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonSkeletonHelpers.h"

// Animation notify names (editor-only cache on USkeleton)
DECLARE_SKELETON_TOOL(ClaireonSkeletonTool_AddAnimationNotify);
DECLARE_SKELETON_TOOL(ClaireonSkeletonTool_RemoveAnimationNotify);
DECLARE_SKELETON_TOOL(ClaireonSkeletonTool_RenameAnimationNotify);

// Curve metadata (UAnimCurveMetaData stored as skeleton asset user data)
DECLARE_SKELETON_TOOL(ClaireonSkeletonTool_AddCurveMetadata);
DECLARE_SKELETON_TOOL(ClaireonSkeletonTool_RemoveCurveMetadata);
DECLARE_SKELETON_TOOL(ClaireonSkeletonTool_RenameCurveMetadata);
DECLARE_SKELETON_TOOL(ClaireonSkeletonTool_SetCurveMetadataFlags);

// NOTE: Generic UAssetUserData add/remove/set_property tools intentionally omitted.
// Persona does not expose UAssetUserData on skeletons in a usable way and no internal
// workflow relies on them, so tool surface was dropped per team request.
