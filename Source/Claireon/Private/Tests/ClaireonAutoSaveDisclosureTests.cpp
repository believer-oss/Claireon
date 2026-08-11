// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonAutoSave.h"
#include "ClaireonSettings.h"

// ---------------------------------------------------------------------------
// P0-7: deferred actions silently wrote dirty packages to disk.
//
// Before every deferred world transition (PIE start/stop, map open, map
// duplicate) the dispatcher calls FClaireonAutoSave::SaveIfNeeded, which runs
// UEditorLoadingAndSavingUtils::SavePackages over every dirty world AND content
// package -- no prompt, no source-control consultation, no path filter, no
// map/asset distinction, on by default.
//
// SaveIfNeeded returned a count and BOTH call sites discarded it, so the only
// trace of a write was a log line. Combined with level_place_actor marking the
// level package dirty, that is the verified mechanism for temp actors
// persisting into a tracked LFS umap unrequested.
//
// The end-to-end path (dirty a map, enqueue a deferred PIEStart, drain it,
// assert the envelope names the package) needs a live editor and is verified
// manually. What is pinned here is the disclosure CONTRACT, which is where the
// silent-write bug actually lived: the out-param must never carry stale or
// invented names, because a caller reading names for a save that did not happen
// is the same class of wrong as a save nobody reported.
// ---------------------------------------------------------------------------
namespace ClaireonAutoSaveDisclosureTestsInternal
{

// File-local discriminator prefix (AutoSaveDisclosure_) per module convention.

/** Restores the crash flag so a failure here cannot suppress auto-save for the
 *  rest of the run and turn this into someone else's mystery. */
struct FAutoSaveDisclosure_ScopedCrashFlag
{
	bool bWasSet;
	FAutoSaveDisclosure_ScopedCrashFlag()
		: bWasSet(FClaireonAutoSave::IsCrashFlagSet()) {}
	~FAutoSaveDisclosure_ScopedCrashFlag()
	{
		if (bWasSet)
		{
			FClaireonAutoSave::SetCrashFlag();
		}
		else
		{
			FClaireonAutoSave::ClearCrashFlag();
		}
	}
};

} // namespace ClaireonAutoSaveDisclosureTestsInternal

using namespace ClaireonAutoSaveDisclosureTestsInternal;

UNTEST_UNIT_OPTS(Claireon, AutoSaveDisclosure, SuppressedSaveReportsNoPackages, UNTEST_TIMEOUTMS(10000))
{
	FAutoSaveDisclosure_ScopedCrashFlag Restore;

	// The crash flag is the cheapest deterministic suppression path -- it
	// short-circuits before any package enumeration, so this needs no editor
	// state.
	FClaireonAutoSave::SetCrashFlag();

	// Pre-populated on purpose: a caller reusing a buffer must not be handed
	// names for a save that never happened.
	TArray<FString> SavedPackages;
	SavedPackages.Add(TEXT("/Game/Stale/LeftoverFromAPreviousCall"));

	const int32 SavedCount = FClaireonAutoSave::SaveIfNeeded(/*bIsPythonExecution=*/false, &SavedPackages);

	UNTEST_EXPECT_EQ(SavedCount, 0);
	UNTEST_EXPECT_EQ(SavedPackages.Num(), 0);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, AutoSaveDisclosure, DisabledAutoSaveReportsNoPackages, UNTEST_TIMEOUTMS(10000))
{
	// The master toggle is the escape hatch the warning text points callers at,
	// so it has to actually suppress both the write and the claim.
	UClaireonSettings* Settings = GetMutableDefault<UClaireonSettings>();
	UNTEST_ASSERT_PTR(Settings);

	const bool bPreviousEnableAutoSave = Settings->bEnableAutoSave;
	Settings->bEnableAutoSave = false;

	TArray<FString> SavedPackages;
	SavedPackages.Add(TEXT("/Game/Stale/LeftoverFromAPreviousCall"));

	const int32 SavedCount = FClaireonAutoSave::SaveIfNeeded(/*bIsPythonExecution=*/false, &SavedPackages);

	Settings->bEnableAutoSave = bPreviousEnableAutoSave;

	UNTEST_EXPECT_EQ(SavedCount, 0);
	UNTEST_EXPECT_EQ(SavedPackages.Num(), 0);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, AutoSaveDisclosure, NullOutParamIsAccepted, UNTEST_TIMEOUTMS(10000))
{
	// The parameter is optional so existing call sites compile unchanged; a null
	// must not crash the suppression paths.
	FAutoSaveDisclosure_ScopedCrashFlag Restore;
	FClaireonAutoSave::SetCrashFlag();

	const int32 SavedCount = FClaireonAutoSave::SaveIfNeeded(/*bIsPythonExecution=*/false, nullptr);
	UNTEST_EXPECT_EQ(SavedCount, 0);

	co_return;
}

#endif // WITH_UNTESTED
