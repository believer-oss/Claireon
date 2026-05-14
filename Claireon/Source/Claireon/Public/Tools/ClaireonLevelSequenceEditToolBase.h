// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"
#include "UObject/WeakObjectPtr.h"

class ULevelSequence;
struct FMCPSessionClosedInfo;

/**
 * Per-tool data for an active Level Sequence edit session.
 * Session lifecycle and locking are managed by FClaireonSessionManager.
 *
 * Struct previously lived in ClaireonTool_SequenceEdit.h; moved here so the
 * decomposed level_sequence_* tools can share it without depending on
 * the deleted monolith.
 */
struct FSequenceEditToolData
{
	/** Weak reference to the Level Sequence being edited */
	TWeakObjectPtr<ULevelSequence> Sequence;

	/** Cursor: which binding index is focused (parallel to PCG edit tool) */
	int32 FocusedBindingIndex = INDEX_NONE;

	/** Cursor: which track index on the focused binding is focused */
	int32 FocusedTrackIndex = INDEX_NONE;

	/** Navigation history -- pairs of (binding_index, track_index) */
	TArray<TPair<int32, int32>> CursorHistory;

	/** Human-readable status of the last operation */
	FString LastOperationStatus;

	/** When true, BuildStateResponse returns minimal output */
	bool bSuppressOutput = false;

	/** Maximum size of cursor history */
	static constexpr int32 MaxHistorySize = 50;

	/** Check if the tool data is still valid (Level Sequence is still loaded) */
	bool IsValid() const
	{
		return Sequence.IsValid();
	}

	/** Push current cursor to history before moving */
	void PushHistory()
	{
		if (CursorHistory.Num() >= MaxHistorySize)
		{
			CursorHistory.RemoveAt(0);
		}
		CursorHistory.Add(TPair<int32, int32>(FocusedBindingIndex, FocusedTrackIndex));
	}
};

/**
 * Base class for all decomposed Level Sequence editing MCP tools
 * (level_sequence_*). Provides shared session management and state
 * response building, mirroring ClaireonBehaviorTreeEditToolBase's shape.
 */
class CLAIREON_API ClaireonLevelSequenceEditToolBase : public IClaireonTool
{
public:
	/** Tool name used when registering sessions with the session manager. */
	static const TCHAR* LevelSequenceSessionToolName;

	/** Shared per-session tool data, keyed by session ID. */
	static TMap<FString, FSequenceEditToolData> ToolData;

	/** Whether the session-closed delegate has been registered. */
	static bool bDelegateRegistered;

	/** Delegate handler: cleans up tool data when a session closes. */
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	/** Ensures the session-closed delegate is registered (idempotent). */
	static void EnsureDelegateRegistered();

	bool RequiresNoPIE() const override { return true; }

	/** All level_sequence tools share the "level_sequence" category. */
	FString GetCategory() const override { return TEXT("level_sequence"); }

protected:
	/**
	 * Looks up session and tool data for a session-requiring operation.
	 * Reads session_id and suppress_output from Arguments.
	 * Returns false and sets OutError on failure.
	 */
	bool RequireSession(
		const TSharedPtr<FJsonObject>& Arguments,
		FString& OutSessionId,
		FSequenceEditToolData*& OutData,
		FString& OutError);

	/** Builds the standard state response with the sequence's structure. */
	FToolResult BuildStateResponse(const FString& SessionId, FSequenceEditToolData* Data);
};

// Macro to reduce declaration boilerplate for individual level_sequence tool classes.
#define DECLARE_LEVEL_SEQUENCE_TOOL(ClassName) \
	class CLAIREON_API ClassName : public ClaireonLevelSequenceEditToolBase \
	{ \
	public: \
		FString GetOperation() const override; \
		FString GetDescription() const override; \
		TSharedPtr<FJsonObject> GetInputSchema() const override; \
		FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override; \
	}
