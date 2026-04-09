// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

class UAnimSequenceBase;
struct FMCPSessionClosedInfo;

/**
 * Per-tool data for an active animation edit session.
 * Session lifecycle and locking are managed by FClaireonSessionManager.
 */
struct FAnimEditToolData
{
	/** Weak reference to the animation being edited */
	TWeakObjectPtr<UAnimSequenceBase> Animation;

	/** Detected asset type: "AnimSequence", "AnimMontage", or "AnimComposite" */
	FString AssetType;

	/** Human-readable status of the last operation */
	FString LastOperationStatus;

	/** When true, BuildStateResponse returns minimal output instead of full animation state */
	bool bSuppressOutput = false;

	/** Index of the currently focused notify, or -1 if none */
	int32 FocusedNotifyIndex = -1;

	/** Check if the tool data is still valid (animation is still loaded) */
	bool IsValid() const { return Animation.IsValid(); }
};

/**
 * MCP tool for interactive animation asset editing using a session-based model.
 * Supports editing notifies (including skeleton-style notifies), curves, montage sections,
 * data modifiers, metadata, and properties.
 * Use 'open' to start a session, then operations to modify, 'save' to persist, and 'close' to end.
 */
class ClaireonTool_AnimEdit : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual FString GetFullDescription() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

private:
	// Session management
	static TMap<FString, FAnimEditToolData> ToolData;
	static bool bDelegateRegistered;
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	// Session ops
	FToolResult Operation_Open(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Close(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_GetState(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Save(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Notify ops
	FToolResult Operation_AddNotify(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveNotify(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_MoveNotify(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetNotifyProperty(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddNotifyTrack(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveNotifyTrack(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_GetNotifyProperty(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_ListNotifyProperties(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_DuplicateNotify(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RenameNotifyTrack(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_ReorderNotifyTrack(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Curve ops
	FToolResult Operation_AddCurve(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveCurve(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddCurveKey(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveCurveKey(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetCurveKeyProperty(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Montage section ops
	FToolResult Operation_AddSection(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveSection(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetSectionLink(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Modifier ops
	FToolResult Operation_ListModifiers(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddModifier(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveModifier(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_ApplyModifier(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RevertModifier(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Metadata ops
	FToolResult Operation_ListMetadata(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddMetadata(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveMetadata(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetMetadataProperty(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Property ops
	FToolResult Operation_SetProperty(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Response building
	FToolResult BuildStateResponse(const FString& SessionId, FAnimEditToolData* Data);
};
