// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonOutputGate.h"
#include "ClaireonSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/Timespan.h"
#include "SquidTasks/Task.h"

// ===========================================================================
// Sweep tests (ClaireonOutputGateSweepTests)
// ===========================================================================
// Cases 2a-2e per CLAIREON_DISK_RESULTS/test-plan.md section 2.  Each case
// populates a scoped test root with synthetic conv_NNN subdirectories, rewinds
// mtimes with IFileManager::SetTimeStamp, then calls SweepStaleSpills.
// ===========================================================================

namespace ClaireonOutputGateSweepHelpers
{
	static FString MakeUniqueTestRoot(const TCHAR* Case)
	{
		const FString ShortGuid = FGuid::NewGuid().ToString(EGuidFormats::Short);
		return FPaths::ProjectIntermediateDir()
			/ TEXT("ClaireonTests")
			/ TEXT("OutputGateSweep")
			/ FString(Case)
			/ ShortGuid;
	}

	struct FScopedTestRoot
	{
		FString Root;
		explicit FScopedTestRoot(const TCHAR* Case)
		{
			Root = MakeUniqueTestRoot(Case);
			IFileManager::Get().MakeDirectory(*Root, /*Tree*/ true);
			FClaireonOutputGate::SetResultsRootOverrideForTests(Root);
		}
		~FScopedTestRoot()
		{
			FClaireonOutputGate::SetResultsRootOverrideForTests(FString());
			if (!Root.IsEmpty())
			{
				IFileManager::Get().DeleteDirectory(*Root, /*bRequireExists*/ false, /*Tree*/ true);
			}
		}
	};

	/** Create a synthetic conv_NNN subdir with one file and rewind the file mtime. */
	static FString MakeAgedConvDir(const FString& Root, const TCHAR* Name, int32 AgeDays)
	{
		const FString Dir = Root / FString(Name);
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		const FString File = Dir / TEXT("spill.txt");
		FFileHelper::SaveStringToFile(TEXT("x"), *File);

		const FDateTime Aged = FDateTime::UtcNow() - FTimespan::FromDays(AgeDays);
		IFileManager::Get().SetTimeStamp(*File, Aged);
		IFileManager::Get().SetTimeStamp(*Dir, Aged);
		return Dir;
	}
}

// ===========================================================================
// Case 2a: empty root sweeps cleanly
// ===========================================================================

// File-IO test needs a generous timeout: SweepStaleSpills enumerates the
// results root and may exceed the 0.5ms default budget on cold disk caches.
UNTEST_UNIT_OPTS(Claireon, OutputGateSweep, EmptyRootNoop, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonOutputGateSweepHelpers;
	FScopedTestRoot Scope(TEXT("EmptyRootNoop"));

	FClaireonOutputGate::SweepStaleSpills(7);

	UNTEST_EXPECT_TRUE(IFileManager::Get().DirectoryExists(*Scope.Root));

	co_return;
}

// ===========================================================================
// Case 2b: two 10-day subdirs -> both deleted
// ===========================================================================

UNTEST_UNIT(Claireon, OutputGateSweep, TenDayOldSubdirsDeleted)
{
	using namespace ClaireonOutputGateSweepHelpers;
	FScopedTestRoot Scope(TEXT("TenDayOldSubdirsDeleted"));

	const FString D1 = MakeAgedConvDir(Scope.Root, TEXT("conv_001"), /*AgeDays*/ 10);
	const FString D2 = MakeAgedConvDir(Scope.Root, TEXT("conv_002"), /*AgeDays*/ 10);

	UNTEST_ASSERT_TRUE(IFileManager::Get().DirectoryExists(*D1));
	UNTEST_ASSERT_TRUE(IFileManager::Get().DirectoryExists(*D2));

	FClaireonOutputGate::SweepStaleSpills(7);

	UNTEST_EXPECT_FALSE(IFileManager::Get().DirectoryExists(*D1));
	UNTEST_EXPECT_FALSE(IFileManager::Get().DirectoryExists(*D2));

	co_return;
}

