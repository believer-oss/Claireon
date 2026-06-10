// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Base-class scenario tests for FClaireonDeltaApplicatorBase.
// Mock subclass records what the driver called and lets each scenario
// inject failures or override SupportsPhase*() to exercise the
// validate / transaction / rollback / id-map paths.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/FClaireonDeltaApplicatorBase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonDeltaApplicatorBaseTests_anon
{
	class FMockDeltaApplicator : public FClaireonDeltaApplicatorBase
	{
	public:
		// Knobs
		bool bShouldPhase1Disconnect = true;
		bool bShouldPhase4Connect = true;
		bool bFailPhase1 = false;
		bool bFailPhase2 = false;
		bool bFailPhase3 = false;
		bool bFailPhase4 = false;
		bool bFailOpenSession = false;
		bool bFailValidate = false;

		// Observed counts
		int32 Phase1CallCount = 0;
		int32 Phase2CallCount = 0;
		int32 Phase3CallCount = 0;
		int32 Phase4CallCount = 0;
		int32 FinalizeCallCount = 0;
		int32 CloseCallCount = 0;
		int32 Phase3CleanupCallCount = 0;

		// Observed values
		int32 LastPhase1EntryCount = -1;
		int32 LastPhase4EntryCount = -1;

		virtual FString GetFamilyName() const override { return TEXT("mock"); }
		virtual bool SupportsPhase1Disconnect() const override { return bShouldPhase1Disconnect; }
		virtual bool SupportsPhase4Connect() const override { return bShouldPhase4Connect; }

	protected:
		virtual bool ValidateArgs(const TSharedPtr<FJsonObject>& Args, TArray<FString>& OutErrors) override
		{
			if (bFailValidate)
			{
				OutErrors.Add(TEXT("mock validate failure"));
				return false;
			}
			return true;
		}

		virtual bool OpenOrReuseSession(const TSharedPtr<FJsonObject>& Args, FString& OutSessionId, FString& OutError) override
		{
			if (bFailOpenSession)
			{
				OutError = TEXT("mock open session failure");
				return false;
			}
			OutSessionId = TEXT("mock-session");
			return true;
		}

		virtual bool ApplyPhase1_Disconnect(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries) override
		{
			++Phase1CallCount;
			LastPhase1EntryCount = Entries.Num();
			if (bFailPhase1) { AddError(TEXT("mock phase 1 failure")); return false; }
			return true;
		}

		virtual bool ApplyPhase2_Remove(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries) override
		{
			++Phase2CallCount;
			if (bFailPhase2) { AddError(TEXT("mock phase 2 failure")); return false; }
			for (int32 i = 0; i < Entries.Num(); ++i) { MarkRemoved(); }
			return true;
		}

		virtual bool ApplyPhase3_Create(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries) override
		{
			++Phase3CallCount;
			if (bFailPhase3) { AddError(TEXT("mock phase 3 failure")); return false; }
			for (const TSharedPtr<FJsonValue>& E : Entries)
			{
				TSharedPtr<FJsonObject> Obj = E.IsValid() ? E->AsObject() : nullptr;
				if (!Obj.IsValid()) { continue; }
				FString LocalId;
				Obj->TryGetStringField(TEXT("id"), LocalId);
				if (!LocalId.IsEmpty())
				{
					RegisterIdMapping(LocalId, FString::Printf(TEXT("actual-%s"), *LocalId));
				}
				MarkCreated();
			}
			return true;
		}

		virtual bool ApplyPhase4_Connect(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries) override
		{
			++Phase4CallCount;
			LastPhase4EntryCount = Entries.Num();
			if (bFailPhase4) { AddError(TEXT("mock phase 4 failure")); return false; }
			for (const TSharedPtr<FJsonValue>& E : Entries)
			{
				TSharedPtr<FJsonObject> Obj = E.IsValid() ? E->AsObject() : nullptr;
				if (!Obj.IsValid()) { continue; }
				FString FromLocal;
				Obj->TryGetStringField(TEXT("from"), FromLocal);
				const FString Resolved = ResolveLocalId(FromLocal);
				MarkConnection();
				RecordAffected(Resolved);
			}
			return true;
		}

		virtual void FinalizeSession(const FString& SessionId) override { ++FinalizeCallCount; }
		virtual void CloseSessionIfOwned(const FString& SessionId) override { ++CloseCallCount; }
		virtual void Phase3CleanupOnFailure(const FString& SessionId) override { ++Phase3CleanupCallCount; }
	};

	static TSharedPtr<FJsonObject> ArgsWithSessionId()
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), TEXT("inbound-session"));
		return Args;
	}

	static TSharedPtr<FJsonObject> ArgsWithAssetPath()
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), TEXT("/Game/Fake/Asset"));
		return Args;
	}

	static void AddJsonArrayField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, const TArray<TSharedPtr<FJsonValue>>& Vals)
	{
		Obj->SetArrayField(Key, Vals);
	}

	static TSharedPtr<FJsonValue> MakeObjectValue(const TFunction<void(TSharedPtr<FJsonObject>)>& Fill)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Fill(Obj);
		return MakeShared<FJsonValueObject>(Obj);
	}
}

