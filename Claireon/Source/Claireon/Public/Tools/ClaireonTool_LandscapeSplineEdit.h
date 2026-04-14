// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

class ULandscapeSplinesComponent;
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
};

/**
 * MCP tool for landscape spline editing using a session-based model.
 * Supports control point and segment creation, modification, removal, and terrain deformation.
 */
class ClaireonTool_LandscapeSplineEdit : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FString GetDescription() const override;
	virtual FString GetFullDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

private:
	static TMap<FString, FLandscapeSplineEditToolData> ToolData;
	static bool bDelegateRegistered;
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	// Session management
	FToolResult Operation_Open(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Close(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Status(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Control point operations
	FToolResult Operation_AddControlPoint(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveControlPoint(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetControlPoint(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Segment operations
	FToolResult Operation_AddSegment(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveSegment(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetSegmentProperty(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Landscape application
	FToolResult Operation_ApplyToLandscape(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Save(const FString& SessionId, FLandscapeSplineEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Response building
	FToolResult BuildStateResponse(const FString& SessionId, FLandscapeSplineEditToolData* Data);
};
