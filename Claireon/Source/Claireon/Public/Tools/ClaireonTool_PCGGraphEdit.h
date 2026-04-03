// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

class UPCGGraph;
struct FMCPSessionClosedInfo;

/**
 * Per-tool data for an active PCG Graph edit session.
 * Session lifecycle and locking are managed by FClaireonSessionManager.
 */
struct FPCGGraphEditToolData
{
	/** Weak reference to the PCG Graph being edited */
	TWeakObjectPtr<UPCGGraph> PCGGraph;

	/** Cursor: which node index we are focused on */
	int32 FocusedNodeIndex = INDEX_NONE;

	/** Navigation history (up to 50 entries) */
	TArray<int32> CursorHistory;

	/** Human-readable status of the last operation */
	FString LastOperationStatus;

	/** When true, returns only a brief status instead of full graph state */
	bool bSuppressOutput = false;

	/** Maximum size of cursor history */
	static constexpr int32 MaxHistorySize = 50;

	/** Check if the tool data is still valid (PCG Graph is still loaded) */
	bool IsValid() const
	{
		return PCGGraph.IsValid();
	}

	/** Push current index to history before moving cursor */
	void PushHistory()
	{
		if (FocusedNodeIndex != INDEX_NONE)
		{
			if (CursorHistory.Num() >= MaxHistorySize)
			{
				CursorHistory.RemoveAt(0);
			}
			CursorHistory.Add(FocusedNodeIndex);
		}
	}
};

/**
 * MCP tool for interactive PCG Graph editing using a session-based model.
 * Supports opening graphs, adding/removing nodes, connecting/disconnecting edges,
 * modifying node properties, and saving.
 */
class ClaireonTool_PCGGraphEdit : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

private:
	// Session management
	static TMap<FString, FPCGGraphEditToolData> ToolData;
	static bool bDelegateRegistered;
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	// Operations
	FToolResult Operation_Open(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Close(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_GetState(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddNode(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveNode(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Connect(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Disconnect(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_DisconnectAll(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetNodeProperty(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_GetNodeProperties(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_ListNodeTypes(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Focus(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_CursorBack(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Save(const FString& SessionId, FPCGGraphEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Response building
	FToolResult BuildStateResponse(const FString& SessionId, FPCGGraphEditToolData* Data);
};
