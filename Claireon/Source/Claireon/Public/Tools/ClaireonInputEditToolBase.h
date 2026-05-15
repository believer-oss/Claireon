// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"
#include "UObject/WeakObjectPtr.h"

class UInputAction;
class UInputMappingContext;
class UInputTrigger;
class UInputModifier;
struct FMCPSessionClosedInfo;

/** Identifies what type of input asset is being edited. */
enum class EInputAssetType : uint8
{
	InputAction,
	MappingContext,
};

/**
 * Per-tool data for an active Enhanced Input edit session.
 * Session lifecycle and locking are managed by FClaireonSessionManager.
 */
struct FInputEditToolData
{
	/** Weak reference to the Input Action being edited (if IA session) */
	TWeakObjectPtr<UInputAction> InputAction;

	/** Weak reference to the Input Mapping Context being edited (if IMC session) */
	TWeakObjectPtr<UInputMappingContext> MappingContext;

	/** Which type of asset this session is editing */
	EInputAssetType AssetType = EInputAssetType::InputAction;

	/** Human-readable status of the last operation */
	FString LastOperationStatus;

	/** When true, BuildStateResponse returns minimal output */
	bool bSuppressOutput = false;

	bool IsValid() const
	{
		return (AssetType == EInputAssetType::InputAction && InputAction.IsValid())
			|| (AssetType == EInputAssetType::MappingContext && MappingContext.IsValid());
	}
};

/**
 * Base class for all individual Enhanced Input editing MCP tools.
 * Provides shared session management and state response building.
 */
class CLAIREON_API ClaireonInputEditToolBase : public IClaireonTool
{
public:
	/** Tool name used when registering sessions with the session manager. */
	static const TCHAR* InputSessionToolName;

	/** Shared per-session tool data, keyed by session ID. */
	static TMap<FString, FInputEditToolData> ToolData;

	/** Whether the session-closed delegate has been registered. */
	static bool bDelegateRegistered;

	/** Delegate handler: cleans up tool data when a session closes. */
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	/** Ensures the session-closed delegate is registered (idempotent). */
	static void EnsureDelegateRegistered();

	bool RequiresNoPIE() const override { return true; }

	/** All Input tools share the "input" category. */
	FString GetCategory() const override { return TEXT("input"); }

protected:
	/**
	 * Looks up session and tool data for a session-requiring operation.
	 * Reads session_id and suppress_output from Arguments.
	 * Returns false and sets OutError on failure.
	 */
	bool RequireSession(
		const TSharedPtr<FJsonObject>& Arguments,
		FString& OutSessionId,
		FInputEditToolData*& OutData,
		FString& OutError);

	/** Validates that the session is editing an Input Action, and returns the IA. */
	UInputAction* RequireInputAction(FInputEditToolData* Data, FString& OutError) const;

	/** Validates that the session is editing a Mapping Context, and returns the IMC. */
	UInputMappingContext* RequireMappingContext(FInputEditToolData* Data, FString& OutError) const;

	/** Builds the standard state response with input-asset info. */
	FToolResult BuildStateResponse(const FString& SessionId, FInputEditToolData* Data);
};

// Macro to reduce declaration boilerplate for individual input tool classes.
#define DECLARE_INPUT_TOOL(ClassName) \
	class CLAIREON_API ClassName : public ClaireonInputEditToolBase \
	{ \
	public: \
		FString GetName() const override; \
		FString GetDescription() const override; \
		TSharedPtr<FJsonObject> GetInputSchema() const override; \
		FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override; \
	}
