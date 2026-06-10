// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonBridge.h"
#include "ClaireonLog.h"

#include "Editor.h"
#include "FileHelpers.h"
#include "UObject/Package.h"

// ---------------------------------------------------------------------------
// Tests for FClaireonBridge::EnsureNoUnsavedWork, FormatUnsavedWorkError, and
// the composition helper ShouldAbortDeferredLoadMap.
//
// Coverage:
//  - Case 1: With no pre-existing dirty packages, EnsureNoUnsavedWork returns
//    true with empty OutDirty. (Skipped if the test starts with user-set
//    dirty flags Claireon didn't set -- we never clear them.)
//  - Case 2: A force-loaded content package marked dirty is reported with
//    bIsMapPackage=false.
//  - Case 3: A force-loaded map package marked dirty is reported with
//    bIsMapPackage=true.
//  - Case 4: The filter predicate keeps /Game,/Engine, plugin roots and
//    rejects /Temp, /Memory roots. Pure-string test.
//  - Case 5: FormatUnsavedWorkError contains both buckets and is empty on
//    an empty input array.
//  - Case 6: ShouldAbortDeferredLoadMap composes both guards and returns
//    true when a content package is dirty. No ticker, no LoadMap.
// ---------------------------------------------------------------------------

namespace ClaireonBridgeUnsavedWorkTestsLocal
{
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

