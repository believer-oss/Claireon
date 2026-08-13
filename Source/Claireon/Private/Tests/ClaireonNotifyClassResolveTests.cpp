// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// WI-12 regression tests: anim_add_notify must resolve native notify and
// notify-state classes in every spelling (short name, U-prefixed C++ name,
// rooted /Script/ path) and classify state-vs-instant from the RESOLVED class
// rather than from the legacy name heuristic ("State" substring / "ANS_"
// prefix). Uses engine native classes (UAnimNotifyState_TimedParticleEffect,
// UAnimNotify_PlaySound) plus a module-local fixture class whose name defeats
// the heuristic (UClaireonTestFSANSApplyBuff), so nothing depends on game
// modules.
#if WITH_UNTESTED

#include "Untest.h"
#include "UObject/Package.h"
#include "ClaireonNameResolver.h"
#include "ClaireonSessionManager.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "Tools/ClaireonAnimTools_Notify.h"
#include "ClaireonNotifyClassResolveTestTypes.h"

#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimNotifies/AnimNotify_PlaySound.h"
#include "Animation/AnimNotifies/AnimNotifyState_TimedParticleEffect.h"
#include "ClaireonLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonNotifyClassResolveTestsNS
{
	// The two notify base scopes, in the order the tool passes them.
	static TArray<UClass*> WI12NotifyBases()
	{
		TArray<UClass*> Bases;
		Bases.Add(UAnimNotify::StaticClass());
		Bases.Add(UAnimNotifyState::StaticClass());
		return Bases;
	}

	// Synthetic /Game/ lock path for the session-manager lock. The session
	// manager's CanonicalizePath rejects any non-/Game/ path (SessionManager
	// "Session locking only applies to /Game/ assets"), so the transient
	// montage's own /Engine/Transient.* path cannot be used as the lock key.
	// The lock path is only a key: RequireSession looks up ToolData by session
	// id and never cross-checks it against the montage, and ClaireonPathResolver
	// accepts /Game/ paths without requiring an asset on disk (same pattern as
	// the WI-11 fixture in ClaireonMontageLengthTests.cpp). No asset is created
	// at this path.
	inline constexpr const TCHAR* WI12SessionLockPath = TEXT("/Game/__MCPTests/WI12_AnimNotifySession");

	// Transient anim-edit session fixture: transient montage + real session
	// manager session (locked on the synthetic /Game/ path above) +
	// ClaireonAnimEditToolBase::ToolData entry, so
	// ClaireonAnimTool_AddNotify::Execute can be driven exactly as the bridge
	// drives it. Nothing is written under Content; the montage is RF_Transient
	// in the transient package and the session is closed in Close().
	struct FWI12AnimNotifySessionFixture
	{
		UAnimMontage* Anim = nullptr;
		FString SessionId;

		bool Open()
		{
			// Release leftovers from any earlier failed test so a stale lock
			// cannot block this fixture.
			FClaireonSessionManager::Get().ForceReleaseAll();

			Anim = NewObject<UAnimMontage>(GetTransientPackage(), NAME_None, RF_Transient);
			if (!IsValid(Anim))
			{
				UE_LOG(LogClaireon, Error, TEXT("WI12 fixture: failed to create transient AnimMontage"));
				return false;
			}

			ClaireonAnimEditToolBase::EnsureDelegateRegistered();

			const FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
				WI12SessionLockPath, ClaireonAnimEditToolBase::AnimSessionToolName, 5.0);
			if (OpenResult.Result != EOpenSessionResult::Success)
			{
				UE_LOG(LogClaireon, Error, TEXT("WI12 fixture: OpenSession on '%s' failed (Result=%d)"),
					WI12SessionLockPath, static_cast<int32>(OpenResult.Result));
				return false;
			}
			SessionId = OpenResult.SessionId;

			FAnimEditToolData ToolDataEntry;
			ToolDataEntry.Animation = Anim;
			ToolDataEntry.AssetType = TEXT("AnimMontage");
			ClaireonAnimEditToolBase::ToolData.Add(SessionId, ToolDataEntry);
			return true;
		}

		void Close()
		{
			if (!SessionId.IsEmpty())
			{
				// CloseSession fires the session-closed delegate, which removes
				// the ToolData entry; the extra Remove is a belt-and-braces
				// cleanup in case the delegate was never registered.
				FClaireonSessionManager::Get().CloseSession(SessionId);
				ClaireonAnimEditToolBase::ToolData.Remove(SessionId);
				SessionId.Reset();
			}
			Anim = nullptr; // RF_Transient in the transient package; GC reclaims it.
		}
	};

	static IClaireonTool::FToolResult WI12ExecuteAddNotify(const FString& SessionId, const FString& NotifyType)
	{
		ClaireonAnimTool_AddNotify Tool;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("session_id"), SessionId);
		Params->SetStringField(TEXT("notify_type"), NotifyType);
		Params->SetNumberField(TEXT("time"), 0.0);
		Params->SetBoolField(TEXT("suppress_output"), true);
		return Tool.Execute(Params);
	}

	// Sub-object classification of the single notify on the fixture montage.
	// Returns false (with a diagnostic) unless exactly one notify exists and
	// its sub-object shape matches the expectation.
	static bool WI12SingleNotifyMatches(
		const UAnimMontage* Anim,
		bool bExpectState,
		UClass* ExpectedSubObjectClass,
		FString& OutDiagnostic)
	{
		if (!IsValid(Anim))
		{
			OutDiagnostic = TEXT("Anim is null");
			return false;
		}
		if (Anim->Notifies.Num() != 1)
		{
			OutDiagnostic = FString::Printf(TEXT("Expected exactly 1 notify, found %d"), Anim->Notifies.Num());
			return false;
		}
		const FAnimNotifyEvent& Event = Anim->Notifies[0];
		if (bExpectState)
		{
			if (!Event.NotifyStateClass)
			{
				OutDiagnostic = TEXT("Expected a state notify (NotifyStateClass sub-object) but NotifyStateClass is null");
				return false;
			}
			if (Event.Notify)
			{
				OutDiagnostic = TEXT("Expected a state notify but the instant Notify sub-object is also set");
				return false;
			}
			if (IsValid(ExpectedSubObjectClass) && Event.NotifyStateClass->GetClass() != ExpectedSubObjectClass)
			{
				OutDiagnostic = FString::Printf(TEXT("State sub-object class mismatch: expected %s, got %s"),
					*ExpectedSubObjectClass->GetName(), *Event.NotifyStateClass->GetClass()->GetName());
				return false;
			}
		}
		else
		{
			if (!Event.Notify)
			{
				OutDiagnostic = TEXT("Expected an instant notify (Notify sub-object) but Notify is null");
				return false;
			}
			if (Event.NotifyStateClass)
			{
				OutDiagnostic = TEXT("Expected an instant notify but the NotifyStateClass sub-object is also set");
				return false;
			}
			if (IsValid(ExpectedSubObjectClass) && Event.Notify->GetClass() != ExpectedSubObjectClass)
			{
				OutDiagnostic = FString::Printf(TEXT("Instant sub-object class mismatch: expected %s, got %s"),
					*ExpectedSubObjectClass->GetName(), *Event.Notify->GetClass()->GetName());
				return false;
			}
		}
		return true;
	}
}

