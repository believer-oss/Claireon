// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/FClaireonDeltaApplicator_Niagara.h"
#include "Tools/ClaireonNiagaraEditToolBase.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonDeltaApplicator_Niagara_anon
{
	static bool NiagDelta_TryGetObject(const TSharedPtr<FJsonValue>& Entry, TSharedPtr<FJsonObject>& OutObj)
	{
		if (!Entry.IsValid() || Entry->Type != EJson::Object) { return false; }
		OutObj = Entry->AsObject();
		return OutObj.IsValid();
	}

	// L4 exact string: do not paraphrase, do not whitespace-normalize.
	static const TCHAR* NiagDelta_GetEmitterStubString()
	{
		return TEXT("niagara apply_delta currently supports parameters only; emitter add/remove is deferred (see ApplySpecCatalog.json niagara_edit gotchas, F1 backlog).");
	}
}

bool FClaireonDeltaApplicator_Niagara::ValidateArgs(const TSharedPtr<FJsonObject>& Args, TArray<FString>& OutErrors)
{
	(void)Args; (void)OutErrors;
	return true;
}

bool FClaireonDeltaApplicator_Niagara::OpenOrReuseSession(const TSharedPtr<FJsonObject>& Args, FString& OutSessionId, FString& OutError)
{
	CachedSystem.Reset();
	CreatedParameterNamesThisCall.Reset();

	FString SessionIdArg;
	const bool bHasSessionId = Args->TryGetStringField(TEXT("session_id"), SessionIdArg) && !SessionIdArg.IsEmpty();
	if (bHasSessionId)
	{
		FNiagaraEditToolData* Data = ClaireonNiagaraEditToolBase::ToolData.Find(SessionIdArg);
		if (!Data || !Data->IsValid())
		{
			OutError = FString::Printf(TEXT("niagara_apply_delta: session_id '%s' not found"), *SessionIdArg);
			return false;
		}
		CachedSystem = Data->System;
		OutSessionId = SessionIdArg;
		return true;
	}

	FString AssetPathArg;
	if (!Args->TryGetStringField(TEXT("asset_path"), AssetPathArg) || AssetPathArg.IsEmpty())
	{
		OutError = TEXT("niagara_apply_delta: missing asset_path");
		return false;
	}

	UNiagaraSystem* System = ClaireonNiagaraHelpers::LoadNiagaraSystemAsset(AssetPathArg, OutError);
	if (!System) { return false; }

	ClaireonNiagaraEditToolBase::EnsureDelegateRegistered();

	const FString ResolvedAssetPath = System->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		ResolvedAssetPath, ClaireonNiagaraEditToolBase::NiagaraSessionToolName);
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		OutError = FString::Printf(TEXT("niagara_apply_delta: asset is locked by %s session %s"),
			*Blocker.ToolName, *Blocker.SessionId);
		return false;
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		OutError = FString::Printf(TEXT("niagara_apply_delta: invalid asset path: %s"), *ResolvedAssetPath);
		return false;
	}

	FNiagaraEditToolData NewData;
	NewData.System = System;
	NewData.LastOperationStatus = TEXT("apply_delta opened");
	ClaireonNiagaraEditToolBase::ToolData.Add(OpenResult.SessionId, MoveTemp(NewData));

	CachedSystem = System;
	OutSessionId = OpenResult.SessionId;
	return true;
}

bool FClaireonDeltaApplicator_Niagara::ApplyPhase2_Remove(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_Niagara_anon;
	(void)SessionId;
	UNiagaraSystem* System = CachedSystem.Get();
	if (!System)
	{
		AddError(TEXT("niagara_apply_delta: system is no longer valid"));
		return false;
	}
	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		FString Ref;
		if (Entries[i].IsValid() && Entries[i]->Type == EJson::String)
		{
			Ref = Entries[i]->AsString();
		}
		else
		{
			TSharedPtr<FJsonObject> Obj;
			if (!NiagDelta_TryGetObject(Entries[i], Obj))
			{
				AddError(FString::Printf(TEXT("niagara_apply_delta: remove_nodes[%d] must be a string or object"), i));
				return false;
			}
			Obj->TryGetStringField(TEXT("name"), Ref);
			if (Ref.IsEmpty()) { Obj->TryGetStringField(TEXT("id"), Ref); }
		}
		if (Ref.IsEmpty())
		{
			AddError(FString::Printf(TEXT("niagara_apply_delta: remove_nodes[%d] requires 'name' or 'id'"), i));
			return false;
		}
		// AR6: resolve via IdMap first (local-id from a phase-3 mint in the same call),
		// else treat as a literal User.<Name>. The helper itself normalizes "Color" -> "User.Color".
		const FString Resolved = ResolveLocalId(Ref);
		FString NormalizedName;
		FString OpError;
		if (!ClaireonNiagaraHelpers::RemoveUserParameter(System, Resolved, NormalizedName, OpError))
		{
			AddError(FString::Printf(TEXT("niagara_apply_delta: remove_nodes[%d]: %s"), i, *OpError));
			return false;
		}
		MarkRemoved();
		RecordAffected(NormalizedName);
	}
	return true;
}

