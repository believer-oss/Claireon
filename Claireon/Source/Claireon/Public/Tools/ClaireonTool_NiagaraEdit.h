// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

class UNiagaraSystem;
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

	/** Check if the tool data is still valid */
	bool IsValid() const
	{
		return System.IsValid();
	}
};

/**
 * MCP tool for Niagara System editing using a session-based model.
 * Supports emitter management, renderer management, property editing, and saving.
 */
class ClaireonTool_NiagaraEdit : public IClaireonTool
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
	static TMap<FString, FNiagaraEditToolData> ToolData;
	static bool bDelegateRegistered;
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	// Session operations
	FToolResult Operation_Open(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Close(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_Status(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_FocusEmitter(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Emitter operations
	FToolResult Operation_AddEmitter(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveEmitter(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RenameEmitter(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetEmitterEnabled(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Renderer operations
	FToolResult Operation_AddRenderer(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveRenderer(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetRendererProperty(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Property operations
	FToolResult Operation_SetEmitterProperty(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Module operations
	FToolResult Operation_ListModules(const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddModule(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveModule(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Module input operations
	FToolResult Operation_GetModuleInputs(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetModuleInput(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// System creation (session-less)
	FToolResult Operation_Create(const TSharedPtr<FJsonObject>& Params);

	// System properties & parameters
	FToolResult Operation_SetSystemProperty(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_AddParameter(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_RemoveParameter(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);
	FToolResult Operation_SetParameterValue(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Compile
	FToolResult Operation_Compile(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// Build operations
	FToolResult Operation_Save(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params);

	// apply_spec
	FToolResult Operation_ApplySpec(const TSharedPtr<FJsonObject>& Params);

	// Response building
	FToolResult BuildStateResponse(const FString& SessionId, FNiagaraEditToolData* Data);
};
