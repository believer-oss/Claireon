// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Dom/JsonObject.h"
#include "Misc/Optional.h"
#include "Templates/SharedPointer.h"

/**
 * Mutation outcomes for extraction, function-property changes, and formatting.
 * Wire strings are stable contracts. Select state from observed durable effects;
 * uncertainty resolves to applied_operation_failed.
 */

/**
 * Maximum string length copied back as a top-level scalar after an output-gate spill.
 * Longer strings and nested data are omitted; write recovery text within this bound.
 */
inline constexpr int32 kClaireonInlineScalarMaxChars = 512;

/** last_completed_phase value when no phase completed. */
inline constexpr TCHAR kClaireonBPPhaseNone[] = TEXT("none");

/** All states except AppliedClean set FToolResult::bIsError. */
enum class EClaireonMutationState : uint8
{
	/**
	 * Wire: "refused". bIsError true. No durable mutation remains, proven either before
	 * the first mutation or by matching start and failure snapshots. No cleanup is needed.
	 */
	Refused,

	/** Wire: "applied_clean". bIsError false. Applied as requested. */
	AppliedClean,

	/**
	 * Wire: "applied_operation_failed". bIsError true. Partially applied and retained.
	 * Also the resolution for any return that cannot prove quiescence.
	 */
	AppliedOperationFailed,

	/**
	 * Wire: "applied_validation_failed". bIsError true. Applied and retained, and
	 * validation beyond what the compiler asserts proved the result wrong. Covers a
	 * compile that SUCCEEDS while carrying a semantic-pruning diagnostic; the
	 * compiler's own verdict travels separately in engine_compile_status.
	 */
	AppliedValidationFailed,
};

/** Compiler verdict, present only when a compile runs. */
enum class EClaireonEngineCompileStatus : uint8
{
	/** Wire: "succeeded". */
	Succeeded,

	/** Wire: "failed". */
	Failed,
};

/**
 * Extraction phases; inapplicable phases are omitted from results.
 * Wire values are disjoint across tool families.
 */
enum class EClaireonBPExtractionPhase : uint8
{
	/** Wire: "extract_collapse". Applies always. */
	Collapse,

	/** Wire: "extract_rename". Applies when new_name is supplied; runs after collapse. */
	Rename,

	/** Wire: "extract_purity_update". Applies when the selection is exec-free. */
	PurityUpdate,

	/** Wire: "extract_parameter_synthesis". Opt-in: promote_enclosing_locals is true AND at least one enclosing local is promoted. */
	ParameterSynthesis,

	/** Wire: "extract_entry_reconstruction". Applies when purity or the signature changed. */
	EntryReconstruction,

	/** Wire: "extract_wiring". Applies when synthesized or boundary parameters must be wired at the call site. */
	Wiring,

	/** Wire: "extract_callsite_refresh". Applies when purity or the signature changed, so call sites reconstruct. */
	CallsiteRefresh,

	/** Wire: "extract_compile_validate". Applies always. A failure here is unconditionally applied_validation_failed. */
	CompileValidate,
};

/** Function-property setter phases. Metadata-only changes do not run reconstruction. */
enum class EClaireonBPSetterPhase : uint8
{
	/**
	 * Wire: "setter_merged_state_validation". Applies always, and runs BEFORE any
	 * transaction opens. A failure here is unconditionally refused, proven by
	 * construction, and takes no snapshot.
	 */
	MergedStateValidation,

	/** Wire: "setter_flag_modification". Applies when any of is_pure, is_const, is_static, access_specifier, is_network_call is supplied. */
	FlagModification,

	/** Wire: "setter_metadata_modification". Applies when category or tooltip is supplied. */
	MetadataModification,

	/** Wire: "setter_entry_reconstruction". Applies when a flag change alters the entry node's signature or purity. */
	EntryReconstruction,

	/** Wire: "setter_callsite_refresh". Applies when a flag change alters call-site shape. */
	CallsiteRefresh,

	/** Wire: "setter_compile_validate". Applies when any modification phase ran. A failure here is unconditionally applied_validation_failed. */
	CompileValidate,
};

/** Per-island formatting phases; island identity is reported separately in failed_island_guid. */
enum class EClaireonBPFormatPhase : uint8
{
	/**
	 * Wire: "format_preflight". Runs before its island is mutated, so it decides a
	 * COVERAGE outcome rather than a mutation state. Deliberately absent from the
	 * phase/outcome matrix for that reason.
	 */
	Preflight,

	/** Wire: "format_selective_dispatch". Runs per island; prior islands may already be mutated. */
	SelectiveDispatch,

