// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/FClaireonSpecApplicatorBase.h"
#include "Tools/ClaireonSpecValidator.h"
#include "ClaireonLog.h"
#include "ClaireonSessionManager.h"
#include "ScopedTransaction.h"
#include "Editor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"

IClaireonTool::FToolResult FClaireonSpecApplicatorBase::ApplySpec(
	const TSharedPtr<FJsonObject>& Spec,
	const FString& AssetPath,
	const FString& ExistingSessionId,
	bool bInDryRun)
{
	// 1. Reset accumulated state
	Reset();

	// Stash spec for subclass OpenOrCreateAsset access; clear on any exit path.
	ActiveSpec = Spec;
	ON_SCOPE_EXIT { ActiveSpec.Reset(); };

	UE_LOG(LogClaireon, Log, TEXT("[apply_spec:%s] Starting apply_spec for %s"), *GetToolName(), *AssetPath);

	// 2. Validate spec
	{
		TArray<FString> ValidationErrors;
		FClaireonSpecValidator Validator;
		if (!Validator.Validate(Spec, ValidationErrors))
		{
			FString ErrorMsg = FString::Printf(TEXT("Spec validation failed: %s"), *FString::Join(ValidationErrors, TEXT("; ")));
			UE_LOG(LogClaireon, Error, TEXT("[apply_spec:%s] %s"), *GetToolName(), *ErrorMsg);
			return IClaireonTool::MakeErrorResult(ErrorMsg);
		}

		if (!ValidateToolSpec(Spec, ValidationErrors))
		{
			FString ErrorMsg = FString::Printf(TEXT("Tool-specific validation failed: %s"), *FString::Join(ValidationErrors, TEXT("; ")));
			UE_LOG(LogClaireon, Error, TEXT("[apply_spec:%s] %s"), *GetToolName(), *ErrorMsg);
			return IClaireonTool::MakeErrorResult(ErrorMsg);
		}
	}

	// 3. Open or reuse session
	FString SessionId;
	bool bOwnSession = false;

	if (!ExistingSessionId.IsEmpty())
	{
		// Reuse existing session
		FMCPSession* Session = FClaireonSessionManager::Get().FindSession(ExistingSessionId);
		if (!Session)
		{
			return IClaireonTool::MakeErrorResult(
				FString::Printf(TEXT("Session not found or expired: %s"), *ExistingSessionId));
		}
		SessionId = ExistingSessionId;
		FClaireonSessionManager::Get().TouchSession(SessionId);
		bOwnSession = false;
		UE_LOG(LogClaireon, Log, TEXT("[apply_spec:%s] Reusing session %s"), *GetToolName(), *SessionId);
	}
	else
	{
		// Open new session via tool-specific implementation
		FString OpenError;
		if (!OpenOrCreateAsset(AssetPath, SessionId, OpenError))
		{
			return IClaireonTool::MakeErrorResult(
				FString::Printf(TEXT("Failed to open asset: %s"), *OpenError));
		}
		bOwnSession = true;
		UE_LOG(LogClaireon, Log, TEXT("[apply_spec:%s] Opened session %s for %s"), *GetToolName(), *SessionId, *AssetPath);
	}

	// 4. Begin scoped transaction
	{
		FScopedTransaction Transaction(FText::FromString(
			FString::Printf(TEXT("[Claireon] apply_spec %s"), *GetToolName())));

		// 5. Pass 1: Create entities
		UE_LOG(LogClaireon, Log, TEXT("[apply_spec:%s] Pass 1: Creating entities"), *GetToolName());
		ApplyPass1_CreateEntities(SessionId, Spec);

		if (HasCriticalError())
		{
			UE_LOG(LogClaireon, Error, TEXT("[apply_spec:%s] Critical error after Pass 1, rolling back"), *GetToolName());
			// Undo the scoped transaction
			if (GEditor)
			{
				GEditor->UndoTransaction();
			}
			if (bOwnSession)
			{
				CloseSession(SessionId);
			}
			return BuildResult(false);
		}

		// 6. Pass 2: Wire relationships
		UE_LOG(LogClaireon, Log, TEXT("[apply_spec:%s] Pass 2: Wiring relationships"), *GetToolName());
		ApplyPass2_WireRelationships(SessionId, Spec);

		if (HasCriticalError())
		{
			UE_LOG(LogClaireon, Error, TEXT("[apply_spec:%s] Critical error after Pass 2, rolling back"), *GetToolName());
			if (GEditor)
			{
				GEditor->UndoTransaction();
			}
			if (bOwnSession)
			{
				CloseSession(SessionId);
			}
			return BuildResult(false);
		}

		// 7. Compile asset (skipped under dry_run; compile dirties package state we'll roll back)
		if (!bInDryRun)
		{
			UE_LOG(LogClaireon, Log, TEXT("[apply_spec:%s] Compiling asset"), *GetToolName());
			FString CompileError;
			if (!CompileAsset(SessionId, CompileError))
			{
				AddWarning(FString::Printf(TEXT("Compilation failed: %s"), *CompileError));
			}
		}

		// 8. Save asset (skipped under dry_run; explicit disk write)
		if (!bInDryRun)
		{
			UE_LOG(LogClaireon, Log, TEXT("[apply_spec:%s] Saving asset"), *GetToolName());
			FString SaveError;
			if (!SaveAsset(SessionId, SaveError))
			{
				AddWarning(FString::Printf(TEXT("Save failed: %s"), *SaveError));
			}
		}

		// Under dry_run, undo the scoped transaction explicitly. This rolls
		// back any asset-auto-create, Pass1 entity-creation, and Pass2
		// wiring before the transaction commits, so dry_run leaves zero
		// on-disk state changes.
		if (bInDryRun && GEditor)
		{
			UE_LOG(LogClaireon, Log, TEXT("[apply_spec:%s] dry_run: rolling back transaction"), *GetToolName());
			GEditor->UndoTransaction();
		}
	} // End FScopedTransaction

	// 9. Close session if we opened it
	if (bOwnSession)
	{
		UE_LOG(LogClaireon, Log, TEXT("[apply_spec:%s] Closing session %s"), *GetToolName(), *SessionId);
		CloseSession(SessionId);
	}

	// 10. Return result
	UE_LOG(LogClaireon, Log, TEXT("[apply_spec:%s] Completed successfully"), *GetToolName());
	return BuildResult(true);
}