// ===========================================================================
// Case 2c: mixed ages -> only the 30-day subdir is deleted
// ===========================================================================

UNTEST_UNIT(Claireon, OutputGateSweep, MixedAgesRetainsYoungDirs)
{
	using namespace ClaireonOutputGateSweepHelpers;
	FScopedTestRoot Scope(TEXT("MixedAgesRetainsYoungDirs"));

	const FString Young = MakeAgedConvDir(Scope.Root, TEXT("conv_young"), /*AgeDays*/ 1);
	const FString Mid   = MakeAgedConvDir(Scope.Root, TEXT("conv_mid"),   /*AgeDays*/ 5);
	const FString Old   = MakeAgedConvDir(Scope.Root, TEXT("conv_old"),   /*AgeDays*/ 30);

	FClaireonOutputGate::SweepStaleSpills(7);

	UNTEST_EXPECT_TRUE(IFileManager::Get().DirectoryExists(*Young));
	UNTEST_EXPECT_TRUE(IFileManager::Get().DirectoryExists(*Mid));
	UNTEST_EXPECT_FALSE(IFileManager::Get().DirectoryExists(*Old));

	co_return;
}

// ===========================================================================
// Case 2d: bKeepResultSpills not observable at the gate level
// ===========================================================================
//
// FClaireonOutputGate::SweepStaleSpills takes RetentionDays as an explicit
// argument; the bKeepResultSpills setting is consulted by the CALLER (the
// connect-time sweep hook, not the gate).  This test documents that contract
// by asserting the gate unconditionally sweeps when invoked, and relies on a
// separate integration test for the caller's guard.
// ===========================================================================

UNTEST_UNIT(Claireon, OutputGateSweep, KeepSpillsIsCallerResponsibility)
{
	using namespace ClaireonOutputGateSweepHelpers;
	FScopedTestRoot Scope(TEXT("KeepSpillsIsCallerResponsibility"));

	const FString Old = MakeAgedConvDir(Scope.Root, TEXT("conv_old"), /*AgeDays*/ 30);

	// Direct call deletes regardless of bKeepResultSpills.  The gate itself is
	// policy-free; connection wiring is what honours the toggle.
	FClaireonOutputGate::SweepStaleSpills(7);
	UNTEST_EXPECT_FALSE(IFileManager::Get().DirectoryExists(*Old));

	co_return;
}

// ===========================================================================
// Case 2e: locked file tolerance -- sweep completes without throwing
// ===========================================================================

UNTEST_UNIT(Claireon, OutputGateSweep, LockedFileToleratedAcrossTwoSweeps)
{
	using namespace ClaireonOutputGateSweepHelpers;
	FScopedTestRoot Scope(TEXT("LockedFileToleratedAcrossTwoSweeps"));

	const FString Dir = MakeAgedConvDir(Scope.Root, TEXT("conv_locked"), /*AgeDays*/ 30);
	const FString Held = Dir / TEXT("held.txt");
	FFileHelper::SaveStringToFile(TEXT("held"), *Held);
	IFileManager::Get().SetTimeStamp(*Held,
		FDateTime::UtcNow() - FTimespan::FromDays(30));

	// Open a handle for reading so the underlying delete may fail.
	IFileHandle* Handle = FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Held, /*bAllowWrite*/ false);

	// First sweep: must not crash.  May or may not fully remove Dir depending
	// on platform file locking semantics.
	FClaireonOutputGate::SweepStaleSpills(7);

	if (Handle)
	{
		delete Handle;
		Handle = nullptr;
	}

	// Second sweep: must complete cleanly; with the handle released, the
	// directory is now removable.
	FClaireonOutputGate::SweepStaleSpills(7);
	UNTEST_EXPECT_FALSE(IFileManager::Get().DirectoryExists(*Dir));

	co_return;
}

#endif // WITH_UNTESTED
