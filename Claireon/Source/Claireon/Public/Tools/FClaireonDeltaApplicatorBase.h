// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Tools/IClaireonTool.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Abstract base class that drives all eight per-family apply_delta tools.
 *
 * Subclasses override ValidateArgs, OpenOrReuseSession, ApplyPhase1_Disconnect,
 * ApplyPhase2_Remove, ApplyPhase3_Create, ApplyPhase4_Connect, FinalizeSession,
 * CloseSessionIfOwned and GetFamilyName. Families with restricted phase support
 * override SupportsPhase1Disconnect()/SupportsPhase4Connect() to return false;
 * the driver enforces empty-array invariants for those phases (AR5/AR9).
 *
 */
class CLAIREON_API FClaireonDeltaApplicatorBase
{
public:
	virtual ~FClaireonDeltaApplicatorBase() = default;

	/**
	 * Single entry point. Reads top-level Args, drives the four phases under one
	 * FScopedTransaction, calls subclass overrides for family-specific work,
	 * and builds the response JSON. Rolls back the entire transaction if any
	 * subclass override returns false.
	 *
	 * Args top-level keys:
	 *   session_id?     string -- reuse an existing family session
	 *   asset_path?     string -- open a temporary session via family Open tool
	 *                              (exactly one of session_id / asset_path required)
	 *   disconnect?     array  -- phase 1 entries
	 *   remove_nodes?   array  -- phase 2 entries
	 *   nodes?          array  -- phase 3 entries
	 *   connections?    array  -- phase 4 entries
	 */
	IClaireonTool::FToolResult ApplyDelta(
		const TSharedPtr<FJsonObject>& Args,
		const FString& OperationLabel);

protected:
	// === Subclass overrides ===

	/** Validate args against family-specific schema rules. Called before the
	 *  transaction opens. Populate OutErrors with one or more human-readable
	 *  messages. Returning false short-circuits with a validation error. */
	virtual bool ValidateArgs(
		const TSharedPtr<FJsonObject>& Args,
		TArray<FString>& OutErrors) = 0;

	/** Open or reuse the family session. Sets OutSessionId on success.
	 *  - if Args has session_id, the existing session is reused (bOwnsSession=false).
	 *  - if Args has asset_path, a temporary session is opened (bOwnsSession=true). */
	virtual bool OpenOrReuseSession(
		const TSharedPtr<FJsonObject>& Args,
		FString& OutSessionId,
		FString& OutError) = 0;

	/** Phase callbacks. Each returns true on success; false halts the
	 *  transaction and triggers rollback. */
	virtual bool ApplyPhase1_Disconnect(
		const FString& SessionId,
		const TArray<TSharedPtr<FJsonValue>>& Entries) = 0;
	virtual bool ApplyPhase2_Remove(
		const FString& SessionId,
		const TArray<TSharedPtr<FJsonValue>>& Entries) = 0;
	virtual bool ApplyPhase3_Create(
		const FString& SessionId,
		const TArray<TSharedPtr<FJsonValue>>& Entries) = 0;
	virtual bool ApplyPhase4_Connect(
		const FString& SessionId,
		const TArray<TSharedPtr<FJsonValue>>& Entries) = 0;

	/** Called after phase 4 succeeds (still inside the transaction). */
	virtual void FinalizeSession(const FString& SessionId) = 0;

	/** Called after FinalizeSession on success, and after Transaction.Cancel()
	 *  on the rollback path. The subclass implementation must be safe to call
	 *  after rollback. */
	virtual void CloseSessionIfOwned(const FString& SessionId) = 0;

	/** Family identifier ("behaviortree", "eqs", ...). */
	virtual FString GetFamilyName() const = 0;

	/** Family-specific rollback cleanup, called when any phase callback fails. */
	virtual void Phase3CleanupOnFailure(const FString& SessionId) {}

	/** Phase-support invariants. Defaults to true; families with restricted
	 *  phase support override to return false. When false, the corresponding
	 *  phase's input array MUST be empty/absent; non-empty input fails
	 *  validation up-front. Empty arrays are no-op success (AR9). */
	virtual bool SupportsPhase1Disconnect() const { return true; }
	virtual bool SupportsPhase4Connect() const { return true; }

	// === Shared state, populated by phase callbacks ===

	void RegisterIdMapping(const FString& LocalId, const FString& ActualId);
	FString ResolveLocalId(const FString& LocalId) const;
	void RecordAffected(const FString& EntityId);
	void AddWarning(const FString& W);
	void AddError(const FString& E);
	bool HasCriticalError() const { return bCriticalError; }
	void SetCriticalError() { bCriticalError = true; }

	void MarkRemoved() { ++RemovedCount; }
	void MarkCreated() { ++CreatedCount; }
	void MarkConnection() { ++ConnectionsMade; }

	const TMap<FString, FString>& GetIdMap() const { return IdMap; }
	const TArray<FString>& GetAffectedEntities() const { return AffectedEntities; }
	const TArray<FString>& GetWarnings() const { return Warnings; }
	const TArray<FString>& GetErrors() const { return Errors; }

	/** Cached top-level Args from the current ApplyDelta call. Phase callbacks
	 *  may use this to peek at top-level keys peer to the phase arrays
	 *  (e.g. StateTree's top-level `transitions[]`). */
	const TSharedPtr<FJsonObject>& GetCachedArgs() const { return CachedArgs; }

	bool DoesOwnSession() const { return bOwnsSession; }

private:
	TMap<FString, FString> IdMap;
	TArray<FString> AffectedEntities;
	TArray<FString> Warnings;
	TArray<FString> Errors;
	bool bCriticalError = false;
	bool bOwnsSession = false;
	TSharedPtr<FJsonObject> CachedArgs;
	int32 RemovedCount = 0;
	int32 CreatedCount = 0;
	int32 ConnectionsMade = 0;
};