void FClaireonSpecApplicatorBase::RegisterIdMapping(const FString& SpecId, const FString& ActualId)
{
	IdMap.Add(SpecId, ActualId);
}

FString FClaireonSpecApplicatorBase::ResolveId(const FString& SpecId) const
{
	const FString* Found = IdMap.Find(SpecId);
	return Found ? *Found : FString();
}

const TMap<FString, FString>& FClaireonSpecApplicatorBase::GetIdMappings() const
{
	return IdMap;
}

void FClaireonSpecApplicatorBase::RecordEntrySuccess(const FString& SpecId, const FString& ActualId)
{
	EntryStatuses.Add({SpecId, TEXT("ok"), ActualId, FString()});
}

void FClaireonSpecApplicatorBase::RecordEntryFailure(const FString& SpecId, const FString& Error)
{
	EntryStatuses.Add({SpecId, TEXT("failed"), FString(), Error});
}

void FClaireonSpecApplicatorBase::RecordEntrySkipped(const FString& SpecId, const FString& Reason)
{
	EntryStatuses.Add({SpecId, TEXT("skipped"), FString(), Reason});
}

void FClaireonSpecApplicatorBase::AddWarning(const FString& Warning)
{
	Warnings.Add(Warning);
	UE_LOG(LogClaireon, Warning, TEXT("[apply_spec:%s] %s"), *GetToolName(), *Warning);
}

void FClaireonSpecApplicatorBase::AddError(const FString& Error)
{
	Errors.Add(Error);
	bCriticalError = true;
	UE_LOG(LogClaireon, Error, TEXT("[apply_spec:%s] CRITICAL: %s"), *GetToolName(), *Error);
}

bool FClaireonSpecApplicatorBase::HasCriticalError() const
{
	return bCriticalError;
}

bool FClaireonSpecApplicatorBase::IsIdCreated(const FString& SpecId) const
{
	return IdMap.Contains(SpecId);
}

IClaireonTool::FToolResult FClaireonSpecApplicatorBase::BuildResult(bool bSuccess) const
{
	IClaireonTool::FToolResult Result;
	Result.bIsError = !bSuccess;

	// Build result data JSON
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	// ID mappings
	TSharedPtr<FJsonObject> Mappings = MakeShared<FJsonObject>();
	for (const auto& Pair : IdMap)
	{
		Mappings->SetStringField(Pair.Key, Pair.Value);
	}
	Data->SetObjectField(TEXT("id_mappings"), Mappings);

	// Per-entry status array (resolution L3)
	TArray<TSharedPtr<FJsonValue>> StatusArray;
	for (const auto& Entry : EntryStatuses)
	{
		TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
		EntryObj->SetStringField(TEXT("spec_id"), Entry.SpecId);
		EntryObj->SetStringField(TEXT("status"), Entry.Status);
		if (!Entry.ActualId.IsEmpty())
		{
			EntryObj->SetStringField(TEXT("actual_id"), Entry.ActualId);
		}
		if (!Entry.Error.IsEmpty())
		{
			EntryObj->SetStringField(TEXT("error"), Entry.Error);
		}
		StatusArray.Add(MakeShared<FJsonValueObject>(EntryObj));
	}
	Data->SetArrayField(TEXT("entries"), StatusArray);

	Result.Data = Data;
	Result.Warnings = Warnings;

	// Summary
	int32 OkCount = 0, FailCount = 0, SkipCount = 0;
	for (const auto& E : EntryStatuses)
	{
		if (E.Status == TEXT("ok")) OkCount++;
		else if (E.Status == TEXT("failed")) FailCount++;
		else if (E.Status == TEXT("skipped")) SkipCount++;
	}

	if (bSuccess)
	{
		Result.Summary = FString::Printf(
			TEXT("apply_spec completed: %d ok, %d failed, %d skipped"),
			OkCount, FailCount, SkipCount);
	}
	else
	{
		Result.ErrorMessage = FString::Printf(
			TEXT("apply_spec failed: %d ok, %d failed, %d skipped. Errors: %s"),
			OkCount, FailCount, SkipCount,
			*FString::Join(Errors, TEXT("; ")));
	}

	return Result;
}

void FClaireonSpecApplicatorBase::Reset()
{
	IdMap.Empty();
	EntryStatuses.Empty();
	Warnings.Empty();
	Errors.Empty();
	bCriticalError = false;
	ActiveSpec.Reset();
}
