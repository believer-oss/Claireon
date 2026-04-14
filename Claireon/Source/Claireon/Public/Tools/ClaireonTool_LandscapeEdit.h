// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

class ALandscapeProxy;
class ALandscape;
class ULandscapeInfo;
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

	/** Check if the tool data is still valid */
	bool IsValid() const
	{
		return LandscapeProxy.IsValid() && LandscapeInfo.IsValid();
	}
};

/**
 * MCP tool for landscape creation and editing using a session-based model.
 * Supports sculpting, weight painting, hole punching, material assignment, and layer management.
 */
class ClaireonTool_LandscapeEdit : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FString GetDescription() const override;
	virtual FString GetFullDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

private:
	static TMap<FString, FLandscapeEditToolData> ToolData;
	static bool bDelegateRegistered;
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	// Session management
	FToolResult Operation_Open(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Create(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Close(const FString& SessionId, FLandscapeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Status(const FString& SessionId, FLandscapeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Editing operations
	FToolResult Operation_Sculpt(const FString& SessionId, FLandscapeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_PaintLayer(const FString& SessionId, FLandscapeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_PunchHole(const FString& SessionId, FLandscapeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetMaterial(const FString& SessionId, FLandscapeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddLayer(const FString& SessionId, FLandscapeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Save(const FString& SessionId, FLandscapeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Response building
	FToolResult BuildStateResponse(const FString& SessionId, FLandscapeEditToolData* Data);
};
