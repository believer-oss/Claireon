// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBPMutationResult.h"

#include "Dom/JsonValue.h"

namespace ClaireonBPMutationResultInternal
{
	inline constexpr TCHAR ClBPMut_FieldMutationState[]        = TEXT("mutation_state");
	inline constexpr TCHAR ClBPMut_FieldMutationRetained[]     = TEXT("mutation_retained");
	inline constexpr TCHAR ClBPMut_FieldFailedPhase[]          = TEXT("failed_phase");
	inline constexpr TCHAR ClBPMut_FieldLastCompletedPhase[]   = TEXT("last_completed_phase");
	inline constexpr TCHAR ClBPMut_FieldFailedIslandGuid[]     = TEXT("failed_island_guid");
	inline constexpr TCHAR ClBPMut_FieldEngineCompileStatus[]  = TEXT("engine_compile_status");
	inline constexpr TCHAR ClBPMut_FieldRollbackAvailable[]    = TEXT("rollback_available");
	inline constexpr TCHAR ClBPMut_FieldRollbackGroupSafe[]    = TEXT("rollback_group_safe");
	inline constexpr TCHAR ClBPMut_FieldUndoRecordAvailable[]  = TEXT("undo_record_available");
	inline constexpr TCHAR ClBPMut_FieldAssetPath[]            = TEXT("asset_path");
	inline constexpr TCHAR ClBPMut_FieldSessionId[]            = TEXT("session_id");

	/**
	 * The eleven names, in the order 010-shared-result-contract.md lists them.
	 */
	static const TCHAR* ClBPMut_InlineScalarFieldNames[] =
	{
		ClBPMut_FieldMutationState,
		ClBPMut_FieldMutationRetained,
		ClBPMut_FieldFailedPhase,
		ClBPMut_FieldLastCompletedPhase,
		ClBPMut_FieldFailedIslandGuid,
		ClBPMut_FieldEngineCompileStatus,
		ClBPMut_FieldRollbackAvailable,
		ClBPMut_FieldRollbackGroupSafe,
		ClBPMut_FieldUndoRecordAvailable,
		ClBPMut_FieldAssetPath,
		ClBPMut_FieldSessionId,
	};
}

