// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/FClaireonDeltaApplicator_EQS.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "Tools/ClaireonEQSEditToolBase.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonDeltaApplicator_EQS_anon
{
	static bool EQSDelta_TryGetObject(const TSharedPtr<FJsonValue>& Entry, TSharedPtr<FJsonObject>& OutObj)
	{
		if (!Entry.IsValid() || Entry->Type != EJson::Object) { return false; }
		OutObj = Entry->AsObject();
		return OutObj.IsValid();
	}

	static TArray<UEnvQueryOption*>& EQSDelta_GetOptionsMutable(UEnvQuery* Query)
	{
		static FArrayProperty* OptionsProp = nullptr;
		if (!OptionsProp)
		{
			OptionsProp = CastField<FArrayProperty>(FindFProperty<FProperty>(UEnvQuery::StaticClass(), TEXT("Options")));
		}
		check(OptionsProp);
		return *OptionsProp->ContainerPtrToValuePtr<TArray<UEnvQueryOption*>>(Query);
	}

	static UClass* EQSDelta_ResolveClass(const FString& ClassName, UClass* BaseClass, const FString& BasePrefix, FString& OutError)
	{
		UClass* FoundClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
		if (FoundClass && FoundClass->IsChildOf(BaseClass)) { return FoundClass; }
		const FString Prefixed = BasePrefix + ClassName;
		FoundClass = FindFirstObject<UClass>(*Prefixed, EFindFirstObjectOptions::NativeFirst);
		if (FoundClass && FoundClass->IsChildOf(BaseClass)) { return FoundClass; }
		OutError = FString::Printf(TEXT("eqs_apply_delta: could not resolve class '%s' (expected subclass of %s)"),
			*ClassName, *BaseClass->GetName());
		return nullptr;
	}
}

bool FClaireonDeltaApplicator_EQS::ValidateArgs(const TSharedPtr<FJsonObject>& Args, TArray<FString>& OutErrors)
{
	(void)Args; (void)OutErrors;
	return true;
}

bool FClaireonDeltaApplicator_EQS::OpenOrReuseSession(const TSharedPtr<FJsonObject>& Args, FString& OutSessionId, FString& OutError)
{
	CachedQuery.Reset();
	CreatedOptionsThisCall.Reset();

	FString SessionIdArg;
	const bool bHasSessionId = Args->TryGetStringField(TEXT("session_id"), SessionIdArg) && !SessionIdArg.IsEmpty();
	if (bHasSessionId)
	{
		FEQSEditToolData* Data = ClaireonEQSEditToolBase::ToolData.Find(SessionIdArg);
		if (!Data || !Data->IsValid())
		{
			OutError = FString::Printf(TEXT("eqs_apply_delta: session_id '%s' not found"), *SessionIdArg);
			return false;
		}
		CachedQuery = Data->Query;
		OutSessionId = SessionIdArg;
		return true;
	}

	FString AssetPathArg;
	if (!Args->TryGetStringField(TEXT("asset_path"), AssetPathArg) || AssetPathArg.IsEmpty())
	{
		OutError = TEXT("eqs_apply_delta: missing asset_path");
		return false;
	}

	UEnvQuery* Query = ClaireonBehaviorTreeHelpers::LoadEQSAsset(AssetPathArg, OutError);
	if (!Query) { return false; }

	ClaireonEQSEditToolBase::EnsureDelegateRegistered();

	const FString ResolvedAssetPath = Query->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		ResolvedAssetPath, ClaireonEQSEditToolBase::EQSSessionToolName);
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		OutError = FString::Printf(TEXT("eqs_apply_delta: asset is locked by %s session %s"),
			*Blocker.ToolName, *Blocker.SessionId);
		return false;
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		OutError = FString::Printf(TEXT("eqs_apply_delta: invalid asset path: %s"), *ResolvedAssetPath);
		return false;
	}

	FEQSEditToolData NewData;
	NewData.Query = Query;
	NewData.LastOperationStatus = TEXT("apply_delta opened");
	ClaireonEQSEditToolBase::ToolData.Add(OpenResult.SessionId, MoveTemp(NewData));

	CachedQuery = Query;
	OutSessionId = OpenResult.SessionId;
	return true;
}