// 1. Empty input is success no-op (AR9).
UNTEST_UNIT(Claireon, DeltaApplicatorBase, EmptyInputIsSuccess)
{
	using namespace ClaireonDeltaApplicatorBaseTests_anon;

	FMockDeltaApplicator Mock;
	auto Result = Mock.ApplyDelta(ArgsWithSessionId(), TEXT("[Mock] Empty"));

	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_EXPECT_EQ(Mock.Phase1CallCount, 1);
	UNTEST_EXPECT_EQ(Mock.Phase2CallCount, 1);
	UNTEST_EXPECT_EQ(Mock.Phase3CallCount, 1);
	UNTEST_EXPECT_EQ(Mock.Phase4CallCount, 1);
	UNTEST_EXPECT_EQ(Mock.FinalizeCallCount, 1);
	UNTEST_EXPECT_EQ(Mock.CloseCallCount, 1);

	int32 RemovedCount = -1, CreatedCount = -1, ConnectionsMade = -1;
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());
	Result.Data->TryGetNumberField(TEXT("removed_count"), RemovedCount);
	Result.Data->TryGetNumberField(TEXT("created_count"), CreatedCount);
	Result.Data->TryGetNumberField(TEXT("connections_made"), ConnectionsMade);
	UNTEST_EXPECT_EQ(RemovedCount, 0);
	UNTEST_EXPECT_EQ(CreatedCount, 0);
	UNTEST_EXPECT_EQ(ConnectionsMade, 0);

	co_return;
}

// 2. Missing both session_id AND asset_path -> validation error.
UNTEST_UNIT(Claireon, DeltaApplicatorBase, MissingSessionAndAssetPath)
{
	using namespace ClaireonDeltaApplicatorBaseTests_anon;

	FMockDeltaApplicator Mock;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Mock.ApplyDelta(Args, TEXT("[Mock] Empty Args"));

	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_EQ(Mock.Phase1CallCount, 0);
	UNTEST_EXPECT_EQ(Mock.Phase2CallCount, 0);

	FString FailedPhase;
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());
	Result.Data->TryGetStringField(TEXT("failed_phase"), FailedPhase);
	UNTEST_EXPECT_EQ(FailedPhase, FString(TEXT("validate")));

	co_return;
}

