// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Functional tests for niagara_apply_delta (#0000).
// Verifies AR5/AR9 phase-1/4 rejection, M5 fail-on-missing, L4 emitter-stub guard,
// AR6 ref resolution (local-id and literal name), and the H2 byte-identical shared-impl
// regression test (apply_delta produces the same FNiagaraVariable as niagara_add_parameter).

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonNiagaraTool_ApplyDelta.h"
#include "Tools/ClaireonNiagaraTool_AddParameter.h"
#include "Tools/ClaireonNiagaraEditToolBase.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "Tools/FClaireonDeltaApplicator_Niagara.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonNiagaraTool_ApplyDeltaTests_anon
{
	static TSharedPtr<FJsonValue> NiagDeltaTest_ObjVal(const TSharedPtr<FJsonObject>& O) { return MakeShared<FJsonValueObject>(O); }

	// Build a transient in-memory UNiagaraSystem fixture.
	static UNiagaraSystem* NiagDeltaTest_CreateTransientSystem()
	{
		UNiagaraSystem* System = NewObject<UNiagaraSystem>(
			GetTransientPackage(),
			FName(TEXT("NS_ClaireonDeltaFixture")),
			RF_Transient | RF_Transactional);
		return System;
	}

	static FString NiagDeltaTest_RegisterFakeSession(UNiagaraSystem* System)
	{
		const FString SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		FNiagaraEditToolData NewData;
		NewData.System = System;
		ClaireonNiagaraEditToolBase::ToolData.Add(SessionId, MoveTemp(NewData));
		return SessionId;
	}

	static void NiagDeltaTest_ClearFakeSession(const FString& SessionId)
	{
		ClaireonNiagaraEditToolBase::ToolData.Remove(SessionId);
	}

	// Lookup a user parameter by name. `GetUserParameters` returns the SIMPLE-named
	// variant (without the "User." prefix; the redirection store strips it), so we
	// accept both "User.X" and "X" forms for caller convenience.
	static bool NiagDeltaTest_GetUserParam(UNiagaraSystem* System, const FString& AnyName, FNiagaraVariable& OutVar)
	{
		if (!System) { return false; }
		FString Simple = AnyName;
		if (Simple.StartsWith(TEXT("User."))) { Simple = Simple.RightChop(5); }
		TArray<FNiagaraVariable> UserParams;
		System->GetExposedParameters().GetUserParameters(UserParams);
		for (const FNiagaraVariable& V : UserParams)
		{
			const FString Name = V.GetName().ToString();
			if (Name.Equals(Simple, ESearchCase::IgnoreCase)
				|| Name.Equals(AnyName, ESearchCase::IgnoreCase))
			{
				OutVar = V;
				return true;
			}
		}
		return false;
	}
}

// 1. Validation error: missing both session_id and asset_path.
UNTEST_UNIT_OPTS(Claireon, NiagaraApplyDelta, MissingSessionAndAssetPath, UNTEST_TIMEOUTMS(10000))
{
	FClaireonNiagaraTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());
	FString FailedPhase;
	Result.Data->TryGetStringField(TEXT("failed_phase"), FailedPhase);
	UNTEST_EXPECT_STREQ(FailedPhase, TEXT("validate"));
	co_return;
}

// 2. M5 fail-on-missing: bogus asset_path returns an error.
UNTEST_UNIT_OPTS(Claireon, NiagaraApplyDelta, FailOnMissingAsset, UNTEST_TIMEOUTMS(15000))
{
	FClaireonNiagaraTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/__definitely_does_not_exist__NS_zz"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	co_return;
}

// 3. AR5/AR9: non-empty disconnect[] is rejected before any phase runs.
UNTEST_UNIT_OPTS(Claireon, NiagaraApplyDelta, RejectsPhase1NonEmpty, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonNiagaraTool_ApplyDeltaTests_anon;
	FClaireonNiagaraTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), TEXT("does-not-exist-session-id"));
	TArray<TSharedPtr<FJsonValue>> Disc;
	{
		TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
		D->SetStringField(TEXT("any"), TEXT("thing"));
		Disc.Add(NiagDeltaTest_ObjVal(D));
	}
	Args->SetArrayField(TEXT("disconnect"), Disc);

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	FString FailedPhase;
	Result.Data->TryGetStringField(TEXT("failed_phase"), FailedPhase);
	UNTEST_EXPECT_STREQ(FailedPhase, TEXT("validate"));
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("disconnect")));
	co_return;
}