bool FClaireonDeltaApplicator_EQS::ApplyPhase2_Remove(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_EQS_anon;
	(void)SessionId;
	UEnvQuery* Query = CachedQuery.Get();
	if (!Query)
	{
		AddError(TEXT("eqs_apply_delta: query is no longer valid"));
		return false;
	}
	Query->Modify();
	TArray<UEnvQueryOption*>& Options = EQSDelta_GetOptionsMutable(Query);
	TArray<int32> ToRemoveDescending;

	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		FString Ref;
		TSharedPtr<FJsonObject> Obj;
		if (Entries[i].IsValid() && Entries[i]->Type == EJson::String)
		{
			Ref = Entries[i]->AsString();
		}
		else if (EQSDelta_TryGetObject(Entries[i], Obj))
		{
			Obj->TryGetStringField(TEXT("id"), Ref);
			if (Ref.IsEmpty()) { Obj->TryGetStringField(TEXT("name"), Ref); }
		}
		if (Ref.IsEmpty())
		{
			AddError(FString::Printf(TEXT("eqs_apply_delta: remove_nodes[%d] requires 'id' or 'name'"), i));
			return false;
		}
		const FString Resolved = ResolveLocalId(Ref);
		int32 Index = INDEX_NONE;
		if (Resolved.IsNumeric()) { Index = FCString::Atoi(*Resolved); }
		if (Index == INDEX_NONE || Index < 0 || Index >= Options.Num())
		{
			AddError(FString::Printf(TEXT("eqs_apply_delta: remove_nodes[%d]: option '%s' not found"), i, *Ref));
			return false;
		}
		ToRemoveDescending.AddUnique(Index);
	}

	ToRemoveDescending.Sort([](int32 A, int32 B) { return A > B; });
	for (int32 Idx : ToRemoveDescending)
	{
		Options.RemoveAt(Idx);
		MarkRemoved();
		RecordAffected(FString::FromInt(Idx));
	}
	Query->MarkPackageDirty();
	return true;
}

