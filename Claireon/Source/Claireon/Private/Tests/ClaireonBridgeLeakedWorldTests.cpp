// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonBridge.h"
#include "ClaireonLog.h"

#include "Editor.h"
#include "Engine/World.h"
#include "UObject/Package.h"
#include "PackageTools.h"

// ---------------------------------------------------------------------------
// Tests for FClaireonBridge::EnsureNoLeakedWorlds + abort accumulator (#0000).
//
// Coverage:
//  - Case 1: With only the editor world loaded, EnsureNoLeakedWorlds returns
//    true and no leaks are reported.
//  - Case 2: A non-dirty force-loaded World package is detected and unloaded
//    cleanly so EnsureNoLeakedWorlds returns true with empty OutRemaining
//    (skipped if no test map is available in the build).
//  - Case 3: A dirty force-loaded World package is reported with bDirty=true
//    in OutRemaining and is NOT unloaded.
//  - Case 4: FormatLeakedWorldError contains both buckets and the
//    duplicate_and_open_map_async directive.
//  - Case 5: Abort accumulator round-trip (Report + Drain + Drain-empties).
// ---------------------------------------------------------------------------

namespace ClaireonBridgeLeakedWorldTestsLocal
{
	// File-local discriminator for any helpers; named to avoid unity collisions
	// with anonymous-namespace helpers in other Tests TUs.
	static UPackage* TryLoadTestMapPackage()
	{
		static const TCHAR* const Candidates[] =
		{
			TEXT("/Engine/Maps/Templates/OpenWorld"),
			TEXT("/Engine/Maps/Templates/Template_Default"),
		};
		for (const TCHAR* Candidate : Candidates)
		{
			UPackage* Loaded = LoadPackage(nullptr, Candidate, LOAD_None);
			if (Loaded)
			{
				return Loaded;
			}
		}
		return nullptr;
	}
}

UNTEST_UNIT_OPTS(Claireon, BridgeLeakedWorld, NoLeaksReturnsTrue, UNTEST_TIMEOUTMS(10000))
{
	TArray<FClaireonLeakedWorld> Remaining;
	const bool bOk = FClaireonBridge::EnsureNoLeakedWorlds(Remaining);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_ASSERT_EQ(Remaining.Num(), 0);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BridgeLeakedWorld, NonDirtyLeakUnloads, UNTEST_TIMEOUTMS(30000))
{
	UPackage* Loaded = ClaireonBridgeLeakedWorldTestsLocal::TryLoadTestMapPackage();
	if (!Loaded)
	{
		// No test map available in this build. Skip.
		UE_LOG(LogClaireon, Display,
			TEXT("[Test] BridgeLeakedWorld.NonDirtyLeakUnloads: no test map asset; skipping."));
		co_return;
	}
	TWeakObjectPtr<UPackage> Weak(Loaded);

	TArray<FClaireonLeakedWorld> Remaining;
	const bool bOk = FClaireonBridge::EnsureNoLeakedWorlds(Remaining);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_ASSERT_EQ(Remaining.Num(), 0);
	UNTEST_ASSERT_FALSE(Weak.IsValid());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BridgeLeakedWorld, DirtyLeakReportedNotUnloaded, UNTEST_TIMEOUTMS(30000))
{
	UPackage* Loaded = ClaireonBridgeLeakedWorldTestsLocal::TryLoadTestMapPackage();
	if (!Loaded)
	{
		UE_LOG(LogClaireon, Display,
			TEXT("[Test] BridgeLeakedWorld.DirtyLeakReportedNotUnloaded: no test map asset; skipping."));
		co_return;
	}
	const FString PackageName = Loaded->GetName();
	Loaded->SetDirtyFlag(true);

	TArray<FClaireonLeakedWorld> Remaining;
	const bool bOk = FClaireonBridge::EnsureNoLeakedWorlds(Remaining);
	UNTEST_ASSERT_FALSE(bOk);
	UNTEST_ASSERT_GE(Remaining.Num(), 1);

	bool bFoundDirty = false;
	for (const FClaireonLeakedWorld& L : Remaining)
	{
		if (L.PackageName == PackageName)
		{
			bFoundDirty = L.bDirty && !L.bUnloadAttempted;
			break;
		}
	}
	UNTEST_ASSERT_TRUE(bFoundDirty);

	UPackage* StillLoaded = FindPackage(nullptr, *PackageName);
	UNTEST_ASSERT_TRUE(StillLoaded != nullptr);

	// Cleanup: clear dirty flag and unload so later tests start clean.
	if (StillLoaded)
	{
		StillLoaded->SetDirtyFlag(false);
		TArray<UPackage*> Cleanup;
		Cleanup.Add(StillLoaded);
		UPackageTools::UnloadPackages(Cleanup);
	}
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BridgeLeakedWorld, FormatErrorContainsBothBuckets, UNTEST_TIMEOUTMS(5000))
{
	TArray<FClaireonLeakedWorld> Sample;
	{
		FClaireonLeakedWorld L;
		L.PackageName = TEXT("/Game/Maps/L_Foo");
		L.bDirty = true;
		Sample.Add(L);
	}
	{
		FClaireonLeakedWorld L;
		L.PackageName = TEXT("/Game/Maps/L_Bar");
		L.bUnloadAttempted = true;
		L.bUnloadSucceeded = false;
		Sample.Add(L);
	}
	const FString Msg = FClaireonBridge::FormatLeakedWorldError(Sample);
	UNTEST_ASSERT_TRUE(Msg.Contains(TEXT("/Game/Maps/L_Foo")));
	UNTEST_ASSERT_TRUE(Msg.Contains(TEXT("/Game/Maps/L_Bar")));
	UNTEST_ASSERT_TRUE(Msg.Contains(TEXT("duplicate_and_open_map_async")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BridgeLeakedWorld, FormatErrorEmptyOnNoLeaks, UNTEST_TIMEOUTMS(5000))
{
	TArray<FClaireonLeakedWorld> Empty;
	const FString Msg = FClaireonBridge::FormatLeakedWorldError(Empty);
	UNTEST_ASSERT_TRUE(Msg.IsEmpty());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BridgeLeakedWorld, AbortAccumulatorRoundTrip, UNTEST_TIMEOUTMS(5000))
{
	// Drain first to ensure clean state.
	(void)FClaireonBridge::DrainDeferredActionAborts();

	FClaireonBridge::ReportDeferredActionAbort(TEXT("Msg A"));
	FClaireonBridge::ReportDeferredActionAbort(TEXT("Msg B"));

	TArray<FString> Out = FClaireonBridge::DrainDeferredActionAborts();
	UNTEST_ASSERT_EQ(Out.Num(), 2);
	UNTEST_ASSERT_STREQ(*Out[0], TEXT("Msg A"));
	UNTEST_ASSERT_STREQ(*Out[1], TEXT("Msg B"));

	TArray<FString> SecondDrain = FClaireonBridge::DrainDeferredActionAborts();
	UNTEST_ASSERT_EQ(SecondDrain.Num(), 0);
	co_return;
}

#endif // WITH_UNTESTED