// ===========================================================================
// Resolver-level: ResolveClassNameMultiBase
// ===========================================================================

// Short name of a native notify-state class resolves and is classified state
// from the resolved class.
UNTEST_UNIT_OPTS(Claireon, NotifyClassResolve, MultiBase_StateShortName, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonNotifyClassResolveTestsNS;
	ClaireonNameResolver::FNameResolveResult Result;
	UClass* Found = ClaireonNameResolver::ResolveClassNameMultiBase(
		TEXT("AnimNotifyState_TimedParticleEffect"), WI12NotifyBases(), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_ASSERT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Found == UAnimNotifyState_TimedParticleEffect::StaticClass());
	UNTEST_EXPECT_TRUE(Found->IsChildOf(UAnimNotifyState::StaticClass()));
	co_return;
}

// Rooted /Script/ path of the same class resolves.
UNTEST_UNIT_OPTS(Claireon, NotifyClassResolve, MultiBase_StateScriptPath, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonNotifyClassResolveTestsNS;
	ClaireonNameResolver::FNameResolveResult Result;
	UClass* Found = ClaireonNameResolver::ResolveClassNameMultiBase(
		TEXT("/Script/Engine.AnimNotifyState_TimedParticleEffect"), WI12NotifyBases(), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_ASSERT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Found == UAnimNotifyState_TimedParticleEffect::StaticClass());
	co_return;
}

