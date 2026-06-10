// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Tools/IClaireonTool.h"

/**
 * Base class for all tool-specific apply_spec applicators.
 *
 * Provides the shared lifecycle that every tool-specific applicator follows:
 * 1. Validate spec (structural + semantic)
 * 2. Open or reuse session
 * 3. Two-pass apply: Pass 1 creates entities, Pass 2 wires relationships
 * 4. On critical failure: undo transaction, close session without saving
 * 5. Compile asset
 * 6. Save asset
 * 7. Close session (if we opened it)
 * 8. Return result with ID mappings and per-entry status
 *
 * Subclasses override the virtual methods for tool-specific behavior.
 * The base class manages ID mapping, error accumulation, transaction scoping,
 * and the two-pass apply pattern.
 */
class CLAIREON_API FClaireonSpecApplicatorBase
{
public:
	virtual ~FClaireonSpecApplicatorBase() = default;

	/**
	 * Main entry point. Applies the spec to an asset.
	 * @param Spec The parsed spec JSON object
	 * @param AssetPath UE asset path (e.g. "/Game/AI/BT_EnemyAI")
	 * @param ExistingSessionId Optional session ID to reuse (empty = open new session)
	 * @param bInDryRun If true, validate and run Pass1/Pass2 in-memory but skip
	 *   Compile + Save and roll the transaction back so no on-disk state survives.
	 *   This explicit rollback guards the auto-create branch which would
	 *   otherwise touch disk even under dry_run.
	 * @return FToolResult with id_mappings, per-entry status, warnings, errors
	 */
	IClaireonTool::FToolResult ApplySpec(
		const TSharedPtr<FJsonObject>& Spec,
		const FString& AssetPath,
		const FString& ExistingSessionId = FString(),
		bool bInDryRun = false);

protected:
	// === Subclass overrides ===

	/** Validate tool-specific semantic rules (called after structural validation). */
	virtual bool ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors) = 0;

	/**
	 * Open or create the asset, returning a session ID. During Apply() the active
	 * Spec is available via GetActiveSpec(); subclasses that auto-create can read
	 * spec hints (e.g. parent_class) from it.
	 */
	virtual bool OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError) = 0;

	/** Returns the Spec currently being applied; valid only during Apply(). May be null when called outside Apply(). */
	const TSharedPtr<FJsonObject>& GetActiveSpec() const { return ActiveSpec; }

	/** Pass 1: Create all entities (nodes, states, widgets, etc.). */
	virtual bool ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) = 0;

	/** Pass 2: Wire relationships (connections, parent-child, bindings, etc.). */
	virtual bool ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec) = 0;

	/** Compile the asset (tool-specific). */
	virtual bool CompileAsset(const FString& SessionId, FString& OutError) = 0;

	/** Save the asset to disk (tool-specific). */
	virtual bool SaveAsset(const FString& SessionId, FString& OutError) = 0;

	/** Close the session. */
	virtual void CloseSession(const FString& SessionId) = 0;

	/** Return the tool name for logging (e.g. "BehaviorTree"). */
	virtual FString GetToolName() const = 0;

	// === ID Mapping ===

	/** Register a mapping from spec ID to actual UE ID. */
	void RegisterIdMapping(const FString& SpecId, const FString& ActualId);

	/** Resolve a spec ID to its actual UE ID. Returns empty string if not found. */
	FString ResolveId(const FString& SpecId) const;

	/** Get all ID mappings. */
	const TMap<FString, FString>& GetIdMappings() const;

	// === Per-Entry Status Tracking ===

	/** Record a successful entry. */
	void RecordEntrySuccess(const FString& SpecId, const FString& ActualId);

	/** Record a failed entry. */
	void RecordEntryFailure(const FString& SpecId, const FString& Error);

	/** Record a skipped entry (dependency failed). */
	void RecordEntrySkipped(const FString& SpecId, const FString& Reason);

	// === Error Accumulation ===

	/** Add a non-fatal warning. */
	void AddWarning(const FString& Warning);

	/** Add a fatal error and mark critical failure. */
	void AddError(const FString& Error);

	/** Check if a critical error has occurred. */
	bool HasCriticalError() const;

	/** Check if a given spec ID has been successfully created. */
	bool IsIdCreated(const FString& SpecId) const;

private:
	/** The spec body currently being applied. Stashed by ApplySpec on entry, cleared on exit.
	 *  Available to subclass overrides via GetActiveSpec(). */
	TSharedPtr<FJsonObject> ActiveSpec;

	/** ID map: spec_id -> actual_id (GUIDs, indices, node names, etc.) */
	TMap<FString, FString> IdMap;

	/** Per-entry status for the result */
	struct FEntryStatus
	{
		FString SpecId;
		FString Status;   // "ok", "failed", "skipped"
		FString ActualId;
		FString Error;
	};
	TArray<FEntryStatus> EntryStatuses;

	/** Non-fatal warnings */
	TArray<FString> Warnings;

	/** Fatal errors */
	TArray<FString> Errors;

	/** Whether a critical error has occurred */
	bool bCriticalError = false;

	/** Build the FToolResult from accumulated state */
	IClaireonTool::FToolResult BuildResult(bool bSuccess) const;

	/** Reset internal state for a new apply_spec call */
	void Reset();
};
