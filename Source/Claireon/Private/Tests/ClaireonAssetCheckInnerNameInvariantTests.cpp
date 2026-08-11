// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Functional tests for asset_check_inner_name_invariant. The
// tool is registry-driven and stateless, so the test covers (a) the
// discoverable surface, (b) the empty-args happy path, (c) the
// invalid-content-path clean result, and (d) the read-only invariant
// via grep-style code-review on the .cpp.

#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonTestDataAssertions.h"
#include "Tools/ClaireonTool_AssetCheckInnerNameInvariant.h"
#include "Tools/IClaireonTool.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"

// ---------------------------------------------------------------------------
// 1. Discoverable surface: name, description, schema shape.
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, AssetCheckInnerNameInvariant, ExposesDiscoverableSurface, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_AssetCheckInnerNameInvariant Tool;

	const FString Name = Tool.GetName();
	UNTEST_EXPECT_EQ(Name, FString(TEXT("asset_check_inner_name_invariant")));

	const FString Desc = Tool.GetDescription();
	UNTEST_EXPECT_TRUE(Desc.Len() >= 80);
	UNTEST_EXPECT_TRUE(Desc.Len() <= 400);
	UNTEST_EXPECT_TRUE(Desc.Contains(TEXT("Stateless")));

	TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_TRUE(Schema.IsValid());
	FString SchemaType;
	UNTEST_ASSERT_TRUE(Schema->TryGetStringField(TEXT("type"), SchemaType));
	UNTEST_EXPECT_EQ(SchemaType, FString(TEXT("object")));

	const TSharedPtr<FJsonObject>* Properties = nullptr;
	UNTEST_ASSERT_TRUE(Schema->TryGetObjectField(TEXT("properties"), Properties));
	UNTEST_EXPECT_TRUE((*Properties)->HasField(TEXT("contentPath")));
	UNTEST_EXPECT_TRUE((*Properties)->HasField(TEXT("includePlugins")));

	co_return;
}

// ---------------------------------------------------------------------------
// 2. Empty args -> defaults to /Game, returns shaped success.
// ---------------------------------------------------------------------------
// Budget: empty args means the tool sweeps all of /Game through the asset registry, and
// the scanned-count check below runs a SECOND sweep to derive the expected number, so the
// budget covers two passes over ~149k assets. The bare UNTEST_UNIT default of 0.50ms
// (FUntestUnitFixture::DefaultTimeoutMs) is not a deliberate perf assertion and cannot
// cover a registry-wide scan. Do not restore it.
UNTEST_UNIT_OPTS(Claireon, AssetCheckInnerNameInvariant, EmptyArgsScansGameDefault, UNTEST_TIMEOUTMS(120000))
{
	ClaireonTool_AssetCheckInnerNameInvariant Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	IClaireonTool::FToolResult Result = Tool.Execute(Args);

	UNTEST_EXPECT_FALSE(Result.bIsError);

	// Structured facts come out of Result.Data, never GetContentAsString().
	// See ClaireonTestDataAssertions.h for the convention and for what the
	// failure message tells you when a field is not there.
	int32 MismatchCount = -1;
	UNTEST_CLAIREON_DATA_INT(Result, "mismatch_count", MismatchCount);

	int32 ScannedCount = -1;
	UNTEST_CLAIREON_DATA_INT(Result, "scanned_count", ScannedCount);

	bool bTruncated = true;
	UNTEST_CLAIREON_DATA_BOOL(Result, "truncated", bTruncated);

	const TArray<TSharedPtr<FJsonValue>>* Mismatches = nullptr;
	UNTEST_CLAIREON_DATA_ARRAY(Result, "mismatches", Mismatches);
	UNTEST_EXPECT_EQ(Mismatches->Num(), MismatchCount);

	// ------------------------------------------------------------------
	// The scan actually scanned something.
	//
	// This replaces `UNTEST_EXPECT_TRUE(ScannedCount >= 0)`, which was
	// unfalsifiable: ScannedCount is built by incrementing from zero, so no code
	// path can make it negative. The tool could return zero for every asset in
	// the project and that assertion still passed.
	//
	// Instead, re-derive the expected count independently from the asset
	// registry using the same filter the tool documents (recursive /Game,
	// on-disk only, redirectors dropped, non-/Game packages dropped when
	// includePlugins is false). That deliberately mirrors the tool's accounting:
	// this test's job is to prove the reported scanned_count corresponds to a real
	// sweep, so if the tool's filter changes this check must change with it.
	// ------------------------------------------------------------------
	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.bRecursivePaths = true;
	Filter.bRecursiveClasses = true;
	Filter.bIncludeOnlyOnDiskAssets = true;
	Filter.PackagePaths.Add(FName(TEXT("/Game")));

	TArray<FAssetData> RegistryAssets;
	AssetRegistry.GetAssets(Filter, RegistryAssets);

	// Absolute floor, justified against the repository rather than guessed:
	// `git ls-files Content/**/*.uasset` is ~149,000 entries plus ~320 .umap, so
	// a registry that reports fewer than 1,000 on-disk assets under /Game has
	// not finished (or never started) its scan. In that state every assertion
	// below is meaningless, so fail here rather than let 0 == 0 pass.
	UNTEST_ASSERT_GE(RegistryAssets.Num(), 1000);

	int32 ExpectedScanned = 0;
	for (const FAssetData& Asset : RegistryAssets)
	{
		if (Asset.IsRedirector())
		{
			continue;
		}
		if (!Asset.PackageName.ToString().StartsWith(TEXT("/Game")))
		{
			continue;
		}
		++ExpectedScanned;
	}

	// The tool bails out of its loop once it has collected 500 mismatches, so
	// scanned_count only tracks the full set on an untruncated sweep.
	if (bTruncated)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[AssetCheckInnerNameInvariant] sweep truncated at the 500-mismatch cap "
			     "(scanned=%d of an expected %d); only the floor is enforced"),
			ScannedCount, ExpectedScanned);
		UNTEST_EXPECT_GE(ScannedCount, 1000);
	}
	else
	{
		// Tolerance rather than strict equality on purpose: this test runs inside a
		// suite whose other tests create and delete /Game/__MCPTests fixtures, so
		// the registry can legitimately gain or lose a handful of entries between
		// the tool's sweep and the one above. 32 is far below any real filter
		// regression (those move the count by thousands, or to zero) and far above
		// the number of scratch fixtures the suite holds at once.
		const int32 Drift = FMath::Abs(ScannedCount - ExpectedScanned);
		if (Drift > 32)
		{
			UE_LOG(LogTemp, Error,
				TEXT("[AssetCheckInnerNameInvariant] scanned_count=%d but an independent /Game "
				     "sweep with the same filter found %d (drift %d); the tool is not scanning "
				     "the set it reports."),
				ScannedCount, ExpectedScanned, Drift);
		}
		UNTEST_EXPECT_LE(Drift, 32);
	}

	co_return;
}