// U-prefixed C++ spelling of the same class resolves.
UNTEST_UNIT_OPTS(Claireon, NotifyClassResolve, MultiBase_StateUPrefixedName, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonNotifyClassResolveTestsNS;
	ClaireonNameResolver::FNameResolveResult Result;
	UClass* Found = ClaireonNameResolver::ResolveClassNameMultiBase(
		TEXT("UAnimNotifyState_TimedParticleEffect"), WI12NotifyBases(), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_ASSERT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Found == UAnimNotifyState_TimedParticleEffect::StaticClass());
	co_return;
}

// Short name of a native instant notify resolves and is classified instant.
UNTEST_UNIT_OPTS(Claireon, NotifyClassResolve, MultiBase_InstantShortName, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonNotifyClassResolveTestsNS;
	ClaireonNameResolver::FNameResolveResult Result;
	UClass* Found = ClaireonNameResolver::ResolveClassNameMultiBase(
		TEXT("AnimNotify_PlaySound"), WI12NotifyBases(), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_ASSERT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Found == UAnimNotify_PlaySound::StaticClass());
	UNTEST_EXPECT_TRUE(Found->IsChildOf(UAnimNotify::StaticClass()));
	UNTEST_EXPECT_FALSE(Found->IsChildOf(UAnimNotifyState::StaticClass()));
	co_return;
}

// The core WI-12 defect shape: a native notify-state class whose name has
// neither the "State" substring nor the "ANS_" prefix. The single-base gate
// under UAnimNotify discards it (documented here), and the multi-base retry
// under UAnimNotifyState recovers it.
UNTEST_UNIT_OPTS(Claireon, NotifyClassResolve, MultiBase_CrossBaseRetry_FSANSShapeName, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonNotifyClassResolveTestsNS;

	// Old single-base behavior: the heuristic-chosen UAnimNotify gate discards
	// the class with no cross-base retry.
	ClaireonNameResolver::FNameResolveResult SingleBaseResult;
	UClass* SingleBaseFound = ClaireonNameResolver::ResolveClassName(
		TEXT("ClaireonTestFSANSApplyBuff"), UAnimNotify::StaticClass(), SingleBaseResult);
	UNTEST_EXPECT_TRUE(SingleBaseFound == nullptr);
	UNTEST_EXPECT_FALSE(SingleBaseResult.bSuccess);

	// Multi-base resolution recovers it and classifies it as a state class.
	ClaireonNameResolver::FNameResolveResult Result;
	UClass* Found = ClaireonNameResolver::ResolveClassNameMultiBase(
		TEXT("ClaireonTestFSANSApplyBuff"), WI12NotifyBases(), Result);
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_ASSERT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Found == UClaireonTestFSANSApplyBuff::StaticClass());
	UNTEST_EXPECT_TRUE(Found->IsChildOf(UAnimNotifyState::StaticClass()));
	co_return;
}

