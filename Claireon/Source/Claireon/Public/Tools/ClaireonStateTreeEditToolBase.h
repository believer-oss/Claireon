// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"
#include "UObject/WeakObjectPtr.h"
#include "Misc/Guid.h"

class UStateTree;
class UStateTreeEditorData;
struct FMCPSessionClosedInfo;

/**
 * Per-tool data for an active State Tree edit session.
 * Session lifecycle and locking are managed by FClaireonSessionManager.
 */
struct FStateTreeEditToolData
{
	/** Weak reference to the State Tree being edited */
	TWeakObjectPtr<UStateTree> StateTree;

	/** Cursor: which state we are focused on */
	FGuid FocusedStateId;

	/** Navigation history (up to 50 entries) */
	TArray<FGuid> CursorHistory;

	/** Human-readable status of the last operation */
	FString LastOperationStatus;

	/** When true, BuildStateResponse returns minimal output instead of full tree state */
	bool bSuppressOutput = false;

	/** Maximum size of cursor history */
	static constexpr int32 MaxHistorySize = 50;

	/** Check if the tool data is still valid (State Tree is still loaded) */
	bool IsValid() const
	{
		return StateTree.IsValid();
	}

	/** Push current state to history before moving cursor */
	void PushHistory()
	{
		if (FocusedStateId.IsValid())
		{
			if (CursorHistory.Num() >= MaxHistorySize)
			{
				CursorHistory.RemoveAt(0);
			}
			CursorHistory.Add(FocusedStateId);
		}
	}
};

/**
 * Base class for all individual State Tree editing MCP tools.
 * Provides shared session management and state response building.
 */
class CLAIREON_API ClaireonStateTreeEditToolBase : public IClaireonTool
{
public:
	/** Tool name used when registering sessions with the session manager. */
	static const TCHAR* StateTreeSessionToolName;

	/** Shared per-session tool data, keyed by session ID. */
	static TMap<FString, FStateTreeEditToolData> ToolData;

	/** Whether the session-closed delegate has been registered. */
	static bool bDelegateRegistered;

	/** Delegate handler: cleans up tool data when a session closes. */
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	/** Ensures the session-closed delegate is registered (idempotent). */
	static void EnsureDelegateRegistered();

	bool RequiresNoPIE() const override { return true; }

	/** All State Tree tools share the "statetree" category. */
	FString GetCategory() const override { return TEXT("statetree"); }

protected:
	/**
	 * Looks up session and tool data for a session-requiring operation.
	 * Reads session_id and suppress_output from Arguments.
	 * Returns false and sets OutError on failure.
	 */
	bool RequireSession(
		const TSharedPtr<FJsonObject>& Arguments,
		FString& OutSessionId,
		FStateTreeEditToolData*& OutData,
		FString& OutError);

	/** Builds the standard state response with State Tree info. */
	FToolResult BuildStateResponse(const FString& SessionId, FStateTreeEditToolData* Data);
};

// Macro to reduce declaration boilerplate for individual statetree tool classes.
#define DECLARE_STATETREE_TOOL(ClassName) \
	class CLAIREON_API ClassName : public ClaireonStateTreeEditToolBase \
	{ \
	public: \
		FString GetName() const override; \
		FString GetDescription() const override; \
		TSharedPtr<FJsonObject> GetInputSchema() const override; \
		FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override; \
	}