// 3. Phase-1 failure rolls back; no phase 2/3/4; close called on rollback path.
UNTEST_UNIT(Claireon, DeltaApplicatorBase, Phase1FailureRollsBack)
{
	using namespace ClaireonDeltaApplicatorBaseTests_anon;

	FMockDeltaApplicator Mock;
	Mock.bFailPhase1 = true;

	TSharedPtr<FJsonObject> Args = ArgsWithSessionId();
	TArray<TSharedPtr<FJsonValue>> Disconnect;
	Disconnect.Add(MakeObjectValue([](TSharedPtr<FJsonObject> Obj){ Obj->SetStringField(TEXT("node"), TEXT("dummy")); Obj->SetStringField(TEXT("pin"), TEXT("p")); }));
	AddJsonArrayField(Args, TEXT("disconnect"), Disconnect);

	auto Result = Mock.ApplyDelta(Args, TEXT("[Mock] P1Fail"));

	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_EQ(Mock.Phase1CallCount, 1);
	UNTEST_EXPECT_EQ(Mock.Phase2CallCount, 0);
	UNTEST_EXPECT_EQ(Mock.Phase3CallCount, 0);
	UNTEST_EXPECT_EQ(Mock.Phase4CallCount, 0);
	UNTEST_EXPECT_EQ(Mock.FinalizeCallCount, 0);
	UNTEST_EXPECT_EQ(Mock.CloseCallCount, 1);
	UNTEST_EXPECT_EQ(Mock.Phase3CleanupCallCount, 1);

	FString FailedPhase;
	Result.Data->TryGetStringField(TEXT("failed_phase"), FailedPhase);
	UNTEST_EXPECT_EQ(FailedPhase, FString(TEXT("1")));

	co_return;
}

// 4. Phase-3 failure: phase 4 never runs; cleanup invoked; close called.
UNTEST_UNIT(Claireon, DeltaApplicatorBase, Phase3FailureRollsBack)
{
	using namespace ClaireonDeltaApplicatorBaseTests_anon;

	FMockDeltaApplicator Mock;
	Mock.bFailPhase3 = true;

	TSharedPtr<FJsonObject> Args = ArgsWithSessionId();
	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(MakeObjectValue([](TSharedPtr<FJsonObject> Obj){ Obj->SetStringField(TEXT("id"), TEXT("n1")); }));
	Nodes.Add(MakeObjectValue([](TSharedPtr<FJsonObject> Obj){ Obj->SetStringField(TEXT("id"), TEXT("n2")); }));
	AddJsonArrayField(Args, TEXT("nodes"), Nodes);

	TArray<TSharedPtr<FJsonValue>> Connections;
	Connections.Add(MakeObjectValue([](TSharedPtr<FJsonObject> Obj){ Obj->SetStringField(TEXT("from"), TEXT("n1")); }));
	AddJsonArrayField(Args, TEXT("connections"), Connections);

	auto Result = Mock.ApplyDelta(Args, TEXT("[Mock] P3Fail"));

	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_EQ(Mock.Phase3CallCount, 1);
	UNTEST_EXPECT_EQ(Mock.Phase4CallCount, 0);
	UNTEST_EXPECT_EQ(Mock.Phase3CleanupCallCount, 1);
	UNTEST_EXPECT_EQ(Mock.CloseCallCount, 1);
	UNTEST_EXPECT_EQ(Mock.FinalizeCallCount, 0);

	FString FailedPhase;
	Result.Data->TryGetStringField(TEXT("failed_phase"), FailedPhase);
	UNTEST_EXPECT_EQ(FailedPhase, FString(TEXT("3")));

	co_return;
}

// 5. SupportsPhase1Disconnect()==false rejects non-empty disconnect.
UNTEST_UNIT(Claireon, DeltaApplicatorBase, Phase1UnsupportedRejectsNonEmpty)
{
	using namespace ClaireonDeltaApplicatorBaseTests_anon;

	FMockDeltaApplicator Mock;
	Mock.bShouldPhase1Disconnect = false;

	TSharedPtr<FJsonObject> Args = ArgsWithSessionId();
	TArray<TSharedPtr<FJsonValue>> Disconnect;
	Disconnect.Add(MakeObjectValue([](TSharedPtr<FJsonObject> Obj){ Obj->SetStringField(TEXT("node"), TEXT("x")); }));
	AddJsonArrayField(Args, TEXT("disconnect"), Disconnect);

	auto Result = Mock.ApplyDelta(Args, TEXT("[Mock] P1Unsup"));

	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_EQ(Mock.Phase1CallCount, 0);
	UNTEST_EXPECT_EQ(Mock.CloseCallCount, 0); // never opened a session

	FString FailedPhase;
	Result.Data->TryGetStringField(TEXT("failed_phase"), FailedPhase);
	UNTEST_EXPECT_EQ(FailedPhase, FString(TEXT("validate")));

	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("mock")));

	co_return;
}

