// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"
#include "UObject/WeakObjectPtr.h"

class AInstancedFoliageActor;
class UFoliageType;
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

	/** Check if the tool data is still valid */
	bool IsValid() const
	{
		return FoliageActor.IsValid();
	}
};

/**
 * Base class for all individual foliage editing MCP tools.
 * Provides shared session management and state response building.
 */
class CLAIREON_API ClaireonFoliageEditToolBase : public IClaireonTool
{
public:
	/** Tool name used when registering sessions with the session manager. */
	static const TCHAR* FoliageSessionToolName;

	/** Shared per-session tool data, keyed by session ID. */
	static TMap<FString, FFoliageEditToolData> ToolData;

	/** Whether the session-closed delegate has been registered. */
	static bool bDelegateRegistered;

	/** Delegate handler: cleans up tool data when a session closes. */
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	/** Ensures the session-closed delegate is registered (idempotent). */
	static void EnsureDelegateRegistered();

	bool RequiresNoPIE() const override { return true; }

	/** All foliage tools share the "foliage" category. */
	FString GetCategory() const override { return TEXT("foliage"); }

protected:
	/** Find a UFoliageType in the foliage actor by name or asset path. Returns nullptr if not found. */
	static UFoliageType* FindFoliageTypeInActor(AInstancedFoliageActor* IFA, const FString& NameOrPath);

	/**
	 * Looks up session and tool data for a session-requiring operation.
	 * Reads session_id and suppress_output from Arguments.
	 * Returns false and sets OutError on failure.
	 */
	bool RequireSession(
		const TSharedPtr<FJsonObject>& Arguments,
		FString& OutSessionId,
		FFoliageEditToolData*& OutData,
		FString& OutError);

	/** Builds the standard state response with foliage type listing. */
	FToolResult BuildStateResponse(const FString& SessionId, FFoliageEditToolData* Data);
};

// Macro to reduce declaration boilerplate for individual foliage tool classes.
#define DECLARE_FOLIAGE_TOOL(ClassName) \
	class CLAIREON_API ClassName : public ClaireonFoliageEditToolBase \
	{ \
	public: \
		FString GetName() const override; \
		FString GetDescription() const override; \
		TSharedPtr<FJsonObject> GetInputSchema() const override; \
		FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override; \
	}
