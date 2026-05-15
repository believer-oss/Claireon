// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"
#include "UObject/WeakObjectPtr.h"

class UMaterialInstanceConstant;
class UMaterialInterface;
struct FMCPSessionClosedInfo;

/**
 * Per-tool data for an active UMaterialInstanceConstant edit session.
 * Session lifecycle and locking are managed by FClaireonSessionManager.
 */
struct FMaterialInstanceEditToolData
{
	/** Weak reference to the material instance being edited */
	TWeakObjectPtr<UMaterialInstanceConstant> Instance;

	/** Human-readable status of the last operation */
	FString LastOperationStatus;

	/** When true, BuildStateResponse returns minimal output */
	bool bSuppressOutput = false;

	bool IsValid() const
	{
		return Instance.IsValid();
	}
};

/**
 * Base class for all individual MaterialInstance editing MCP tools.
 * Provides shared session management and state response building.
 */
class CLAIREON_API ClaireonMaterialInstanceEditToolBase : public IClaireonTool
{
public:
	/** Tool name used when registering sessions with the session manager. */
	static const TCHAR* MaterialInstanceSessionToolName;

	/** Shared per-session tool data, keyed by session ID. */
	static TMap<FString, FMaterialInstanceEditToolData> ToolData;

	/** Whether the session-closed delegate has been registered. */
	static bool bDelegateRegistered;

	/** Delegate handler: cleans up tool data when a session closes. */
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	/** Ensures the session-closed delegate is registered (idempotent). */
	static void EnsureDelegateRegistered();

	bool RequiresNoPIE() const override { return true; }

	/** All MaterialInstance tools share the "material_instance" category. */
	FString GetCategory() const override { return TEXT("material_instance"); }

protected:
	/**
	 * Looks up session and tool data for a session-requiring operation.
	 * Reads session_id and suppress_output from Arguments.
	 * Returns false and sets OutError on failure.
	 */
	bool RequireSession(
		const TSharedPtr<FJsonObject>& Arguments,
		FString& OutSessionId,
		FMaterialInstanceEditToolData*& OutData,
		FString& OutError);

	/** Builds the standard state response with instance info, overrides, etc. */
	FToolResult BuildStateResponse(const FString& SessionId, FMaterialInstanceEditToolData* Data);
};

// Macro to reduce declaration boilerplate for individual material_instance tool classes.
#define DECLARE_MATERIAL_INSTANCE_TOOL(ClassName) \
	class CLAIREON_API ClassName : public ClaireonMaterialInstanceEditToolBase \
	{ \
	public: \
		FString GetName() const override; \
		FString GetDescription() const override; \
		TSharedPtr<FJsonObject> GetInputSchema() const override; \
		FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override; \
	}
