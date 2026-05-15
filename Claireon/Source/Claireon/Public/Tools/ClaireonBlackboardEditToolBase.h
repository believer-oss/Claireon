// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"
#include "UObject/WeakObjectPtr.h"

class UBlackboardData;
class UBlackboardKeyType;
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
 * Base class for all individual Blackboard editing MCP tools.
 * Provides shared session management and state response building.
 */
class CLAIREON_API ClaireonBlackboardEditToolBase : public IClaireonTool
{
public:
	/** Tool name used when registering sessions with the session manager. */
	static const TCHAR* BlackboardSessionToolName;

	/** Shared per-session tool data, keyed by session ID. */
	static TMap<FString, FBlackboardEditToolData> ToolData;

	/** Whether the session-closed delegate has been registered. */
	static bool bDelegateRegistered;

	/** Delegate handler: cleans up tool data when a session closes. */
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	/** Ensures the session-closed delegate is registered (idempotent). */
	static void EnsureDelegateRegistered();

	bool RequiresNoPIE() const override { return true; }

	/** All blackboard tools share the "blackboard" category. */
	FString GetCategory() const override { return TEXT("blackboard"); }

protected:
	/** Create a UBlackboardKeyType for a given type name. Returns nullptr and sets OutError on failure. */
	static UBlackboardKeyType* CreateKeyTypeForName(const FString& TypeName, UObject* Outer, FString& OutError);

	/**
	 * Looks up session and tool data for a session-requiring operation.
	 * Reads session_id and suppress_output from Arguments.
	 * Returns false and sets OutError on failure.
	 */
	bool RequireSession(
		const TSharedPtr<FJsonObject>& Arguments,
		FString& OutSessionId,
		FBlackboardEditToolData*& OutData,
		FString& OutError);

	/** Builds the standard state response with blackboard structure. */
	FToolResult BuildStateResponse(const FString& SessionId, FBlackboardEditToolData* Data);
};

// Macro to reduce declaration boilerplate for individual blackboard tool classes.
#define DECLARE_BLACKBOARD_TOOL(ClassName) \
	class CLAIREON_API ClassName : public ClaireonBlackboardEditToolBase \
	{ \
	public: \
		FString GetName() const override; \
		FString GetDescription() const override; \
		TSharedPtr<FJsonObject> GetInputSchema() const override; \
		FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override; \
	}
