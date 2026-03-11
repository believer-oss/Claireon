// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

class UBehaviorTree;
class UBehaviorTreeGraph;
struct FMCPSessionClosedInfo;

/**
 * Per-tool data for an active Behavior Tree edit session.
 * Session lifecycle and locking are managed by FClaireonSessionManager.
 */
struct FBehaviorTreeEditToolData
{
	/** Weak reference to the Behavior Tree being edited */
	TWeakObjectPtr<UBehaviorTree> BehaviorTree;

	/** Cached reference to the BTGraph (derived from BehaviorTree->BTGraph) */
	TWeakObjectPtr<UBehaviorTreeGraph> BTGraph;

	/** Currently focused node GUID (for context in responses) */
	FGuid FocusedNodeGuid;

	/** Human-readable status of the last operation */
	FString LastOperationStatus;

	/** When true, BuildStateResponse returns minimal output */
	bool bSuppressOutput = false;

	/** Check if the tool data is still valid (Behavior Tree is still loaded) */
	bool IsValid() const
	{
		return BehaviorTree.IsValid();
	}
};

/**
 * MCP tool for Behavior Tree editing using a session-based model.
 * Supports full node manipulation: add/remove composites, tasks, decorators, services,
 * set properties via reflection, update_asset (rebuild runtime BT from graph), and save.
 */
class ClaireonTool_BehaviorTreeEdit : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetCategory() const override;
	virtual FString GetDescription() const override;
	virtual FString GetFullDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

private:
	// Session management
	static TMap<FString, FBehaviorTreeEditToolData> ToolData;
	static bool bDelegateRegistered;
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	// Session operations
	FToolResult Operation_Open(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Close(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Status(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Node operations
	FToolResult Operation_AddNode(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveNode(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_MoveNode(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetNodeProperty(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Decorator/Service operations
	FToolResult Operation_AddDecorator(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveDecorator(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddService(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveService(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Subtree operations
	FToolResult Operation_SetSubtreeAsset(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Build operations
	FToolResult Operation_UpdateAsset(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Save(const FString& SessionId, FBehaviorTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Discovery
	FToolResult Operation_ListNodeTypes(const TSharedPtr<FJsonObject>& Params);

	// Response building
	FToolResult BuildStateResponse(const FString& SessionId, FBehaviorTreeEditToolData* Data);
};