// Garbage name fails with an error naming every attempted base scope.
// Generous timeout: an unresolvable name pays the resolver's full
// TObjectIterator<UClass> scan once per base scope, which is slow on a cold
// class table.
UNTEST_UNIT_OPTS(Claireon, NotifyClassResolve, MultiBase_GarbageNameNamesScopes, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonNotifyClassResolveTestsNS;
	ClaireonNameResolver::FNameResolveResult Result;
	UClass* Found = ClaireonNameResolver::ResolveClassNameMultiBase(
		TEXT("Claireon_Bogus_Notify_XYZ"), WI12NotifyBases(), Result);
	UNTEST_EXPECT_TRUE(Found == nullptr);
	UNTEST_EXPECT_FALSE(Result.bSuccess);
	// Exact text owned by ClaireonNameResolver.cpp (ResolveClassNameMultiBase
	// not-found branch); a sync comment there points back here.
	UNTEST_EXPECT_TRUE(Result.Error.Equals(
		TEXT("Class not found: 'Claireon_Bogus_Notify_XYZ' (attempted scopes: AnimNotify, AnimNotifyState)")));
	co_return;
}

// ===========================================================================
// Tool-level: ClaireonAnimTool_AddNotify::Execute
// ===========================================================================

// Short name of a native notify-state class -> notify added with a state
// sub-object of the resolved class.
UNTEST_UNIT_OPTS(Claireon, NotifyClassResolve, AddNotify_StateShortName_CreatesStateSubObject, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonNotifyClassResolveTestsNS;
	FWI12AnimNotifySessionFixture Fixture;
	UNTEST_ASSERT_TRUE(Fixture.Open());

	const IClaireonTool::FToolResult ToolResult =
		WI12ExecuteAddNotify(Fixture.SessionId, TEXT("AnimNotifyState_TimedParticleEffect"));
	FString Diagnostic;
	const bool bMatches = WI12SingleNotifyMatches(
		Fixture.Anim, /*bExpectState=*/true,
		UAnimNotifyState_TimedParticleEffect::StaticClass(), Diagnostic);
	if (!bMatches)
	{
		UE_LOG(LogClaireon, Error, TEXT("AddNotify_StateShortName: %s (tool error: %s)"), *Diagnostic, *ToolResult.ErrorMessage);
	}
	Fixture.Close();

	UNTEST_EXPECT_FALSE(ToolResult.bIsError);
	UNTEST_EXPECT_TRUE(bMatches);
	co_return;
}

// Short name of a native instant notify -> notify added with an instant
// sub-object of the resolved class.
UNTEST_UNIT_OPTS(Claireon, NotifyClassResolve, AddNotify_InstantShortName_CreatesInstantSubObject, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonNotifyClassResolveTestsNS;
	FWI12AnimNotifySessionFixture Fixture;
	UNTEST_ASSERT_TRUE(Fixture.Open());

	const IClaireonTool::FToolResult ToolResult =
		WI12ExecuteAddNotify(Fixture.SessionId, TEXT("AnimNotify_PlaySound"));
	FString Diagnostic;
	const bool bMatches = WI12SingleNotifyMatches(
		Fixture.Anim, /*bExpectState=*/false,
		UAnimNotify_PlaySound::StaticClass(), Diagnostic);
	if (!bMatches)
	{
		UE_LOG(LogClaireon, Error, TEXT("AddNotify_InstantShortName: %s (tool error: %s)"), *Diagnostic, *ToolResult.ErrorMessage);
	}
	Fixture.Close();

	UNTEST_EXPECT_FALSE(ToolResult.bIsError);
	UNTEST_EXPECT_TRUE(bMatches);
	co_return;
}

