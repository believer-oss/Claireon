// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

class UStateTree;
struct FMCPSessionClosedInfo;

/**
 * Per-tool data for an active State Tree edit session.
 * Session lifecycle and locking are managed by FClaireonSessionManager.
 */
struct FStateTreeEditToolData
{
	/** Weak reference to the State Tree being edited */
	TWeakObjectPtr<UStateTree> StateTree;

	/** Cursor: which state we are focused on */
	FGuid FocusedStateId;

	/** Navigation history (up to 50 entries) */
	TArray<FGuid> CursorHistory;

	/** Human-readable status of the last operation */
	FString LastOperationStatus;

	/** When true, BuildStateResponse returns minimal output instead of full tree state */
	bool bSuppressOutput = false;

	/** Maximum size of cursor history */
	static constexpr int32 MaxHistorySize = 50;

	/** Check if the tool data is still valid (State Tree is still loaded) */
	bool IsValid() const
	{
		return StateTree.IsValid();
	}

	/** Push current state to history before moving cursor */
	void PushHistory()
	{
		if (FocusedStateId.IsValid())
		{
			if (CursorHistory.Num() >= MaxHistorySize)
			{
				CursorHistory.RemoveAt(0);
			}
			CursorHistory.Add(FocusedStateId);
		}
	}
};

/**
 * MCP tool for interactive State Tree editing using a session-based model.
 * Supports opening State Trees, adding/removing/modifying states, tasks, conditions,
 * transitions, evaluators, considerations, property bindings, and compiling/saving.
 */
class ClaireonTool_StateTreeEdit : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetCategory() const override;
	virtual FString GetDescription() const override;
	virtual FString GetFullDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

private:
	// Session management
	static TMap<FString, FStateTreeEditToolData> ToolData;
	static bool bDelegateRegistered;
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	// Operations
	FToolResult Operation_Open(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddState(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveState(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RenameState(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_MoveState(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetStateType(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetStateSelectionBehavior(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetStateEnabled(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddTask(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveTask(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddEnterCondition(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveEnterCondition(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddConsideration(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveConsideration(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddTransition(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveTransition(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_ModifyTransition(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddTransitionCondition(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveTransitionCondition(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddEvaluator(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveEvaluator(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddGlobalTask(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveGlobalTask(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddBinding(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddPropertyFunction(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveBinding(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetNodeProperty(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Compile(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Save(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Close(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Response building
	FToolResult BuildStateResponse(const FString& SessionId, FStateTreeEditToolData* Data);
};
