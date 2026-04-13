// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonAnimEditToolBase.h"

// ============================================================================
// Macro to reduce declaration boilerplate.
// Each tool overrides: GetName, GetDescription, GetInputSchema, Execute.
// ============================================================================

#define DECLARE_ANIM_TOOL(ClassName) \
	class CLAIREON_API ClassName : public ClaireonAnimEditToolBase \
	{ \
	public: \
		FString GetName() const override; \
		FString GetDescription() const override; \
		TSharedPtr<FJsonObject> GetInputSchema() const override; \
		FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override; \
	}

// ============================================================================
// Session Operations
// ============================================================================

DECLARE_ANIM_TOOL(ClaireonAnimTool_Open);
DECLARE_ANIM_TOOL(ClaireonAnimTool_Close);
DECLARE_ANIM_TOOL(ClaireonAnimTool_GetState);
DECLARE_ANIM_TOOL(ClaireonAnimTool_Save);

// ============================================================================
// Asset Creation (no session needed)
// ============================================================================

DECLARE_ANIM_TOOL(ClaireonAnimTool_CreateMontage);
DECLARE_ANIM_TOOL(ClaireonAnimTool_CreateComposite);
DECLARE_ANIM_TOOL(ClaireonAnimTool_DuplicateAsset);

// ============================================================================
// Notify Operations
// ============================================================================

DECLARE_ANIM_TOOL(ClaireonAnimTool_AddNotify);
DECLARE_ANIM_TOOL(ClaireonAnimTool_RemoveNotify);
DECLARE_ANIM_TOOL(ClaireonAnimTool_MoveNotify);
DECLARE_ANIM_TOOL(ClaireonAnimTool_DuplicateNotify);
DECLARE_ANIM_TOOL(ClaireonAnimTool_SetNotifyProperty);
DECLARE_ANIM_TOOL(ClaireonAnimTool_GetNotifyProperty);
DECLARE_ANIM_TOOL(ClaireonAnimTool_ListNotifyProperties);
DECLARE_ANIM_TOOL(ClaireonAnimTool_AddNotifyTrack);
DECLARE_ANIM_TOOL(ClaireonAnimTool_RemoveNotifyTrack);
DECLARE_ANIM_TOOL(ClaireonAnimTool_RenameNotifyTrack);
DECLARE_ANIM_TOOL(ClaireonAnimTool_ReorderNotifyTrack);

// ============================================================================
// Curve Operations
// ============================================================================

DECLARE_ANIM_TOOL(ClaireonAnimTool_AddCurve);
DECLARE_ANIM_TOOL(ClaireonAnimTool_RemoveCurve);
DECLARE_ANIM_TOOL(ClaireonAnimTool_AddCurveKey);
DECLARE_ANIM_TOOL(ClaireonAnimTool_RemoveCurveKey);
DECLARE_ANIM_TOOL(ClaireonAnimTool_SetCurveKeyProperty);

// ============================================================================
// Montage Section Operations
// ============================================================================

DECLARE_ANIM_TOOL(ClaireonAnimTool_AddSection);
DECLARE_ANIM_TOOL(ClaireonAnimTool_RemoveSection);
DECLARE_ANIM_TOOL(ClaireonAnimTool_SetSectionLink);
DECLARE_ANIM_TOOL(ClaireonAnimTool_SetSectionLinkMethod);

// ============================================================================
// Montage Slot/Segment Operations
// ============================================================================

DECLARE_ANIM_TOOL(ClaireonAnimTool_AddSegment);
DECLARE_ANIM_TOOL(ClaireonAnimTool_RemoveSegment);
DECLARE_ANIM_TOOL(ClaireonAnimTool_SetSegmentProperty);
DECLARE_ANIM_TOOL(ClaireonAnimTool_AddSlot);
DECLARE_ANIM_TOOL(ClaireonAnimTool_RemoveSlot);
DECLARE_ANIM_TOOL(ClaireonAnimTool_SetSlotProperty);
DECLARE_ANIM_TOOL(ClaireonAnimTool_InspectSegment);
DECLARE_ANIM_TOOL(ClaireonAnimTool_RetimeSegment);
DECLARE_ANIM_TOOL(ClaireonAnimTool_BatchRetimeAnimation);

// ============================================================================
// Modifier Operations (AnimSequence only)
// ============================================================================

DECLARE_ANIM_TOOL(ClaireonAnimTool_ListModifiers);
DECLARE_ANIM_TOOL(ClaireonAnimTool_AddModifier);
DECLARE_ANIM_TOOL(ClaireonAnimTool_RemoveModifier);
DECLARE_ANIM_TOOL(ClaireonAnimTool_ApplyModifier);
DECLARE_ANIM_TOOL(ClaireonAnimTool_RevertModifier);

// ============================================================================
// Metadata Operations
// ============================================================================

DECLARE_ANIM_TOOL(ClaireonAnimTool_ListMetadata);
DECLARE_ANIM_TOOL(ClaireonAnimTool_AddMetadata);
DECLARE_ANIM_TOOL(ClaireonAnimTool_RemoveMetadata);
DECLARE_ANIM_TOOL(ClaireonAnimTool_SetMetadataProperty);

// ============================================================================
// Property Operations
// ============================================================================

DECLARE_ANIM_TOOL(ClaireonAnimTool_SetProperty);

#undef DECLARE_ANIM_TOOL