// The FSANS_ApplyGameplayEffects regression shape end-to-end: a native state
// class whose name defeats the legacy heuristic must still resolve and be
// classified state (with the old code this errored under every spelling).
UNTEST_UNIT_OPTS(Claireon, NotifyClassResolve, AddNotify_FSANSShapeName_CreatesStateSubObject, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonNotifyClassResolveTestsNS;
	FWI12AnimNotifySessionFixture Fixture;
	UNTEST_ASSERT_TRUE(Fixture.Open());

	const IClaireonTool::FToolResult ToolResult =
		WI12ExecuteAddNotify(Fixture.SessionId, TEXT("ClaireonTestFSANSApplyBuff"));
	FString Diagnostic;
	const bool bMatches = WI12SingleNotifyMatches(
		Fixture.Anim, /*bExpectState=*/true,
		UClaireonTestFSANSApplyBuff::StaticClass(), Diagnostic);
	if (!bMatches)
	{
		UE_LOG(LogClaireon, Error, TEXT("AddNotify_FSANSShapeName: %s (tool error: %s)"), *Diagnostic, *ToolResult.ErrorMessage);
	}
	Fixture.Close();

	UNTEST_EXPECT_FALSE(ToolResult.bIsError);
	UNTEST_EXPECT_TRUE(bMatches);
	co_return;
}

// Rooted /Script/ path spelling works through the tool as well.
UNTEST_UNIT_OPTS(Claireon, NotifyClassResolve, AddNotify_ScriptPath_CreatesStateSubObject, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonNotifyClassResolveTestsNS;
	FWI12AnimNotifySessionFixture Fixture;
	UNTEST_ASSERT_TRUE(Fixture.Open());

	const IClaireonTool::FToolResult ToolResult =
		WI12ExecuteAddNotify(Fixture.SessionId, TEXT("/Script/Engine.AnimNotifyState_TimedParticleEffect"));
	FString Diagnostic;
	const bool bMatches = WI12SingleNotifyMatches(
		Fixture.Anim, /*bExpectState=*/true,
		UAnimNotifyState_TimedParticleEffect::StaticClass(), Diagnostic);
	if (!bMatches)
	{
		UE_LOG(LogClaireon, Error, TEXT("AddNotify_ScriptPath: %s (tool error: %s)"), *Diagnostic, *ToolResult.ErrorMessage);
	}
	Fixture.Close();

	UNTEST_EXPECT_FALSE(ToolResult.bIsError);
	UNTEST_EXPECT_TRUE(bMatches);
	co_return;
}

// Garbage class name -> error result whose exact text names both attempted
// scopes and the Blueprint asset-registry fallback; no notify is added.
// Generous timeout: the tool path pays the resolver's full
// TObjectIterator<UClass> scan per base scope PLUS the asset-registry
// Blueprint fallback twice before failing.
UNTEST_UNIT_OPTS(Claireon, NotifyClassResolve, AddNotify_GarbageName_ErrorNamesScopes, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonNotifyClassResolveTestsNS;
	FWI12AnimNotifySessionFixture Fixture;
	UNTEST_ASSERT_TRUE(Fixture.Open());

	const IClaireonTool::FToolResult ToolResult =
		WI12ExecuteAddNotify(Fixture.SessionId, TEXT("Claireon_Bogus_Notify_XYZ"));
	const int32 NotifyCount = IsValid(Fixture.Anim) ? Fixture.Anim->Notifies.Num() : -1;
	Fixture.Close();

	UNTEST_EXPECT_TRUE(ToolResult.bIsError);
	// Exact text owned by Tools/ClaireonAnimTools_Notify.cpp (add_notify
	// could-not-resolve branch); a sync comment there points back here.
	UNTEST_EXPECT_TRUE(ToolResult.ErrorMessage.Equals(
		TEXT("Could not resolve notify class 'Claireon_Bogus_Notify_XYZ'. Attempted scopes: AnimNotify, AnimNotifyState (native classes and Blueprint asset registry). Use a class name like 'AnimNotify_PlaySound' or 'AnimNotifyState_TimedParticleEffect', a U-prefixed C++ name, or a /Script/ path.")));
	UNTEST_EXPECT_TRUE(NotifyCount == 0);
	co_return;
}

#endif // WITH_UNTESTED
