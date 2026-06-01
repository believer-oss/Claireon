// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"
#include "UObject/WeakObjectPtr.h"

class UBehaviorTree;
class UBehaviorTreeGraph;
class UBTNode;
class UBTDecorator;
class UBTService;
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

	/** Counts consecutive calls resolved via asset_path (no session_id). Used by EmitSessionHintIfNeeded. */
	int32 ConsecutiveAssetPathCalls = 0;

	/** Check if the tool data is still valid (Behavior Tree is still loaded) */
	bool IsValid() const
	{
		return BehaviorTree.IsValid();
	}
};

/**
 * Base class for all individual Behavior Tree editing MCP tools.
 * Provides shared session management and state response building.
 */
class CLAIREON_API ClaireonBehaviorTreeEditToolBase : public IClaireonTool
{
public:
	/** Tool name used when registering sessions with the session manager. */
	static const TCHAR* BehaviorTreeSessionToolName;

	/** Shared per-session tool data, keyed by session ID. */
	static TMap<FString, FBehaviorTreeEditToolData> ToolData;

	/** Whether the session-closed delegate has been registered. */
	static bool bDelegateRegistered;

	/** Delegate handler: cleans up tool data when a session closes. */
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	/** Ensures the session-closed delegate is registered (idempotent). */
	static void EnsureDelegateRegistered();

	bool RequiresNoPIE() const override { return true; }

	/** All behaviortree tools share the "behaviortree" category. */
	FString GetCategory() const override { return TEXT("behaviortree"); }

protected:
	/**
	 * Looks up session and tool data for a session-requiring operation.
	 * Reads session_id and suppress_output from Arguments.
	 * Returns false and sets OutError on failure.
	 */
	bool RequireSession(
		const TSharedPtr<FJsonObject>& Arguments,
		FString& OutSessionId,
		FBehaviorTreeEditToolData*& OutData,
		FString& OutError);

	/** Builds the standard state response with BT structure. */
	FToolResult BuildStateResponse(const FString& SessionId, FBehaviorTreeEditToolData* Data);

	/**
	 * Parse a GUID parameter from JSON. Returns false with OutError on failure.
	 */
	static bool ParseGuidParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, FGuid& OutGuid, FString& OutError);
};

// Macro to reduce declaration boilerplate for individual behaviortree tool classes.
#define DECLARE_BEHAVIORTREE_TOOL(ClassName) \
	class CLAIREON_API ClassName : public ClaireonBehaviorTreeEditToolBase \
	{ \
	public: \
		FString GetOperation() const override; \
		FString GetDescription() const override; \
		TSharedPtr<FJsonObject> GetInputSchema() const override; \
		FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override; \
	}
