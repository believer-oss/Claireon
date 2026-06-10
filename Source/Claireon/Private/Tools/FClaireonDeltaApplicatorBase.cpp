// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/FClaireonDeltaApplicatorBase.h"
#include "ScopedTransaction.h"
#include "Misc/ScopeExit.h"

using FToolResult = IClaireonTool::FToolResult;

namespace FClaireonDeltaApplicatorBase_Internal
{
	static const TArray<TSharedPtr<FJsonValue>>& GetEmptyJsonArray()
	{
		static const TArray<TSharedPtr<FJsonValue>> Empty;
		return Empty;
	}

	static FString PhaseLabel(int32 Phase)
	{
		switch (Phase)
		{
			case 1: return TEXT("1");
			case 2: return TEXT("2");
			case 3: return TEXT("3");
			case 4: return TEXT("4");
			default: return TEXT("unknown");
		}
	}

	static FString JoinPhases(bool bSupportsPhase1, bool bSupportsPhase4)
	{
		TArray<FString> Phases;
		if (bSupportsPhase1) { Phases.Add(TEXT("disconnect")); }
		Phases.Add(TEXT("remove_nodes"));
		Phases.Add(TEXT("nodes"));
		if (bSupportsPhase4) { Phases.Add(TEXT("connect")); }
		return FString::Join(Phases, TEXT(", "));
	}

	static TSharedPtr<FJsonObject> MakeIdMapJson(const TMap<FString, FString>& IdMap)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : IdMap)
		{
			Obj->SetStringField(Pair.Key, Pair.Value);
		}
		return Obj;
	}

	static TArray<TSharedPtr<FJsonValue>> MakeStringArrayJson(const TArray<FString>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(In.Num());
		for (const FString& S : In)
		{
			Out.Add(MakeShared<FJsonValueString>(S));
		}
		return Out;
	}
}

void FClaireonDeltaApplicatorBase::RegisterIdMapping(const FString& LocalId, const FString& ActualId)
{
	IdMap.Add(LocalId, ActualId);
}

FString FClaireonDeltaApplicatorBase::ResolveLocalId(const FString& LocalId) const
{
	if (const FString* Found = IdMap.Find(LocalId))
	{
		return *Found;
	}
	return LocalId;
}

void FClaireonDeltaApplicatorBase::RecordAffected(const FString& EntityId)
{
	AffectedEntities.AddUnique(EntityId);
}

void FClaireonDeltaApplicatorBase::AddWarning(const FString& W)
{
	Warnings.Add(W);
}

void FClaireonDeltaApplicatorBase::AddError(const FString& E)
{
	Errors.Add(E);
}