bool FClaireonDeltaApplicator_EQS::ApplyPhase3_Create(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_EQS_anon;
	(void)SessionId;
	UEnvQuery* Query = CachedQuery.Get();
	if (!Query)
	{
		AddError(TEXT("eqs_apply_delta: query is no longer valid"));
		return false;
	}
	Query->Modify();
	TArray<UEnvQueryOption*>& Options = EQSDelta_GetOptionsMutable(Query);

	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!EQSDelta_TryGetObject(Entries[i], Obj))
		{
			AddError(FString::Printf(TEXT("eqs_apply_delta: nodes[%d] must be an object"), i));
			return false;
		}
		FString LocalId, Kind;
		Obj->TryGetStringField(TEXT("id"), LocalId);
		Obj->TryGetStringField(TEXT("kind"), Kind);
		if (LocalId.IsEmpty())
		{
			AddError(FString::Printf(TEXT("eqs_apply_delta: nodes[%d] requires 'id'"), i));
			return false;
		}
		if (Kind.IsEmpty()) { Kind = TEXT("option"); }

		if (Kind == TEXT("option"))
		{
			FString GeneratorClassName;
			const TSharedPtr<FJsonObject>* GeneratorObj = nullptr;
			if (Obj->TryGetObjectField(TEXT("generator"), GeneratorObj) && GeneratorObj && (*GeneratorObj).IsValid())
			{
				(*GeneratorObj)->TryGetStringField(TEXT("type"), GeneratorClassName);
				if (GeneratorClassName.IsEmpty())
				{
					(*GeneratorObj)->TryGetStringField(TEXT("class"), GeneratorClassName);
				}
			}
			else
			{
				Obj->TryGetStringField(TEXT("generator_class"), GeneratorClassName);
			}
			if (GeneratorClassName.IsEmpty())
			{
				AddError(FString::Printf(TEXT("eqs_apply_delta: nodes[%d]: option requires 'generator.type'"), i));
				return false;
			}
			FString ResolveError;
			UClass* GeneratorClass = EQSDelta_ResolveClass(GeneratorClassName,
				UEnvQueryGenerator::StaticClass(), TEXT("EnvQueryGenerator_"), ResolveError);
			if (!GeneratorClass)
			{
				AddError(FString::Printf(TEXT("eqs_apply_delta: nodes[%d]: %s"), i, *ResolveError));
				return false;
			}
			UEnvQueryOption* NewOption = NewObject<UEnvQueryOption>(Query);
			NewOption->Generator = NewObject<UEnvQueryGenerator>(NewOption, GeneratorClass);

			const TArray<TSharedPtr<FJsonValue>>* TestsArr = nullptr;
			if (Obj->TryGetArrayField(TEXT("tests"), TestsArr) && TestsArr)
			{
				for (int32 t = 0; t < TestsArr->Num(); ++t)
				{
					TSharedPtr<FJsonObject> TObj;
					if (!EQSDelta_TryGetObject((*TestsArr)[t], TObj)) { continue; }
					FString TestClassName;
					TObj->TryGetStringField(TEXT("type"), TestClassName);
					if (TestClassName.IsEmpty()) { TObj->TryGetStringField(TEXT("class"), TestClassName); }
					if (TestClassName.IsEmpty()) { continue; }
					FString TestResolveError;
					UClass* TestClass = EQSDelta_ResolveClass(TestClassName,
						UEnvQueryTest::StaticClass(), TEXT("EnvQueryTest_"), TestResolveError);
					if (!TestClass)
					{
						AddWarning(FString::Printf(TEXT("eqs_apply_delta: nodes[%d].tests[%d]: %s"), i, t, *TestResolveError));
						continue;
					}
					UEnvQueryTest* NewTest = NewObject<UEnvQueryTest>(NewOption, TestClass);
					NewOption->Tests.Add(NewTest);
				}
			}

			Options.Add(NewOption);
			CreatedOptionsThisCall.Add(NewOption);
			const int32 NewIdx = Options.Num() - 1;
			const FString IdxStr = FString::FromInt(NewIdx);
			RegisterIdMapping(LocalId, IdxStr);
			MarkCreated();
			RecordAffected(IdxStr);
		}
		else if (Kind == TEXT("test"))
		{
			FString OptionRef, TestClassName;
			Obj->TryGetStringField(TEXT("option_id"), OptionRef);
			Obj->TryGetStringField(TEXT("type"), TestClassName);
			if (TestClassName.IsEmpty()) { Obj->TryGetStringField(TEXT("class"), TestClassName); }
			if (OptionRef.IsEmpty() || TestClassName.IsEmpty())
			{
				AddError(FString::Printf(TEXT("eqs_apply_delta: nodes[%d]: 'test' kind requires 'option_id' and 'type'"), i));
				return false;
			}
			const FString ResolvedOpt = ResolveLocalId(OptionRef);
			int32 OptIdx = INDEX_NONE;
			if (ResolvedOpt.IsNumeric()) { OptIdx = FCString::Atoi(*ResolvedOpt); }
			if (OptIdx < 0 || OptIdx >= Options.Num())
			{
				AddError(FString::Printf(TEXT("eqs_apply_delta: nodes[%d]: option_id '%s' not found"), i, *OptionRef));
				return false;
			}
			FString ResolveError;
			UClass* TestClass = EQSDelta_ResolveClass(TestClassName,
				UEnvQueryTest::StaticClass(), TEXT("EnvQueryTest_"), ResolveError);
			if (!TestClass)
			{
				AddError(FString::Printf(TEXT("eqs_apply_delta: nodes[%d]: %s"), i, *ResolveError));
				return false;
			}
			UEnvQueryOption* Option = Options[OptIdx];
			UEnvQueryTest* NewTest = NewObject<UEnvQueryTest>(Option, TestClass);
			Option->Tests.Add(NewTest);
			const FString TestId = FString::Printf(TEXT("%d.tests[%d]"), OptIdx, Option->Tests.Num() - 1);
			RegisterIdMapping(LocalId, TestId);
			MarkCreated();
			RecordAffected(TestId);
		}
		else
		{
			AddError(FString::Printf(TEXT("eqs_apply_delta: nodes[%d]: unknown kind '%s' (expected 'option' or 'test')"), i, *Kind));
			return false;
		}
	}
	Query->MarkPackageDirty();
	return true;
}

void FClaireonDeltaApplicator_EQS::FinalizeSession(const FString& SessionId)
{
	(void)SessionId;
	UEnvQuery* Query = CachedQuery.Get();
	if (Query) { Query->MarkPackageDirty(); }
}

void FClaireonDeltaApplicator_EQS::CloseSessionIfOwned(const FString& SessionId)
{
	if (DoesOwnSession() && !SessionId.IsEmpty())
	{
		ClaireonEQSEditToolBase::ToolData.Remove(SessionId);
		FClaireonSessionManager::Get().CloseSession(SessionId);
	}
}

void FClaireonDeltaApplicator_EQS::Phase3CleanupOnFailure(const FString& SessionId)
{
	using namespace ClaireonDeltaApplicator_EQS_anon;
	(void)SessionId;
	UEnvQuery* Query = CachedQuery.Get();
	if (!Query) { return; }
	TArray<UEnvQueryOption*>& Options = EQSDelta_GetOptionsMutable(Query);
	for (const TWeakObjectPtr<UEnvQueryOption>& Weak : CreatedOptionsThisCall)
	{
		UEnvQueryOption* Opt = Weak.Get();
		if (Opt) { Options.Remove(Opt); }
	}
	CreatedOptionsThisCall.Reset();
}
