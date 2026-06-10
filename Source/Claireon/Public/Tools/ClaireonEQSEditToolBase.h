// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"
#include "UObject/WeakObjectPtr.h"

class UEnvQuery;
class UEnvQueryOption;
class UEnvQueryGenerator;
class UEnvQueryTest;
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

	/** Counts consecutive calls resolved via asset_path (no session_id). Used by EmitSessionHintIfNeeded. */
	int32 ConsecutiveAssetPathCalls = 0;

	/** Check if the tool data is still valid */
	bool IsValid() const
	{
		return Query.IsValid();
	}
};

/**
 * Base class for all individual EQS editing MCP tools.
 * Provides shared session management and state response building.
 */
class CLAIREON_API ClaireonEQSEditToolBase : public IClaireonTool
{
public:
	/** Tool name used when registering sessions with the session manager. */
	static const TCHAR* EQSSessionToolName;

	/** Shared per-session tool data, keyed by session ID. */
	static TMap<FString, FEQSEditToolData> ToolData;

	/** Whether the session-closed delegate has been registered. */
	static bool bDelegateRegistered;

	/** Delegate handler: cleans up tool data when a session closes. */
	static void HandleSessionClosed(const FMCPSessionClosedInfo& Info);

	/** Ensures the session-closed delegate is registered (idempotent). */
	static void EnsureDelegateRegistered();

	bool RequiresNoPIE() const override { return true; }

	/** All EQS tools share the "eqs" category. */
	FString GetCategory() const override { return TEXT("eqs"); }

protected:
	/**
	 * Resolve an EQS class name (e.g. "SimpleGrid") to a UClass of the expected base type.
	 * Tries the bare name first, then with the conventional prefix (e.g. "EnvQueryGenerator_SimpleGrid").
	 * Returns nullptr and sets OutError on failure.
	 */
	static UClass* ResolveEQSClass(const FString& ClassName, UClass* BaseClass, const FString& BasePrefix, FString& OutError);

	/**
	 * Set a single property on an EQS node (generator or test) via reflection / ImportText.
	 * Returns false and sets OutError on failure.
	 */
	static bool SetEQSNodeProperty(UObject* Node, const FString& PropertyName, const FString& PropertyValue, FString& OutError);

	/**
	 * Get a mutable reference to the Options array on a UEnvQuery.
	 * UEnvQuery::GetOptions() returns const ref; we need write access for editing.
	 * The query must already be marked for modification via Modify() before calling this.
	 */
	static TArray<UEnvQueryOption*>& GetOptionsMutable(UEnvQuery* Query);

	/**
	 * Looks up session and tool data for a session-requiring operation.
	 * Reads session_id and suppress_output from Arguments.
	 * Returns false and sets OutError on failure.
	 */
	bool RequireSession(
		const TSharedPtr<FJsonObject>& Arguments,
		FString& OutSessionId,
		FEQSEditToolData*& OutData,
		FString& OutError);

	/** Builds the standard state response with EQS structure. */
	FToolResult BuildStateResponse(const FString& SessionId, FEQSEditToolData* Data);
};

// Macro to reduce declaration boilerplate for individual EQS tool classes.
#define DECLARE_EQS_TOOL(ClassName) \
	class CLAIREON_API ClassName : public ClaireonEQSEditToolBase \
	{ \
	public: \
		FString GetOperation() const override; \
		FString GetDescription() const override; \
		TSharedPtr<FJsonObject> GetInputSchema() const override; \
		FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override; \
	}
