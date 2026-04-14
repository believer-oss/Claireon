// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

class UInputAction;
class UInputMappingContext;
struct FMCPSessionClosedInfo;

/** Identifies what type of input asset is being edited. */
enum class EInputAssetType : uint8
{
	InputAction,
	MappingContext,
};

/**
 * Per-tool data for an active Enhanced Input edit session.
 * Session lifecycle and locking are managed by FClaireonSessionManager.
 */
struct FInputEditToolData
{
	/** Weak reference to the Input Action being edited (if IA session) */
	TWeakObjectPtr<UInputAction> InputAction;

	/** Weak reference to the Input Mapping Context being edited (if IMC session) */
	TWeakObjectPtr<UInputMappingContext> MappingContext;

	/** Which type of asset this session is editing */
	EInputAssetType AssetType = EInputAssetType::InputAction;

	/** Human-readable status of the last operation */
	FString LastOperationStatus;

	/** When true, BuildStateResponse returns minimal output */
	bool bSuppressOutput = false;

	/** Check if the tool data is still valid */
	bool IsValid() const
	{
		return (AssetType == EInputAssetType::InputAction && InputAction.IsValid())
			|| (AssetType == EInputAssetType::MappingContext && MappingContext.IsValid());
	}
};

/**
 * MCP tool for Enhanced Input editing using a session-based model.
 * Supports creating/modifying Input Actions (value type, triggers, modifiers)
 * and Input Mapping Contexts (mappings, keys, per-mapping triggers/modifiers).
 */
class ClaireonTool_InputEdit : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FString GetDescription() const override;
	virtual FString GetFullDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

private:
	// Session management
	static TMap<FString, FInputEditToolData> ToolData;
	static bool bDelegateRegistered;
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	// Session operations
	FToolResult Operation_Open(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Create(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Close(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Status(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Save(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Input Action operations
	FToolResult Operation_SetValueType(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetActionProperty(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddActionTrigger(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveActionTrigger(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetActionTriggerProperty(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddActionModifier(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveActionModifier(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetActionModifierProperty(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Input Mapping Context operations
	FToolResult Operation_AddMapping(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveMapping(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetMappingKey(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetMappingAction(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddMappingTrigger(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveMappingTrigger(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddMappingModifier(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveMappingModifier(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Response building
	FToolResult BuildStateResponse(const FString& SessionId, FInputEditToolData* Data);
};