bool FClaireonDeltaApplicator_Niagara::ApplyPhase3_Create(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_Niagara_anon;
	(void)SessionId;
	UNiagaraSystem* System = CachedSystem.Get();
	if (!System)
	{
		AddError(TEXT("niagara_apply_delta: system is no longer valid"));
		return false;
	}

	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!NiagDelta_TryGetObject(Entries[i], Obj))
		{
			AddError(FString::Printf(TEXT("niagara_apply_delta: nodes[%d] must be an object"), i));
			return false;
		}

		// L4 emitter-stub guard: any kind other than "parameter" is rejected up-front
		// with the exact L4 string. Empty/absent kind defaults to "parameter".
		FString Kind;
		Obj->TryGetStringField(TEXT("kind"), Kind);
		if (!Kind.IsEmpty() && !Kind.Equals(TEXT("parameter"), ESearchCase::IgnoreCase))
		{
			AddError(NiagDelta_GetEmitterStubString());
			return false;
		}

		FString LocalId, ParamName, TypeStr;
		Obj->TryGetStringField(TEXT("id"), LocalId);
		Obj->TryGetStringField(TEXT("name"), ParamName);
		Obj->TryGetStringField(TEXT("type"), TypeStr);
		if (LocalId.IsEmpty() || ParamName.IsEmpty() || TypeStr.IsEmpty())
		{
			AddError(FString::Printf(TEXT("niagara_apply_delta: nodes[%d] requires 'id', 'name', 'type'"), i));
			return false;
		}

		FNiagaraTypeDefinition TypeDef;
		FString TypeError;
		if (!ClaireonNiagaraHelpers::ResolveUserParameterTypeDef(TypeStr, TypeDef, TypeError))
		{
			AddError(FString::Printf(TEXT("niagara_apply_delta: nodes[%d]: %s"), i, *TypeError));
			return false;
		}

		FString NormalizedName;
		FString OpError;
		if (!ClaireonNiagaraHelpers::AddOrUpdateUserParameter(System, ParamName, TypeDef, NormalizedName, OpError))
		{
			AddError(FString::Printf(TEXT("niagara_apply_delta: nodes[%d]: %s"), i, *OpError));
			return false;
		}
		CreatedParameterNamesThisCall.Add(NormalizedName);
		RegisterIdMapping(LocalId, NormalizedName);
		MarkCreated();
		RecordAffected(NormalizedName);

		// Optional 'value' default: deferred to F1; if provided, record as a warning so callers
		// know the default-write was skipped. The shared helper does not currently expose a
		// per-type default setter -- adding one would diverge from the existing add_parameter
		// behavior. See F1 backlog.
		if (Obj->HasField(TEXT("value")))
		{
			AddWarning(FString::Printf(
				TEXT("niagara_apply_delta: nodes[%d] 'value' default-write is deferred (F1 backlog); parameter created without default"),
				i));
		}
	}
	return true;
}

void FClaireonDeltaApplicator_Niagara::FinalizeSession(const FString& SessionId)
{
	(void)SessionId;
	UNiagaraSystem* System = CachedSystem.Get();
	if (System) { System->MarkPackageDirty(); }
}

void FClaireonDeltaApplicator_Niagara::CloseSessionIfOwned(const FString& SessionId)
{
	if (DoesOwnSession() && !SessionId.IsEmpty())
	{
		ClaireonNiagaraEditToolBase::ToolData.Remove(SessionId);
		FClaireonSessionManager::Get().CloseSession(SessionId);
	}
}

void FClaireonDeltaApplicator_Niagara::Phase3CleanupOnFailure(const FString& SessionId)
{
	(void)SessionId;
	UNiagaraSystem* System = CachedSystem.Get();
	if (!System) { return; }
	for (const FString& Name : CreatedParameterNamesThisCall)
	{
		FString Normalized, RemErr;
		ClaireonNiagaraHelpers::RemoveUserParameter(System, Name, Normalized, RemErr);
	}
	CreatedParameterNamesThisCall.Reset();
}
