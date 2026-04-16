// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

class UBlackboardData;

struct FMCPSessionClosedInfo;

/**
 * Per-tool data for an active Blackboard edit session.
 * Session lifecycle and locking are managed by FClaireonSessionManager.
 */
struct FBlackboardEditToolData
{
	/** Weak reference to the Blackboard being edited */
	TWeakObjectPtr<UBlackboardData> BlackboardData;

	/** Human-readable status of the last operation */
	FString LastOperationStatus;

	/** When true, BuildStateResponse returns minimal output */
	bool bSuppressOutput = false;

	/** Check if the tool data is still valid */
	bool IsValid() const
	{
		return BlackboardData.IsValid();
	}
};

/**
 * MCP tool for Blackboard Data editing using a session-based model.
 * Supports adding/removing/renaming keys, changing key types, setting parent BB, and saving.
 */
class ClaireonTool_BlackboardEdit : public IClaireonTool
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
	static TMap<FString, FBlackboardEditToolData> ToolData;
	static bool bDelegateRegistered;
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	// Operations
	FToolResult Operation_Open(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Close(const FString& SessionId, FBlackboardEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Status(const FString& SessionId, FBlackboardEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddKey(const FString& SessionId, FBlackboardEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveKey(const FString& SessionId, FBlackboardEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RenameKey(const FString& SessionId, FBlackboardEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetKeyType(const FString& SessionId, FBlackboardEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetParent(const FString& SessionId, FBlackboardEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Save(const FString& SessionId, FBlackboardEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// apply_spec
	FToolResult Operation_ApplySpec(const TSharedPtr<FJsonObject>& Params);

	// Response building
	FToolResult BuildStateResponse(const FString& SessionId, FBlackboardEditToolData* Data);
};
