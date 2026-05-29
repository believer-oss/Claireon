// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"
#include "UObject/WeakObjectPtr.h"

class ALandscape;
class ALandscapeProxy;
class ULandscapeInfo;
class ULandscapeLayerInfoObject;
class UMaterialInterface;
struct FMCPSessionClosedInfo;

/**
 * Per-tool data for an active landscape edit session.
 * Session lifecycle and locking are managed by FClaireonSessionManager.
 */
struct FLandscapeEditToolData
{
	/** Weak reference to the landscape proxy being edited */
	TWeakObjectPtr<ALandscapeProxy> LandscapeProxy;

	/** Weak reference to the landscape info */
	TWeakObjectPtr<ULandscapeInfo> LandscapeInfo;

	/** Human-readable status of the last operation */
	FString LastOperationStatus;

	/** When true, BuildStateResponse returns minimal output */
	bool bSuppressOutput = false;

	/** Counts consecutive calls resolved via asset_path (no session_id). Used by EmitSessionHintIfNeeded. */
	int32 ConsecutiveAssetPathCalls = 0;

	/** Check if the tool data is still valid */
	bool IsValid() const
	{
		return LandscapeProxy.IsValid() && LandscapeInfo.IsValid();
	}
};

/**
 * Base class for all individual landscape editing MCP tools.
 * Provides shared session management and state response building.
 */
class CLAIREON_API ClaireonLandscapeEditToolBase : public IClaireonTool
{
public:
	/** Tool name used when registering sessions with the session manager. */
	static const TCHAR* LandscapeSessionToolName;

	/** Shared per-session tool data, keyed by session ID. */
	static TMap<FString, FLandscapeEditToolData> ToolData;

	/** Whether the session-closed delegate has been registered. */
	static bool bDelegateRegistered;

	/** Delegate handler: cleans up tool data when a session closes. */
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	/** Ensures the session-closed delegate is registered (idempotent). */
	static void EnsureDelegateRegistered();

	bool RequiresNoPIE() const override { return true; }

	/** All landscape tools share the "landscape" category. */
	FString GetCategory() const override { return TEXT("landscape"); }

protected:
	/**
	 * Looks up session and tool data for a session-requiring operation.
	 * Reads session_id and suppress_output from Arguments.
	 * Returns false and sets OutError on failure.
	 */
	bool RequireSession(
		const TSharedPtr<FJsonObject>& Arguments,
		FString& OutSessionId,
		FLandscapeEditToolData*& OutData,
		FString& OutError);

	/** Builds the standard state response with landscape info JSON. */
	FToolResult BuildStateResponse(const FString& SessionId, FLandscapeEditToolData* Data);
};

// Macro to reduce declaration boilerplate for individual landscape tool classes.
#define DECLARE_LANDSCAPE_TOOL(ClassName) \
	class CLAIREON_API ClassName : public ClaireonLandscapeEditToolBase \
	{ \
	public: \
		FString GetName() const override; \
		FString GetDescription() const override; \
		TSharedPtr<FJsonObject> GetInputSchema() const override; \
		FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override; \
	}