	/** Wire: "format_settle". Runs per island; prior islands may already be mutated. */
	Settle,

	/**
	 * Wire: "format_invariant_validation". Runs per island, on an island that IS
	 * formatted. A failure here is unconditionally applied_validation_failed, and
	 * carries no engine_compile_status because no compile ran.
	 */
	InvariantValidation,
};

namespace ClaireonBPMutation
{
	/** The frozen wire vocabulary. Never emit an enum name; always emit one of these. */
	CLAIREON_API const TCHAR* ToWireString(EClaireonMutationState State);
	CLAIREON_API const TCHAR* ToWireString(EClaireonEngineCompileStatus Status);
	CLAIREON_API const TCHAR* ToWireString(EClaireonBPExtractionPhase Phase);
	CLAIREON_API const TCHAR* ToWireString(EClaireonBPSetterPhase Phase);
	CLAIREON_API const TCHAR* ToWireString(EClaireonBPFormatPhase Phase);

	/** True for every state except AppliedClean. This is the FToolResult::bIsError polarity. */
	CLAIREON_API bool IsErrorState(EClaireonMutationState State);

	/**
	 * True for AppliedOperationFailed and AppliedValidationFailed.
	 * Use to initialize bMutationRetained; report that field explicitly.
	 */
	CLAIREON_API bool RetainsMutation(EClaireonMutationState State);

	/** Wire field names written by FClaireonBPMutationResult::WriteInlineScalars, in contract order. */
	CLAIREON_API TConstArrayView<const TCHAR*> GetInlineScalarFieldNames();

	/**
	 * Report transactions this tool opened, with titles and caller-group nesting.
	 * This is not a total undo count: plugins can append transactions asynchronously,
	 * and nested transactions share the caller group's entry. This nested report is omitted on spill.
	 */
	CLAIREON_API TSharedPtr<FJsonObject> MakeClaireonTransactionsReport(
		const TArray<FString>& OpenedTitles,
		bool bNestedInActiveGroup);

}

/**
 * Recovery scalars copied back after an output-gate spill. Bulk diagnostics, deltas,
 * and journals remain in the spill file; rollback instructions travel in FToolResult::Hint.
 */
struct FClaireonBPMutationResult
{
	/**
	 * Wire: mutation_state. Defaults to AppliedOperationFailed so an unpopulated result
	 * does not claim success or proven refusal.
	 */
	EClaireonMutationState MutationState = EClaireonMutationState::AppliedOperationFailed;

	/**
	 * Wire: mutation_retained. Reported, not inferred by the caller. Keep in step with
	 * MutationState via ClaireonBPMutation::RetainsMutation.
	 */
	bool bMutationRetained = true;

	/**
	 * Wire: failed_phase. Assign a family phase via ToWireString; empty omits the field.
	 * Present for both applied failure states.
	 */
	FString FailedPhase;

	/** Wire: last_completed_phase. A phase wire string, or kClaireonBPPhaseNone if none completed. */
	FString LastCompletedPhase = kClaireonBPPhaseNone;

	/** Wire: failed_island_guid. Island whose format pipeline failed; empty omits the field. */
	FString FailedIslandGuid;

	/** Wire: engine_compile_status. Present only when a compile ran; absent for formatting. */
	TOptional<EClaireonEngineCompileStatus> EngineCompileStatus;

	/**
	 * Wire: rollback_available. A caller-owned transaction group is active and holds
	 * this operation. Group activity ALONE is not authority to roll back.
	 */
	bool bRollbackAvailable = false;

	/**
	 * Wire: rollback_group_safe. The product-side guard has established that rolling
	 * back now is safe: pending third-party formatting work is settled or provably
	 * absent, and the affected graph population is known. Never emit a rollback
	 * instruction this has not cleared.
	 */
	bool bRollbackGroupSafe = false;

	/**
	 * Wire: undo_record_available. Tool-asserted; false after cancellation or without a
	 * transaction buffer. Buffer length and head identity cannot establish this after cancellation.
	 */
	bool bUndoRecordAvailable = false;

	/**
	 * Wire: asset_path. Bounded by kClaireonInlineScalarMaxChars -- a package path can
	 * approach it, so a longer path is a dropped field, not a truncated one.
	 */
	FString AssetPath;

	/** Wire: session_id. Subject to kClaireonInlineScalarMaxChars. */
	FString SessionId;

	/**
	 * Write top-level scalars to tool-owned Data for preservation after a spill.
	 * Omit absent optionals and empty strings. Called by mutating tools, not transports.
	 */
	void WriteInlineScalars(FJsonObject& OutData) const;
};