namespace ClaireonBPMutation
{

const TCHAR* ToWireString(EClaireonMutationState State)
{
	switch (State)
	{
	case EClaireonMutationState::Refused:                 return TEXT("refused");
	case EClaireonMutationState::AppliedClean:            return TEXT("applied_clean");
	case EClaireonMutationState::AppliedValidationFailed: return TEXT("applied_validation_failed");
	default:                                              return TEXT("applied_operation_failed");
	}
}

const TCHAR* ToWireString(EClaireonEngineCompileStatus Status)
{
	switch (Status)
	{
	case EClaireonEngineCompileStatus::Succeeded: return TEXT("succeeded");
	default:                                      return TEXT("failed");
	}
}

const TCHAR* ToWireString(EClaireonBPExtractionPhase Phase)
{
	switch (Phase)
	{
	case EClaireonBPExtractionPhase::Collapse:            return TEXT("extract_collapse");
	case EClaireonBPExtractionPhase::Rename:              return TEXT("extract_rename");
	case EClaireonBPExtractionPhase::PurityUpdate:        return TEXT("extract_purity_update");
	case EClaireonBPExtractionPhase::ParameterSynthesis:  return TEXT("extract_parameter_synthesis");
	case EClaireonBPExtractionPhase::EntryReconstruction: return TEXT("extract_entry_reconstruction");
	case EClaireonBPExtractionPhase::Wiring:              return TEXT("extract_wiring");
	case EClaireonBPExtractionPhase::CallsiteRefresh:     return TEXT("extract_callsite_refresh");
	default:                                              return TEXT("extract_compile_validate");
	}
}

const TCHAR* ToWireString(EClaireonBPSetterPhase Phase)
{
	switch (Phase)
	{
	case EClaireonBPSetterPhase::MergedStateValidation: return TEXT("setter_merged_state_validation");
	case EClaireonBPSetterPhase::FlagModification:      return TEXT("setter_flag_modification");
	case EClaireonBPSetterPhase::MetadataModification:  return TEXT("setter_metadata_modification");
	case EClaireonBPSetterPhase::EntryReconstruction:   return TEXT("setter_entry_reconstruction");
	case EClaireonBPSetterPhase::CallsiteRefresh:       return TEXT("setter_callsite_refresh");
	default:                                            return TEXT("setter_compile_validate");
	}
}

const TCHAR* ToWireString(EClaireonBPFormatPhase Phase)
{
	switch (Phase)
	{
	case EClaireonBPFormatPhase::Preflight:         return TEXT("format_preflight");
	case EClaireonBPFormatPhase::SelectiveDispatch: return TEXT("format_selective_dispatch");
	case EClaireonBPFormatPhase::Settle:            return TEXT("format_settle");
	default:                                        return TEXT("format_invariant_validation");
	}
}

bool IsErrorState(EClaireonMutationState State)
{
	return State != EClaireonMutationState::AppliedClean;
}

bool RetainsMutation(EClaireonMutationState State)
{
	return State == EClaireonMutationState::AppliedOperationFailed
		|| State == EClaireonMutationState::AppliedValidationFailed;
}

TConstArrayView<const TCHAR*> GetInlineScalarFieldNames()
{
	using namespace ClaireonBPMutationResultInternal;
	return TConstArrayView<const TCHAR*>(
		ClBPMut_InlineScalarFieldNames,
		UE_ARRAY_COUNT(ClBPMut_InlineScalarFieldNames));
}

TSharedPtr<FJsonObject> MakeClaireonTransactionsReport(
	const TArray<FString>& OpenedTitles,
	bool bNestedInActiveGroup)
{
	TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> Titles;
	Titles.Reserve(OpenedTitles.Num());
	for (const FString& Title : OpenedTitles)
	{
		Titles.Add(MakeShared<FJsonValueString>(Title));
	}

	Report->SetNumberField(TEXT("count"), OpenedTitles.Num());
	Report->SetArrayField(TEXT("titles"), Titles);
	Report->SetBoolField(TEXT("nested_in_active_group"), bNestedInActiveGroup);
	Report->SetStringField(TEXT("scope"),
		TEXT("Transactions Claireon opened for this call. NOT a total undo count: editor plugins "
		     "can append their own transactions asynchronously on a later tick, after this call "
		     "has returned, and no delta measured around the call can see them."));
	Report->SetStringField(TEXT("buffer_query"),
		TEXT("transaction_history reads the current buffer, and is ADVISORY only: it is "
		     "editor-wide, with titles and indices but no operation, session or asset id, so it "
		     "cannot prove which entries belong to which call."));

	if (bNestedInActiveGroup)
	{
		Report->SetStringField(TEXT("group_note"),
			TEXT("A transaction opened inside an active transaction group does not reach the undo "
			     "buffer as its own entry; the group's single entry absorbs it."));
	}

	return Report;
}

}

void FClaireonBPMutationResult::WriteInlineScalars(FJsonObject& OutData) const
{
	using namespace ClaireonBPMutationResultInternal;

	OutData.SetStringField(ClBPMut_FieldMutationState, ClaireonBPMutation::ToWireString(MutationState));
	OutData.SetBoolField(ClBPMut_FieldMutationRetained, bMutationRetained);

	if (!FailedPhase.IsEmpty())
	{
		OutData.SetStringField(ClBPMut_FieldFailedPhase, FailedPhase);
	}
	if (!LastCompletedPhase.IsEmpty())
	{
		OutData.SetStringField(ClBPMut_FieldLastCompletedPhase, LastCompletedPhase);
	}
	if (!FailedIslandGuid.IsEmpty())
	{
		OutData.SetStringField(ClBPMut_FieldFailedIslandGuid, FailedIslandGuid);
	}

	if (EngineCompileStatus.IsSet())
	{
		OutData.SetStringField(
			ClBPMut_FieldEngineCompileStatus,
			ClaireonBPMutation::ToWireString(EngineCompileStatus.GetValue()));
	}

	OutData.SetBoolField(ClBPMut_FieldRollbackAvailable, bRollbackAvailable);
	OutData.SetBoolField(ClBPMut_FieldRollbackGroupSafe, bRollbackGroupSafe);
	OutData.SetBoolField(ClBPMut_FieldUndoRecordAvailable, bUndoRecordAvailable);

	if (!AssetPath.IsEmpty())
	{
		OutData.SetStringField(ClBPMut_FieldAssetPath, AssetPath);
	}
	if (!SessionId.IsEmpty())
	{
		OutData.SetStringField(ClBPMut_FieldSessionId, SessionId);
	}
}
