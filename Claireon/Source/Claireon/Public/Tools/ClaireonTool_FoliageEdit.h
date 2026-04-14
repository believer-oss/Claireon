// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

class AInstancedFoliageActor;
struct FMCPSessionClosedInfo;

/**
 * Per-tool data for an active foliage edit session.
 * Session lifecycle and locking are managed by FClaireonSessionManager.
 */
struct FFoliageEditToolData
{
	/** Weak reference to the foliage actor being edited */
	TWeakObjectPtr<AInstancedFoliageActor> FoliageActor;

	/** Human-readable status of the last operation */
	FString LastOperationStatus;

	/** When true, BuildStateResponse returns minimal output */
	bool bSuppressOutput = false;
};

/**
 * MCP tool for foliage instance management using a session-based model.
 * Supports foliage type registration, painting, erasing, density adjustment, and procedural scatter.
 */
class ClaireonTool_FoliageEdit : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FString GetDescription() const override;
	virtual FString GetFullDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

private:
	static TMap<FString, FFoliageEditToolData> ToolData;
	static bool bDelegateRegistered;
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	// Session management
	FToolResult Operation_Open(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Close(const FString& SessionId, FFoliageEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Status(const FString& SessionId, FFoliageEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Foliage type management
	FToolResult Operation_AddFoliageType(const FString& SessionId, FFoliageEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveFoliageType(const FString& SessionId, FFoliageEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Instance operations
	FToolResult Operation_Paint(const FString& SessionId, FFoliageEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Erase(const FString& SessionId, FFoliageEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetDensity(const FString& SessionId, FFoliageEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Scatter(const FString& SessionId, FFoliageEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Save(const FString& SessionId, FFoliageEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Response building
	FToolResult BuildStateResponse(const FString& SessionId, FFoliageEditToolData* Data);
};