// 6. SupportsPhase1Disconnect()==false accepts empty disconnect (AR9).
UNTEST_UNIT(Claireon, DeltaApplicatorBase, Phase1UnsupportedAcceptsEmpty)
{
	using namespace ClaireonDeltaApplicatorBaseTests_anon;

	FMockDeltaApplicator Mock;
	Mock.bShouldPhase1Disconnect = false;

	TSharedPtr<FJsonObject> Args = ArgsWithSessionId();
	TArray<TSharedPtr<FJsonValue>> Empty;
	AddJsonArrayField(Args, TEXT("disconnect"), Empty);

	auto Result = Mock.ApplyDelta(Args, TEXT("[Mock] P1UnsupEmpty"));

	UNTEST_EXPECT_FALSE(Result.bIsError);
	UNTEST_EXPECT_EQ(Mock.Phase1CallCount, 1);
	UNTEST_EXPECT_EQ(Mock.LastPhase1EntryCount, 0);

	co_return;
}

// 7. SupportsPhase4Connect()==false rejects non-empty connections.
UNTEST_UNIT(Claireon, DeltaApplicatorBase, Phase4UnsupportedRejectsNonEmpty)
{
	using namespace ClaireonDeltaApplicatorBaseTests_anon;

	FMockDeltaApplicator Mock;
	Mock.bShouldPhase4Connect = false;

	TSharedPtr<FJsonObject> Args = ArgsWithSessionId();
	TArray<TSharedPtr<FJsonValue>> Connections;
	Connections.Add(MakeObjectValue([](TSharedPtr<FJsonObject> Obj){ Obj->SetStringField(TEXT("from"), TEXT("x")); }));
	AddJsonArrayField(Args, TEXT("connections"), Connections);

	auto Result = Mock.ApplyDelta(Args, TEXT("[Mock] P4Unsup"));

	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_EQ(Mock.Phase4CallCount, 0);

	FString FailedPhase;
	Result.Data->TryGetStringField(TEXT("failed_phase"), FailedPhase);
	UNTEST_EXPECT_EQ(FailedPhase, FString(TEXT("validate")));

	co_return;
}

// 8. SupportsPhase4Connect()==false accepts empty connections (AR9).
UNTEST_UNIT(Claireon, DeltaApplicatorBase, Phase4UnsupportedAcceptsEmpty)
{
	using namespace ClaireonDeltaApplicatorBaseTests_anon;

	FMockDeltaApplicator Mock;
	Mock.bShouldPhase4Connect = false;

	TSharedPtr<FJsonObject> Args = ArgsWithSessionId();
	TArray<TSharedPtr<FJsonValue>> Empty;
	AddJsonArrayField(Args, TEXT("connections"), Empty);

	auto Result = Mock.ApplyDelta(Args, TEXT("[Mock] P4UnsupEmpty"));

	UNTEST_EXPECT_FALSE(Result.bIsError);
	UNTEST_EXPECT_EQ(Mock.Phase4CallCount, 1);
	UNTEST_EXPECT_EQ(Mock.LastPhase4EntryCount, 0);

	co_return;
}