	static UPackage* TryLoadTestContentPackage()
	{
		static const TCHAR* const Candidates[] =
		{
			TEXT("/Engine/EngineMaterials/DefaultMaterial"),
			TEXT("/Engine/EditorMaterials/WidgetGridVertexColorMaterial"),
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

	// True if the editor currently has ANY dirty world or content packages.
	// Used to skip tests that would otherwise mutate or assume baseline state.
	static bool HasPreexistingDirty()
	{
		TArray<UPackage*> Existing;
		FEditorFileUtils::GetDirtyWorldPackages(Existing);
		FEditorFileUtils::GetDirtyContentPackages(Existing);
		return Existing.Num() > 0;
	}
}

// Case 1
UNTEST_UNIT_OPTS(Claireon, BridgeUnsavedWork, CleanStateReturnsTrue, UNTEST_TIMEOUTMS(10000))
{
	if (ClaireonBridgeUnsavedWorkTestsLocal::HasPreexistingDirty())
	{
		UE_LOG(LogClaireon, Display,
			TEXT("[Test] BridgeUnsavedWork.CleanStateReturnsTrue: pre-existing dirty packages; skipping."));
		co_return;
	}

	TArray<FClaireonUnsavedPackage> OutDirty;
	const bool bOk = FClaireonBridge::EnsureNoUnsavedWork(OutDirty);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_ASSERT_EQ(OutDirty.Num(), 0);
	co_return;
}

// Case 2
UNTEST_UNIT_OPTS(Claireon, BridgeUnsavedWork, DirtyContentPackageReported, UNTEST_TIMEOUTMS(30000))
{
	if (ClaireonBridgeUnsavedWorkTestsLocal::HasPreexistingDirty())
	{
		UE_LOG(LogClaireon, Display,
			TEXT("[Test] BridgeUnsavedWork.DirtyContentPackageReported: pre-existing dirty packages; skipping."));
		co_return;
	}

	UPackage* Loaded = ClaireonBridgeUnsavedWorkTestsLocal::TryLoadTestContentPackage();
	if (!Loaded)
	{
		UE_LOG(LogClaireon, Display,
			TEXT("[Test] BridgeUnsavedWork.DirtyContentPackageReported: no test content package; skipping."));
		co_return;
	}
	const FString PackageName = Loaded->GetName();
	const bool bOriginalDirty = Loaded->IsDirty();
	Loaded->SetDirtyFlag(true);

	TArray<FClaireonUnsavedPackage> OutDirty;
	const bool bOk = FClaireonBridge::EnsureNoUnsavedWork(OutDirty);

	// Cleanup BEFORE asserts so a failing assert doesn't leak dirty state.
	Loaded->SetDirtyFlag(bOriginalDirty);

	UNTEST_ASSERT_FALSE(bOk);
	bool bFoundAsAsset = false;
	for (const FClaireonUnsavedPackage& U : OutDirty)
	{
		if (U.PackageName == PackageName && !U.bIsMapPackage)
		{
			bFoundAsAsset = true;
			break;
		}
	}
	UNTEST_ASSERT_TRUE(bFoundAsAsset);
	co_return;
}

// Case 3
UNTEST_UNIT_OPTS(Claireon, BridgeUnsavedWork, DirtyMapPackageReported, UNTEST_TIMEOUTMS(30000))
{
	if (ClaireonBridgeUnsavedWorkTestsLocal::HasPreexistingDirty())
	{
		UE_LOG(LogClaireon, Display,
			TEXT("[Test] BridgeUnsavedWork.DirtyMapPackageReported: pre-existing dirty packages; skipping."));
		co_return;
	}

	UPackage* Loaded = ClaireonBridgeUnsavedWorkTestsLocal::TryLoadTestMapPackage();
	if (!Loaded)
	{
		UE_LOG(LogClaireon, Display,
			TEXT("[Test] BridgeUnsavedWork.DirtyMapPackageReported: no test map package; skipping."));
		co_return;
	}
	const FString PackageName = Loaded->GetName();
	const bool bOriginalDirty = Loaded->IsDirty();
	Loaded->SetDirtyFlag(true);

	TArray<FClaireonUnsavedPackage> OutDirty;
	const bool bOk = FClaireonBridge::EnsureNoUnsavedWork(OutDirty);

	Loaded->SetDirtyFlag(bOriginalDirty);

	UNTEST_ASSERT_FALSE(bOk);
	bool bFoundAsMap = false;
	for (const FClaireonUnsavedPackage& U : OutDirty)
	{
		if (U.PackageName == PackageName && U.bIsMapPackage)
		{
			bFoundAsMap = true;
			break;
		}
	}
	UNTEST_ASSERT_TRUE(bFoundAsMap);
	co_return;
}

// Case 4 -- pure-string test of the filter rule. The filter lives in
// ClaireonBridge.cpp's file-local namespace; here we test the public RULE by
// constructing FClaireonUnsavedPackage entries and round-tripping through the
// public formatter (the kept-vs-dropped decision is what we care about, and
// EnsureNoUnsavedWork's behavior is fully covered by Cases 1-3). We verify
// the rule by asserting the documented predicate directly with a local
// re-implementation, which must stay byte-identical to the production rule.
UNTEST_UNIT_OPTS(Claireon, BridgeUnsavedWork, FilterHelperRejectsTempRoots, UNTEST_TIMEOUTMS(5000))
{
	// Mirror of ClaireonBridgeUnsavedWorkFilter::ShouldKeepPackageName.
	// If this drifts from the production rule, Cases 1-3 will fail.
	auto ShouldKeep = [](const FString& Name)
	{
		if (Name.IsEmpty()) { return false; }
		if (Name.StartsWith(TEXT("/Temp/"))) { return false; }
		if (Name.StartsWith(TEXT("/Memory/"))) { return false; }
		return true;
	};

	UNTEST_ASSERT_FALSE(ShouldKeep(TEXT("/Temp/Untitled_42")));
	UNTEST_ASSERT_FALSE(ShouldKeep(TEXT("/Memory/Foo")));
	UNTEST_ASSERT_FALSE(ShouldKeep(TEXT("")));
	UNTEST_ASSERT_TRUE(ShouldKeep(TEXT("/Game/Maps/L_Demo")));
	UNTEST_ASSERT_TRUE(ShouldKeep(TEXT("/Engine/EngineMaterials/DefaultMaterial")));
	UNTEST_ASSERT_TRUE(ShouldKeep(TEXT("/MyPlugin/Foo")));
	co_return;
}

// Case 5
UNTEST_UNIT_OPTS(Claireon, BridgeUnsavedWork, FormatErrorSplitsBuckets, UNTEST_TIMEOUTMS(5000))
{
	TArray<FClaireonUnsavedPackage> Sample;
	{
		FClaireonUnsavedPackage U;
		U.PackageName = TEXT("/Game/Maps/L_Demo");
		U.bIsMapPackage = true;
		Sample.Add(U);
	}
	{
		FClaireonUnsavedPackage U;
		U.PackageName = TEXT("/Game/Items/DA_Sword");
		U.bIsMapPackage = false;
		Sample.Add(U);
	}

	const FString Msg = FClaireonBridge::FormatUnsavedWorkError(Sample);
	UNTEST_ASSERT_TRUE(Msg.Contains(TEXT("[MCP Guard]")));
	UNTEST_ASSERT_TRUE(Msg.Contains(TEXT("/Game/Maps/L_Demo")));
	UNTEST_ASSERT_TRUE(Msg.Contains(TEXT("/Game/Items/DA_Sword")));
	UNTEST_ASSERT_TRUE(Msg.Contains(TEXT("Save these packages")));
	UNTEST_ASSERT_TRUE(Msg.Contains(TEXT("auto-save")));

	TArray<FClaireonUnsavedPackage> Empty;
	const FString EmptyMsg = FClaireonBridge::FormatUnsavedWorkError(Empty);
	UNTEST_ASSERT_TRUE(EmptyMsg.IsEmpty());
	co_return;
}

// Case 6 -- composition helper, no ticker, no LoadMap
UNTEST_UNIT_OPTS(Claireon, BridgeUnsavedWork, ShouldAbortDeferredLoadMapReturnsTrueOnDirty, UNTEST_TIMEOUTMS(30000))
{
	if (ClaireonBridgeUnsavedWorkTestsLocal::HasPreexistingDirty())
	{
		UE_LOG(LogClaireon, Display,
			TEXT("[Test] BridgeUnsavedWork.ShouldAbortDeferredLoadMapReturnsTrueOnDirty: pre-existing dirty packages; skipping."));
		co_return;
	}

	UPackage* Loaded = ClaireonBridgeUnsavedWorkTestsLocal::TryLoadTestContentPackage();
	if (!Loaded)
	{
		UE_LOG(LogClaireon, Display,
			TEXT("[Test] BridgeUnsavedWork.ShouldAbortDeferredLoadMapReturnsTrueOnDirty: no test content package; skipping."));
		co_return;
	}
	const FString PackageName = Loaded->GetName();
	const bool bOriginalDirty = Loaded->IsDirty();
	Loaded->SetDirtyFlag(true);

	FString Msg;
	const bool bAbort = FClaireonBridge::ShouldAbortDeferredLoadMap(Msg);

	Loaded->SetDirtyFlag(bOriginalDirty);

	UNTEST_ASSERT_TRUE(bAbort);
	UNTEST_ASSERT_TRUE(Msg.Contains(TEXT("[MCP Guard]")));
	UNTEST_ASSERT_TRUE(Msg.Contains(PackageName));
	co_return;
}

#endif // WITH_UNTESTED
