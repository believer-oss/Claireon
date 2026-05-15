// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"
#include "UObject/WeakObjectPtr.h"

class UMaterial;
class UMaterialExpression;
class UMaterialInterface;
struct FMCPSessionClosedInfo;

/**
 * Per-tool data for an active UMaterial edit session.
 * Session lifecycle and locking are managed by FClaireonSessionManager.
 */
struct FMaterialEditToolData
{
	/** Weak reference to the material being edited */
	TWeakObjectPtr<UMaterial> Material;

	/** Human-readable status of the last operation */
	FString LastOperationStatus;

	/** When true, BuildStateResponse returns minimal output */
	bool bSuppressOutput = false;

	bool IsValid() const
	{
		return Material.IsValid();
	}
};

/**
 * Base class for all individual Material editing MCP tools.
 * Provides shared session management and state response building.
 */
class CLAIREON_API ClaireonMaterialEditToolBase : public IClaireonTool
{
public:
	/** Tool name used when registering sessions with the session manager. */
	static const TCHAR* MaterialSessionToolName;

	/** Shared per-session tool data, keyed by session ID. */
	static TMap<FString, FMaterialEditToolData> ToolData;

	/** Whether the session-closed delegate has been registered. */
	static bool bDelegateRegistered;

	/** Delegate handler: cleans up tool data when a session closes. */
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	/** Ensures the session-closed delegate is registered (idempotent). */
	static void EnsureDelegateRegistered();

	bool RequiresNoPIE() const override { return true; }

	/** All Material tools share the "material" category. */
	FString GetCategory() const override { return TEXT("material"); }

protected:
	/**
	 * Looks up session and tool data for a session-requiring operation.
	 * Reads session_id and suppress_output from Arguments.
	 * Returns false and sets OutError on failure.
	 */
	bool RequireSession(
		const TSharedPtr<FJsonObject>& Arguments,
		FString& OutSessionId,
		FMaterialEditToolData*& OutData,
		FString& OutError);

	/** Builds the standard state response with material info. */
	FToolResult BuildStateResponse(const FString& SessionId, FMaterialEditToolData* Data);
};

// Macro to reduce declaration boilerplate for individual material tool classes.
#define DECLARE_MATERIAL_TOOL(ClassName) \
	class CLAIREON_API ClassName : public ClaireonMaterialEditToolBase \
	{ \
	public: \
		FString GetName() const override; \
		FString GetDescription() const override; \
		TSharedPtr<FJsonObject> GetInputSchema() const override; \
		FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override; \
	}
