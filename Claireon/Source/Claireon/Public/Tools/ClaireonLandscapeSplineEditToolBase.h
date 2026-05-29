// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"
#include "UObject/WeakObjectPtr.h"

class ULandscapeSplinesComponent;
class ULandscapeSplineControlPoint;
class ULandscapeSplineSegment;
class ALandscapeProxy;
class ULandscapeInfo;
struct FMCPSessionClosedInfo;

/**
 * Per-tool data for an active landscape spline edit session.
 * Session lifecycle and locking are managed by FClaireonSessionManager.
 */
struct FLandscapeSplineEditToolData
{
	/** Weak reference to the splines component being edited */
	TWeakObjectPtr<ULandscapeSplinesComponent> SplinesComponent;

	/** Weak reference to the owning landscape proxy */
	TWeakObjectPtr<ALandscapeProxy> LandscapeProxy;

	/** Weak reference to the landscape info */
	TWeakObjectPtr<ULandscapeInfo> LandscapeInfo;

	/** Currently focused control point index (-1 = none) */
	int32 FocusedControlPointIndex = INDEX_NONE;

	/** Human-readable status of the last operation */
	FString LastOperationStatus;

	/** When true, BuildStateResponse returns minimal output */
	bool bSuppressOutput = false;

	/** Counts consecutive calls resolved via asset_path (no session_id). Used by EmitSessionHintIfNeeded. */
	int32 ConsecutiveAssetPathCalls = 0;

	bool IsValid() const
	{
		return SplinesComponent.IsValid();
	}
};

/**
 * Base class for all individual landscape spline editing MCP tools.
 * Provides shared session management and state response building.
 */
class CLAIREON_API ClaireonLandscapeSplineEditToolBase : public IClaireonTool
{
public:
	/** Tool name used when registering sessions with the session manager. */
	static const TCHAR* LandscapeSplineSessionToolName;

	/** Shared per-session tool data, keyed by session ID. */
	static TMap<FString, FLandscapeSplineEditToolData> ToolData;

	/** Whether the session-closed delegate has been registered. */
	static bool bDelegateRegistered;

	/** Delegate handler: cleans up tool data when a session closes. */
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	/** Ensures the session-closed delegate is registered (idempotent). */
	static void EnsureDelegateRegistered();

	bool RequiresNoPIE() const override { return true; }

	/** All landscape spline tools share the "landscape_spline" category. */
	FString GetCategory() const override { return TEXT("landscape_spline"); }

protected:
	/**
	 * Looks up session and tool data for a session-requiring operation.
	 * Reads session_id and suppress_output from Arguments.
	 * Returns false and sets OutError on failure.
	 */
	bool RequireSession(
		const TSharedPtr<FJsonObject>& Arguments,
		FString& OutSessionId,
		FLandscapeSplineEditToolData*& OutData,
		FString& OutError);

	/** Builds the standard state response with control points and segments. */
	FToolResult BuildStateResponse(const FString& SessionId, FLandscapeSplineEditToolData* Data);
};

// Macro to reduce declaration boilerplate for individual landscape spline tool classes.
#define DECLARE_LANDSCAPE_SPLINE_TOOL(ClassName) \
	class CLAIREON_API ClassName : public ClaireonLandscapeSplineEditToolBase \
	{ \
	public: \
		FString GetName() const override; \
		FString GetDescription() const override; \
		TSharedPtr<FJsonObject> GetInputSchema() const override; \
		FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override; \
	}
