// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"
#include "UObject/WeakObjectPtr.h"

class UNiagaraSystem;
class UNiagaraEmitter;
class UNiagaraNodeFunctionCall;
struct FMCPSessionClosedInfo;

/**
 * Per-tool data for an active Niagara edit session.
 * Session lifecycle and locking are managed by FClaireonSessionManager.
 */
struct FNiagaraEditToolData
{
	/** Weak reference to the Niagara System being edited */
	TWeakObjectPtr<UNiagaraSystem> System;

	/** Currently focused emitter index (-1 = system level) */
	int32 FocusedEmitterIndex = -1;

	/** Human-readable status of the last operation */
	FString LastOperationStatus;

	/** When true, BuildStateResponse returns minimal output */
	bool bSuppressOutput = false;

	/** Counts consecutive calls resolved via asset_path (no session_id). Used by EmitSessionHintIfNeeded. */
	int32 ConsecutiveAssetPathCalls = 0;

	/** Check if the tool data is still valid */
	bool IsValid() const
	{
		return System.IsValid();
	}
};

/**
 * Base class for all individual Niagara editing MCP tools.
 * Provides shared session management and state response building.
 */
class CLAIREON_API ClaireonNiagaraEditToolBase : public IClaireonTool
{
public:
	/** Tool name used when registering sessions with the session manager. */
	static const TCHAR* NiagaraSessionToolName;

	/** Shared per-session tool data, keyed by session ID. */
	static TMap<FString, FNiagaraEditToolData> ToolData;

	/** Whether the session-closed delegate has been registered. */
	static bool bDelegateRegistered;

	/** Delegate handler: cleans up tool data when a session closes. */
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	/** Ensures the session-closed delegate is registered (idempotent). */
	static void EnsureDelegateRegistered();

	bool RequiresNoPIE() const override { return true; }

	/** All Niagara tools share the "niagara" category. */
	FString GetCategory() const override { return TEXT("niagara"); }

protected:
	/**
	 * Looks up session and tool data for a session-requiring operation.
	 * Reads session_id and suppress_output from Arguments.
	 * Returns false and sets OutError on failure.
	 */
	bool RequireSession(
		const TSharedPtr<FJsonObject>& Arguments,
		FString& OutSessionId,
		FNiagaraEditToolData*& OutData,
		FString& OutError);

	/** Builds the standard state response with Niagara system info. */
	FToolResult BuildStateResponse(const FString& SessionId, FNiagaraEditToolData* Data);
};

// Macro to reduce declaration boilerplate for individual niagara tool classes.
#define DECLARE_NIAGARA_TOOL(ClassName) \
	class CLAIREON_API ClassName : public ClaireonNiagaraEditToolBase \
	{ \
	public: \
		FString GetName() const override; \
		FString GetDescription() const override; \
		TSharedPtr<FJsonObject> GetInputSchema() const override; \
		FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override; \
	}