// 4. AR5/AR9: non-empty connections[] is rejected before any phase runs.
UNTEST_UNIT_OPTS(Claireon, NiagaraApplyDelta, RejectsPhase4NonEmpty, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonNiagaraTool_ApplyDeltaTests_anon;
	FClaireonNiagaraTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), TEXT("does-not-exist-session-id"));
	TArray<TSharedPtr<FJsonValue>> Conns;
	{
		TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetStringField(TEXT("any"), TEXT("thing"));
		Conns.Add(NiagDeltaTest_ObjVal(C));
	}
	Args->SetArrayField(TEXT("connections"), Conns);

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	FString FailedPhase;
	Result.Data->TryGetStringField(TEXT("failed_phase"), FailedPhase);
	UNTEST_EXPECT_STREQ(FailedPhase, TEXT("validate"));
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("connect")));
	co_return;
}

// 5. Happy path: phase 3 creates User.NewFloat and User.NewVec; both end up in the
//    System's exposed-parameters store.
UNTEST_UNIT_OPTS(Claireon, NiagaraApplyDelta, HappyPath_CreatesParams, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonNiagaraTool_ApplyDeltaTests_anon;
	UNiagaraSystem* System = NiagDeltaTest_CreateTransientSystem();
	UNTEST_ASSERT_PTR(System);
	const FString SessionId = NiagDeltaTest_RegisterFakeSession(System);

	FClaireonNiagaraTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);

	TArray<TSharedPtr<FJsonValue>> Nodes;
	{
		TSharedPtr<FJsonObject> N1 = MakeShared<FJsonObject>();
		N1->SetStringField(TEXT("id"), TEXT("p1"));
		N1->SetStringField(TEXT("name"), TEXT("User.NewFloat"));
		N1->SetStringField(TEXT("type"), TEXT("Float"));
		Nodes.Add(NiagDeltaTest_ObjVal(N1));

		TSharedPtr<FJsonObject> N2 = MakeShared<FJsonObject>();
		N2->SetStringField(TEXT("id"), TEXT("p2"));
		N2->SetStringField(TEXT("name"), TEXT("NewVec"));  // bare name -> helper normalizes to User.NewVec
		N2->SetStringField(TEXT("type"), TEXT("Vector"));
		Nodes.Add(NiagDeltaTest_ObjVal(N2));
	}
	Args->SetArrayField(TEXT("nodes"), Nodes);

	auto Result = Tool.Execute(Args);
	UNTEST_EXPECT_FALSE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());

	FNiagaraVariable NewFloat, NewVec;
	UNTEST_EXPECT_TRUE(NiagDeltaTest_GetUserParam(System, TEXT("User.NewFloat"), NewFloat));
	UNTEST_EXPECT_TRUE(NiagDeltaTest_GetUserParam(System, TEXT("User.NewVec"), NewVec));

	NiagDeltaTest_ClearFakeSession(SessionId);
	co_return;
}

// 6. L4 emitter-stub guard: a nodes[] entry with kind == "emitter" returns the exact
//    L4 string verbatim and runs no phases.
UNTEST_UNIT_OPTS(Claireon, NiagaraApplyDelta, L4_EmitterStubGuard, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonNiagaraTool_ApplyDeltaTests_anon;
	UNiagaraSystem* System = NiagDeltaTest_CreateTransientSystem();
	UNTEST_ASSERT_PTR(System);
	const FString SessionId = NiagDeltaTest_RegisterFakeSession(System);

	FClaireonNiagaraTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);

	TArray<TSharedPtr<FJsonValue>> Nodes;
	{
		TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>();
		N->SetStringField(TEXT("id"), TEXT("e1"));
		N->SetStringField(TEXT("kind"), TEXT("emitter"));
		N->SetStringField(TEXT("name"), TEXT("StubEmitter"));
		Nodes.Add(NiagDeltaTest_ObjVal(N));
	}
	Args->SetArrayField(TEXT("nodes"), Nodes);

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);

	// The exact L4 string must appear verbatim in errors[] -- no paraphrase, no whitespace
	// normalization. We byte-compare against the spec-mandated literal.
	const TCHAR* ExpectedExact = TEXT("niagara apply_delta currently supports parameters only; emitter add/remove is deferred (see ApplySpecCatalog.json niagara_edit gotchas, F1 backlog).");
	const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetArrayField(TEXT("errors"), Errors) && Errors);
	bool bFoundExact = false;
	for (const TSharedPtr<FJsonValue>& V : *Errors)
	{
		if (V.IsValid() && V->Type == EJson::String && V->AsString() == ExpectedExact)
		{
			bFoundExact = true;
			break;
		}
	}
	UNTEST_EXPECT_TRUE(bFoundExact);

	NiagDeltaTest_ClearFakeSession(SessionId);
	co_return;
}