// ---------------------------------------------------------------------------
// 3. Invalid contentPath -> zero scanned/mismatched, no error.
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, AssetCheckInnerNameInvariant, InvalidContentPathReturnsCleanResult, UNTEST_TIMEOUTMS(30000))
{
	ClaireonTool_AssetCheckInnerNameInvariant Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("contentPath"), TEXT("/Nonexistent/Path/Does/Not/Exist"));
	IClaireonTool::FToolResult Result = Tool.Execute(Args);

	// Asset registry returns zero entries for invalid paths; the tool
	// should report success with zero scanned + zero mismatches rather
	// than erroring.
	UNTEST_EXPECT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	int32 ScannedCount = -1;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetNumberField(TEXT("scanned_count"), ScannedCount));
	UNTEST_EXPECT_EQ(ScannedCount, 0);

	co_return;
}

// ---------------------------------------------------------------------------
// 4. Read-only invariant: source contains no mutation calls.
// ---------------------------------------------------------------------------
// Budget: the bare UNTEST_UNIT default is 0.50ms (FUntestUnitFixture::DefaultTimeoutMs),
// which is not a deliberate perf assertion. Too tight now that the tool registry is
// populated process-wide and this test does real work -- do not restore the default.
UNTEST_UNIT_OPTS(Claireon, AssetCheckInnerNameInvariant, ReadOnlyByConstruction, UNTEST_TIMEOUTMS(10000))
{
	const FString PluginDir = IPluginManager::Get()
		.FindPlugin(TEXT("Claireon"))->GetBaseDir();
	const FString SourcePath = PluginDir
		/ TEXT("Source/Claireon/Private/Tools/ClaireonTool_AssetCheckInnerNameInvariant.cpp");

	FString SourceText;
	UNTEST_ASSERT_TRUE(FFileHelper::LoadFileToString(SourceText, *SourcePath));

	UNTEST_EXPECT_FALSE(SourceText.Contains(TEXT("SaveAsset")));
	UNTEST_EXPECT_FALSE(SourceText.Contains(TEXT("MarkPackageDirty")));
	UNTEST_EXPECT_FALSE(SourceText.Contains(TEXT("UPackage::Rename")));
	UNTEST_EXPECT_FALSE(SourceText.Contains(TEXT("AssetRegistry.AssetCreated")));
	UNTEST_EXPECT_FALSE(SourceText.Contains(TEXT("FAssetRegistryModule::AssetCreated")));

	co_return;
}

#endif // WITH_UNTESTED