FToolResult FClaireonDeltaApplicatorBase::ApplyDelta(
	const TSharedPtr<FJsonObject>& Args,
	const FString& OperationLabel)
{
	using namespace FClaireonDeltaApplicatorBase_Internal;

	// 0. Entry setup: reset all shared state.
	IdMap.Reset();
	AffectedEntities.Reset();
	Warnings.Reset();
	Errors.Reset();
	bCriticalError = false;
	bOwnsSession = false;
	RemovedCount = 0;
	CreatedCount = 0;
	ConnectionsMade = 0;
	CachedArgs = Args;

	// Always clear CachedArgs on any return path.
	ON_SCOPE_EXIT { CachedArgs.Reset(); };

	const FString FamilyName = GetFamilyName();

	auto MakeErrorResultWithDetails = [&](const FString& FailedPhase, const FString& PrimaryError) -> FToolResult
	{
		FToolResult R;
		R.bIsError = true;
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("family"), FamilyName);
		Data->SetStringField(TEXT("status"), TEXT("error"));
		Data->SetStringField(TEXT("failed_phase"), FailedPhase);
		Data->SetArrayField(TEXT("errors"), MakeStringArrayJson(Errors));
		Data->SetArrayField(TEXT("warnings"), MakeStringArrayJson(Warnings));
		R.Data = Data;
		R.ErrorMessage = PrimaryError;
		return R;
	};

	if (!Args.IsValid())
	{
		AddError(TEXT("Args object is null"));
		return MakeErrorResultWithDetails(TEXT("validate"), TEXT("Args object is null"));
	}

	// 1. Pre-flight validation.
	FString SessionIdArg;
	FString AssetPathArg;
	const bool bHasSessionId = Args->TryGetStringField(TEXT("session_id"), SessionIdArg) && !SessionIdArg.IsEmpty();
	const bool bHasAssetPath = Args->TryGetStringField(TEXT("asset_path"), AssetPathArg) && !AssetPathArg.IsEmpty();

	if (bHasSessionId == bHasAssetPath)
	{
		const FString Msg = bHasSessionId
			? FString::Printf(TEXT("%s_apply_delta: provide exactly one of 'session_id' or 'asset_path', not both"), *FamilyName)
			: FString::Printf(TEXT("%s_apply_delta: provide exactly one of 'session_id' or 'asset_path'"), *FamilyName);
		AddError(Msg);
		return MakeErrorResultWithDetails(TEXT("validate"), Msg);
	}

	const bool bPhase1Supported = SupportsPhase1Disconnect();
	const bool bPhase4Supported = SupportsPhase4Connect();

	auto ExtractPhaseArray = [&](const FString& Key, const TArray<TSharedPtr<FJsonValue>>*& OutArr) -> bool
	{
		OutArr = nullptr;
		return Args->TryGetArrayField(Key, OutArr) && OutArr != nullptr;
	};

	const TArray<TSharedPtr<FJsonValue>>* DisconnectArr = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* RemoveArr = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* NodesArr = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* ConnectionsArr = nullptr;
	ExtractPhaseArray(TEXT("disconnect"), DisconnectArr);
	ExtractPhaseArray(TEXT("remove_nodes"), RemoveArr);
	ExtractPhaseArray(TEXT("nodes"), NodesArr);
	ExtractPhaseArray(TEXT("connections"), ConnectionsArr);

	if (!bPhase1Supported && DisconnectArr && DisconnectArr->Num() > 0)
	{
		const FString Msg = FString::Printf(
			TEXT("%s_apply_delta does not support 'disconnect' phase; supported phases: %s"),
			*FamilyName, *JoinPhases(bPhase1Supported, bPhase4Supported));
		AddError(Msg);
		return MakeErrorResultWithDetails(TEXT("validate"), Msg);
	}
	if (!bPhase4Supported && ConnectionsArr && ConnectionsArr->Num() > 0)
	{
		const FString Msg = FString::Printf(
			TEXT("%s_apply_delta does not support 'connect' phase; supported phases: %s"),
			*FamilyName, *JoinPhases(bPhase1Supported, bPhase4Supported));
		AddError(Msg);
		return MakeErrorResultWithDetails(TEXT("validate"), Msg);
	}

	// Family-specific validation.
	{
		TArray<FString> ValidationErrors;
		if (!ValidateArgs(Args, ValidationErrors))
		{
			for (const FString& E : ValidationErrors) { AddError(E); }
			const FString Primary = (ValidationErrors.Num() > 0)
				? ValidationErrors[0]
				: FString::Printf(TEXT("%s_apply_delta: validation failed"), *FamilyName);
			return MakeErrorResultWithDetails(TEXT("validate"), Primary);
		}
	}

	// 2. Open or reuse session.
	bOwnsSession = bHasAssetPath;
	FString SessionId;
	{
		FString OpenError;
		if (!OpenOrReuseSession(Args, SessionId, OpenError))
		{
			if (!OpenError.IsEmpty()) { AddError(OpenError); }
			return MakeErrorResultWithDetails(TEXT("validate"), OpenError.IsEmpty() ? FString::Printf(TEXT("%s_apply_delta: failed to open session"), *FamilyName) : OpenError);
		}
	}

	// 3. Open transaction.
	FScopedTransaction Transaction(FText::FromString(OperationLabel));

	// 4. Phase dispatch. The driver always invokes the phase callback
	// (even with an empty array) so subclasses see all four phases
	// uniformly. Validation above already rejected non-empty arrays
	// for unsupported phases (AR5); for unsupported phases reaching
	// this point, the array is guaranteed empty.
	auto RunPhase = [&](int32 PhaseNum, bool (FClaireonDeltaApplicatorBase::*Phase)(const FString&, const TArray<TSharedPtr<FJsonValue>>&), const TArray<TSharedPtr<FJsonValue>>* Arr) -> bool
	{
		const TArray<TSharedPtr<FJsonValue>>& Entries = Arr ? *Arr : GetEmptyJsonArray();
		const bool bPhaseOk = (this->*Phase)(SessionId, Entries);
		if (!bPhaseOk || HasCriticalError())
		{
			return false;
		}
		return true;
	};

	auto HandleFailure = [&](int32 PhaseNum) -> FToolResult
	{
		Phase3CleanupOnFailure(SessionId);
		Transaction.Cancel();
		CloseSessionIfOwned(SessionId);
		const FString Primary = (Errors.Num() > 0)
			? Errors[0]
			: FString::Printf(TEXT("%s_apply_delta: phase %d failed"), *FamilyName, PhaseNum);
		return MakeErrorResultWithDetails(PhaseLabel(PhaseNum), Primary);
	};

	if (!RunPhase(1, &FClaireonDeltaApplicatorBase::ApplyPhase1_Disconnect, DisconnectArr))
	{
		return HandleFailure(1);
	}
	if (!RunPhase(2, &FClaireonDeltaApplicatorBase::ApplyPhase2_Remove, RemoveArr))
	{
		return HandleFailure(2);
	}
	if (!RunPhase(3, &FClaireonDeltaApplicatorBase::ApplyPhase3_Create, NodesArr))
	{
		return HandleFailure(3);
	}
	if (!RunPhase(4, &FClaireonDeltaApplicatorBase::ApplyPhase4_Connect, ConnectionsArr))
	{
		return HandleFailure(4);
	}

	// 5. Finalize.
	FinalizeSession(SessionId);

	// 6. Close session if owned.
	CloseSessionIfOwned(SessionId);

	// 7. Build success result.
	FToolResult Result;
	Result.bIsError = false;
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("family"), FamilyName);
	Data->SetStringField(TEXT("session_id"), SessionId);
	Data->SetStringField(TEXT("status"), TEXT("ok"));
	Data->SetNumberField(TEXT("removed_count"), RemovedCount);
	Data->SetNumberField(TEXT("created_count"), CreatedCount);
	Data->SetNumberField(TEXT("connections_made"), ConnectionsMade);
	Data->SetObjectField(TEXT("id_map"), MakeIdMapJson(IdMap));
	Data->SetArrayField(TEXT("affected"), MakeStringArrayJson(AffectedEntities));
	Data->SetArrayField(TEXT("warnings"), MakeStringArrayJson(Warnings));
	Result.Data = Data;
	Result.Summary = FString::Printf(
		TEXT("%s_apply_delta: %d removed, %d created, %d connections (session %s)"),
		*FamilyName, RemovedCount, CreatedCount, ConnectionsMade, *SessionId);
	return Result;
}
