// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

class UEnvQuery;
struct FMCPSessionClosedInfo;

/**
 * Per-tool data for an active EQS edit session.
 * Session lifecycle and locking are managed by FClaireonSessionManager.
 */
struct FEQSEditToolData
{
	/** Weak reference to the EQS Query being edited */
	TWeakObjectPtr<UEnvQuery> Query;

	/** Human-readable status of the last operation */
	FString LastOperationStatus;

	/** When true, BuildStateResponse returns minimal output */
	bool bSuppressOutput = false;

	/** Check if the tool data is still valid */
	bool IsValid() const
	{
		return Query.IsValid();
	}
};

/**
 * MCP tool for EQS Query editing using a session-based model.
 * Supports adding/removing options, generators, tests, property editing, and saving.
 */
class ClaireonTool_EQSEdit : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FString GetDescription() const override;
	virtual FString GetFullDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

private:
	// Session management
	static TMap<FString, FEQSEditToolData> ToolData;
	static bool bDelegateRegistered;
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	// Operations
	FToolResult Operation_Open(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_CreateNew(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Close(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Status(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddOption(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveOption(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetGenerator(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddTest(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveTest(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_ReorderTests(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetNodeProperty(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Save(const FString& SessionId, FEQSEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// apply_spec
	FToolResult Operation_ApplySpec(const TSharedPtr<FJsonObject>& Params);

	// Response building
	FToolResult BuildStateResponse(const FString& SessionId, FEQSEditToolData* Data);
};