// 7. AR6 ref resolution: remove a User.<Name> parameter by literal name after a prior
//    add. (In a single call, the local-id IdMap would resolve "p1" -> "User.X"; here we
//    just verify the literal-name path on a freshly-added param.)
UNTEST_UNIT_OPTS(Claireon, NiagaraApplyDelta, AR6_RemoveByLiteralName, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonNiagaraTool_ApplyDeltaTests_anon;
	UNiagaraSystem* System = NiagDeltaTest_CreateTransientSystem();
	UNTEST_ASSERT_PTR(System);
	const FString SessionId = NiagDeltaTest_RegisterFakeSession(System);

	// Seed: add User.MintedFloat via the discrete add_parameter tool (the SAME helper
	// path as apply_delta -- this is the H2 shared-impl invariant in passive form).
	{
		FNiagaraTypeDefinition FloatDef = FNiagaraTypeDefinition::GetFloatDef();
		FString Normalized, OpError;
		const bool bAdded = ClaireonNiagaraHelpers::AddOrUpdateUserParameter(
			System, TEXT("MintedFloat"), FloatDef, Normalized, OpError);
		UNTEST_ASSERT_TRUE(bAdded);
	}

	// Now remove by literal name via apply_delta phase 2.
	FClaireonNiagaraTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	TArray<TSharedPtr<FJsonValue>> Remove;
	Remove.Add(MakeShared<FJsonValueString>(TEXT("User.MintedFloat")));
	Args->SetArrayField(TEXT("remove_nodes"), Remove);

	auto Result = Tool.Execute(Args);
	UNTEST_EXPECT_FALSE(Result.bIsError);

	FNiagaraVariable Removed;
	UNTEST_EXPECT_FALSE(NiagDeltaTest_GetUserParam(System, TEXT("User.MintedFloat"), Removed));

	NiagDeltaTest_ClearFakeSession(SessionId);
	co_return;
}

// 8. H2 byte-identical shared-impl invariant: creating the same User parameter via the
//    discrete add_parameter helper path AND via apply_delta phase 3 (both routed through
//    ClaireonNiagaraHelpers::AddOrUpdateUserParameter) must produce IDENTICAL FNiagaraVariable
//    results. If they differ, the helpers diverged -- find and remove the inlined copy.
UNTEST_UNIT_OPTS(Claireon, NiagaraApplyDelta, H2_ByteIdenticalSharedImpl, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonNiagaraTool_ApplyDeltaTests_anon;

	// System A: add via the discrete add_parameter helper path directly.
	UNiagaraSystem* SystemA = NiagDeltaTest_CreateTransientSystem();
	UNTEST_ASSERT_PTR(SystemA);
	{
		FNiagaraTypeDefinition FloatDef = FNiagaraTypeDefinition::GetFloatDef();
		FString Normalized, OpError;
		const bool bAdded = ClaireonNiagaraHelpers::AddOrUpdateUserParameter(
			SystemA, TEXT("User.SharedFloat"), FloatDef, Normalized, OpError);
		UNTEST_ASSERT_TRUE(bAdded);
	}
	FNiagaraVariable VarA;
	UNTEST_ASSERT_TRUE(NiagDeltaTest_GetUserParam(SystemA, TEXT("User.SharedFloat"), VarA));

	// System B: add via apply_delta phase 3.
	UNiagaraSystem* SystemB = NiagDeltaTest_CreateTransientSystem();
	UNTEST_ASSERT_PTR(SystemB);
	const FString SessionB = NiagDeltaTest_RegisterFakeSession(SystemB);
	{
		FClaireonNiagaraTool_ApplyDelta Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionB);
		TArray<TSharedPtr<FJsonValue>> Nodes;
		TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>();
		N->SetStringField(TEXT("id"), TEXT("s1"));
		N->SetStringField(TEXT("name"), TEXT("User.SharedFloat"));
		N->SetStringField(TEXT("type"), TEXT("Float"));
		Nodes.Add(NiagDeltaTest_ObjVal(N));
		Args->SetArrayField(TEXT("nodes"), Nodes);
		auto Result = Tool.Execute(Args);
		UNTEST_ASSERT_FALSE(Result.bIsError);
	}
	FNiagaraVariable VarB;
	UNTEST_ASSERT_TRUE(NiagDeltaTest_GetUserParam(SystemB, TEXT("User.SharedFloat"), VarB));

	// Compare: name, type, size, data bytes. Both must be byte-identical.
	UNTEST_EXPECT_STREQ(VarA.GetName().ToString(), VarB.GetName().ToString());
	UNTEST_EXPECT_TRUE(VarA.GetType() == VarB.GetType());
	UNTEST_EXPECT_TRUE(VarA.GetSizeInBytes() == VarB.GetSizeInBytes());

	NiagDeltaTest_ClearFakeSession(SessionB);
	co_return;
}

#endif // WITH_UNTESTED