// 9. ID mapping persists from phase 3 to phase 4; unknown ids pass through.
UNTEST_UNIT(Claireon, DeltaApplicatorBase, IdMappingPersistsAcrossPhases)
{
	using namespace ClaireonDeltaApplicatorBaseTests_anon;

	FMockDeltaApplicator Mock;

	TSharedPtr<FJsonObject> Args = ArgsWithSessionId();
	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(MakeObjectValue([](TSharedPtr<FJsonObject> Obj){ Obj->SetStringField(TEXT("id"), TEXT("n1")); }));
	Nodes.Add(MakeObjectValue([](TSharedPtr<FJsonObject> Obj){ Obj->SetStringField(TEXT("id"), TEXT("n2")); }));
	AddJsonArrayField(Args, TEXT("nodes"), Nodes);

	TArray<TSharedPtr<FJsonValue>> Connections;
	Connections.Add(MakeObjectValue([](TSharedPtr<FJsonObject> Obj){ Obj->SetStringField(TEXT("from"), TEXT("n1")); }));
	Connections.Add(MakeObjectValue([](TSharedPtr<FJsonObject> Obj){ Obj->SetStringField(TEXT("from"), TEXT("asset-resident")); }));
	AddJsonArrayField(Args, TEXT("connections"), Connections);

	auto Result = Mock.ApplyDelta(Args, TEXT("[Mock] IdMap"));
	UNTEST_ASSERT_FALSE(Result.bIsError);

	// Response id_map must include n1 / n2 -> actual-*.
	const TSharedPtr<FJsonObject>* IdMapPtr = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("id_map"), IdMapPtr));
	UNTEST_ASSERT_TRUE(IdMapPtr && (*IdMapPtr).IsValid());

	FString N1Resolved, N2Resolved;
	(*IdMapPtr)->TryGetStringField(TEXT("n1"), N1Resolved);
	(*IdMapPtr)->TryGetStringField(TEXT("n2"), N2Resolved);
	UNTEST_EXPECT_EQ(N1Resolved, FString(TEXT("actual-n1")));
	UNTEST_EXPECT_EQ(N2Resolved, FString(TEXT("actual-n2")));

	// affected[] should contain actual-n1 (resolved) and asset-resident (pass-through).
	const TArray<TSharedPtr<FJsonValue>>* Affected = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetArrayField(TEXT("affected"), Affected));
	bool bFoundResolved = false;
	bool bFoundPassthrough = false;
	for (const TSharedPtr<FJsonValue>& Val : *Affected)
	{
		const FString S = Val->AsString();
		if (S == TEXT("actual-n1")) { bFoundResolved = true; }
		if (S == TEXT("asset-resident")) { bFoundPassthrough = true; }
	}
	UNTEST_EXPECT_TRUE(bFoundResolved);
	UNTEST_EXPECT_TRUE(bFoundPassthrough);

	co_return;
}

// 10. Counter increment correctness.
UNTEST_UNIT(Claireon, DeltaApplicatorBase, CountersIncrementCorrectly)
{
	using namespace ClaireonDeltaApplicatorBaseTests_anon;

	FMockDeltaApplicator Mock;

	TSharedPtr<FJsonObject> Args = ArgsWithSessionId();
	TArray<TSharedPtr<FJsonValue>> Remove;
	Remove.Add(MakeObjectValue([](TSharedPtr<FJsonObject>){}));
	AddJsonArrayField(Args, TEXT("remove_nodes"), Remove);

	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(MakeObjectValue([](TSharedPtr<FJsonObject> Obj){ Obj->SetStringField(TEXT("id"), TEXT("a")); }));
	Nodes.Add(MakeObjectValue([](TSharedPtr<FJsonObject> Obj){ Obj->SetStringField(TEXT("id"), TEXT("b")); }));
	AddJsonArrayField(Args, TEXT("nodes"), Nodes);

	TArray<TSharedPtr<FJsonValue>> Connections;
	Connections.Add(MakeObjectValue([](TSharedPtr<FJsonObject> Obj){ Obj->SetStringField(TEXT("from"), TEXT("a")); }));
	Connections.Add(MakeObjectValue([](TSharedPtr<FJsonObject> Obj){ Obj->SetStringField(TEXT("from"), TEXT("b")); }));
	AddJsonArrayField(Args, TEXT("connections"), Connections);

	auto Result = Mock.ApplyDelta(Args, TEXT("[Mock] Counters"));
	UNTEST_ASSERT_FALSE(Result.bIsError);

	int32 Removed = -1, Created = -1, Connections_made = -1;
	Result.Data->TryGetNumberField(TEXT("removed_count"), Removed);
	Result.Data->TryGetNumberField(TEXT("created_count"), Created);
	Result.Data->TryGetNumberField(TEXT("connections_made"), Connections_made);
	UNTEST_EXPECT_EQ(Removed, 1);
	UNTEST_EXPECT_EQ(Created, 2);
	UNTEST_EXPECT_EQ(Connections_made, 2);

	co_return;
}

#endif // WITH_UNTESTED
