// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"
#include "UObject/WeakObjectPtr.h"

class UPCGGraph;
class UPCGNode;
class UPCGSettings;
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

	/** Counts consecutive calls resolved via asset_path (no session_id). Used by EmitSessionHintIfNeeded. */
	int32 ConsecutiveAssetPathCalls = 0;

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
 * Base class for all individual PCG Graph editing MCP tools.
 * Provides shared session management and state response building.
 */
class CLAIREON_API ClaireonPCGGraphEditToolBase : public IClaireonTool
{
public:
	/** Tool name used when registering sessions with the session manager. */
	static const TCHAR* PCGSessionToolName;

	/** Shared per-session tool data, keyed by session ID. */
	static TMap<FString, FPCGGraphEditToolData> ToolData;

	/** Whether the session-closed delegate has been registered. */
	static bool bDelegateRegistered;

	/** Delegate handler: cleans up tool data when a session closes. */
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	/** Ensures the session-closed delegate is registered (idempotent). */
	static void EnsureDelegateRegistered();

	bool RequiresNoPIE() const override { return true; }

	/** All PCG tools share the "pcg" category. */
	FString GetCategory() const override { return TEXT("pcg"); }

protected:
	/**
	 * Looks up session and tool data for a session-requiring operation.
	 * Reads session_id and suppress_output from Arguments.
	 * Returns false and sets OutError on failure.
	 */
	bool RequireSession(
		const TSharedPtr<FJsonObject>& Arguments,
		FString& OutSessionId,
		FPCGGraphEditToolData*& OutData,
		FString& OutError);

	/** Builds the standard state response with graph structure. */
	FToolResult BuildStateResponse(const FString& SessionId, FPCGGraphEditToolData* Data);
};

// Macro to reduce declaration boilerplate for individual PCG tool classes.
#define DECLARE_PCG_TOOL(ClassName) \
	class CLAIREON_API ClassName : public ClaireonPCGGraphEditToolBase \
	{ \
	public: \
		FString GetOperation() const override; \
		FString GetDescription() const override; \
		TSharedPtr<FJsonObject> GetInputSchema() const override; \
		FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override; \
	}
