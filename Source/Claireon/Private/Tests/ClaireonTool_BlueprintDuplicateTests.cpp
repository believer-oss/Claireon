// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Tests for bp_duplicate (fracture 04).
//
// The project's Untest framework exposes UNTEST_UNIT and UNTEST_WORLD (plus a few
// client/server variants); there is no standalone UNTEST_FUNCTIONAL macro. Per the
// fracture-04 "collapse into one file" fallback, this single file contains both
// unit-style parameter/error-surface tests and functional-style end-to-end tests.
// Functional tests use UNTEST_UNIT (like other tool-exercising tests in this folder,
// e.g. ClaireonApplySpecTests.cpp) and encode their category in the third macro
// argument: Unit_* vs Functional_* to allow test-name filtering.
//
// Test skip policy:
//  - The acceptance-case test auto-discovers any project UBlueprint to duplicate; if the
//    project has none (e.g. a bare host), the test logs a note and early-returns success.
//  - Branch-2 IsChildOf<UBlueprint> requires a concrete UBlueprint-derived class
//    that is NOT in the {Blueprint, AnimBlueprint, WidgetBlueprint} allowlist. The
//    project currently has no such class in a discoverable fixture path; the test
//    skips if no suitable fixture is located.
//  - FName-path-holder D5-exclusion test and the PrimaryAssetId rewrite test rely
//    on fixtures with specific variable layouts that cannot be synthesised via
//    a plain UBlueprint at test time; these are covered by the generic-reference
//    rewrite tests instead.
//  - "DuplicateAsset returns nullptr" and "SavePackage fails" cannot be simulated
//    without invasive fakes and are omitted per the fracture's allowance.
//
// Fixture and crash-avoidance policy (see Docs/llm/todo/claireon-untest-harness-
// reliability.md item 1 for the full mechanism -- a whole-object-graph referencer scan
// that DeleteAsset/ForceDeleteObjects runs before deleting, which crashes serializing
// Niagara type data if any Niagara asset happens to be resident in memory):
//  - SOURCE fixtures (CreatePlainBlueprintFixture) are created in-memory only and
//    deliberately never saved, mirroring FindBlueprintWithStructArray in
//    ClaireonTool_SetBlueprintCDOPropertyTests.cpp. bp_duplicate only needs the source
//    registered with the asset registry and loadable, not present on disk, so this
//    avoids a delete call -- and therefore a referencer scan -- for every source fixture.
//  - DESTINATION fixtures cannot use that trick: bp_duplicate unconditionally saves the
//    duplicate to disk (that is the behavior under test), so every functional test's
//    destination is a real .uasset and its cleanup delete is a genuine referencer-scan
//    trigger. That is unavoidable without changing the tool's own behavior, which is out
//    of scope here.
//  - Destinations live under /Game/__MCPTests/ (normalized from the previous
//    /Game/Sandbox/, which was the only Claireon suite not following the standard
//    convention). SweepStaleDestinationFixturesOnce() clears all of this suite's known
//    destination paths exactly once, before the first functional test's body runs. This
//    does not make any single delete safer -- deleting a Niagara-referencing duplicate
//    (Functional_AcceptanceCaseMatchesParentProposal, which duplicates a discovered project
//    Blueprint that may reference Niagara systems) is exactly as likely to hit the crash as
//    before. What it fixes is a crashed run's leftover poisoning
//    `git status --porcelain -- Content/` indefinitely: the next run's sweep clears it
//    before that run has loaded anything Niagara-related, which is the safe place to do it.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonTool_BlueprintDuplicate.h"
#include "Tools/IClaireonTool.h"
#include "ClaireonTestAssetDiscovery.h"
#include "Misc/PackageName.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Editor.h"
#include "Engine/Blueprint.h"

#include "GameFramework/Actor.h"

#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

#include "Kismet2/KismetEditorUtilities.h"

#include "ClaireonTestAssetDeletion.h"
namespace ClaireonTool_BlueprintDuplicateTests_Private
{

static const TCHAR* FixtureFolder = TEXT("/Game/Tests/ClaireonBlueprintDuplicate");

// --- args helpers -----------------------------------------------------------

TSharedPtr<FJsonObject> MakeDuplicateArgs(const TCHAR* SourcePath, const TCHAR* DestPath)
{
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("source_path"), SourcePath);
	Args->SetStringField(TEXT("dest_path"), DestPath);
	return Args;
}

TSharedPtr<FJsonObject> MakeDuplicateArgsWithRename(const TCHAR* SourcePath, const TCHAR* DestPath, bool bRename)
{
	TSharedPtr<FJsonObject> Args = MakeDuplicateArgs(SourcePath, DestPath);
	Args->SetBoolField(TEXT("rename_dependencies"), bRename);
	return Args;
}

// --- cleanup helper ---------------------------------------------------------

void DeleteIfExists(const FString& ObjectPath)
{
	if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad(); IsValid(Asset))
	{
		TArray<UObject*> AssetsToDelete;
		AssetsToDelete.Add(Asset);
		ClaireonTestAssetDeletion::DeleteObjectsForTest(AssetsToDelete);
	}
}

// Deletes any stale leftover at this suite's known destination paths, exactly once per
// process, before the first functional test's body runs. A function-local static
// initializer (rather than hooking a specific "first" test by declaration order) means
// this fires correctly no matter which functional test happens to run first under a
// narrower -TestFilter.
//
// This does NOT make any single delete safer -- deleting a Niagara-referencing duplicate
// (Functional_AcceptanceCaseMatchesParentProposal) is exactly as likely to hit the
// GetAllReferencersIncludingWeak crash as before. What it fixes is a crashed run's
// leftover poisoning `git status --porcelain -- Content/` indefinitely: the next run's
// sweep clears it before this suite has loaded anything Niagara-related, which is the
// safe place to do it (see Docs/llm/todo/claireon-untest-harness-reliability.md item 1).
void SweepStaleDestinationFixturesOnce()
{
	static const bool bSwept = []() -> bool
	{
		// Kept in sync by hand with the DestPackage/DestPath literals in the functional
		// tests below; a path missing here just means that one test does not benefit
		// from the cross-run sweep (its own per-test pre-flight DeleteIfExists still
		// protects it exactly as before).
		static const TCHAR* KnownDestinationPaths[] =
		{
			TEXT("/Game/__MCPTests/BP_DoesNotExist_Clone.BP_DoesNotExist_Clone"),
			TEXT("/Game/__MCPTests/BP_Plain_Happy_Clone.BP_Plain_Happy_Clone"),
			TEXT("/Game/__MCPTests/BP_Plain_Branch1_Clone.BP_Plain_Branch1_Clone"),
			TEXT("/Game/__MCPTests/BP_Plain_DefaultRename_Clone.BP_Plain_DefaultRename_Clone"),
			TEXT("/Game/__MCPTests/BP_Plain_DestExists_Clone.BP_Plain_DestExists_Clone"),
			TEXT("/Game/__MCPTests/BP_Plain_RenameTrue_Clone.BP_Plain_RenameTrue_Clone"),
		};
		for (const TCHAR* Path : KnownDestinationPaths)
		{
			DeleteIfExists(Path);
		}
		return true;
	}();
	(void)bSwept;
}

// --- fixture creation -------------------------------------------------------

// Create a plain Blueprint (parent=AActor) at PackagePath, registered with the asset
// registry but deliberately NEVER saved to disk -- bp_duplicate only needs the source
// loadable and registry-visible, not present in Content/, and an in-memory-only source
// needs no cleanup delete, which is what triggers the referencer-scan crash (see the
// file-level comment above). Same pattern as FindBlueprintWithStructArray in
// ClaireonTool_SetBlueprintCDOPropertyTests.cpp. Returns nullptr if creation fails.
UBlueprint* CreatePlainBlueprintFixture(const FString& PackagePath)
{
	// If it already exists (an earlier test in this run created it), reuse it.
	if (UBlueprint* Existing = Cast<UBlueprint>(FSoftObjectPath(PackagePath + TEXT(".") + FPackageName::GetShortName(PackagePath)).TryLoad()); IsValid(Existing))
	{
		return Existing;
	}

	UPackage* Package = CreatePackage(*PackagePath);
	if (!IsValid(Package))
	{
		return nullptr;
	}

	const FString AssetName = FPackageName::GetShortName(PackagePath);
	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		NAME_None);

	if (!IsValid(BP))
	{
		return nullptr;
	}

	FAssetRegistryModule::AssetCreated(BP);
	// Deliberately no UPackage::Save call -- see the function comment above.

	return BP;
}

} // namespace ClaireonTool_BlueprintDuplicateTests_Private
using namespace ClaireonTool_BlueprintDuplicateTests_Private;

// ============================================================================
// Unit tests -- parameter parsing and error surface
// ============================================================================

// Budget: same reason as Unit_InvalidDestinationPathPropagatesResolverError below --
// the bare UNTEST_UNIT default is 0.50ms (FUntestUnitFixture::DefaultTimeoutMs), which
// is not a deliberate perf assertion. This is the first test in the suite, so it absorbs
// one-time construction/registry warmup and measured 0.65ms while its siblings run in
// 0.02ms. Do not restore the default.
UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Unit_MissingSourcePathReturnsStructuredError, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_BlueprintDuplicate Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("dest_path"), TEXT("/Game/__MCPTests/Foo"));

	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_STREQ(*Result.ErrorMessage, TEXT("Missing required field: source_path"));
	co_return;
}

// Budget: see the UNTEST_TIMEOUTMS rationale on Unit_MissingSourcePathReturnsStructuredError
// above -- the bare default (0.50ms) is not a deliberate perf assertion.
UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Unit_MissingDestPathReturnsStructuredError, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_BlueprintDuplicate Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("source_path"), TEXT("/Game/__MCPTests/Foo"));

	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_STREQ(*Result.ErrorMessage, TEXT("Missing required field: dest_path"));
	co_return;
}

// Budget: see the UNTEST_TIMEOUTMS rationale on Unit_MissingSourcePathReturnsStructuredError
// above -- the bare default (0.50ms) is not a deliberate perf assertion.
UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Unit_EmptySourcePathReturnsStructuredError, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_BlueprintDuplicate Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("source_path"), TEXT(""));
	Args->SetStringField(TEXT("dest_path"), TEXT("/Game/__MCPTests/Foo"));

	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_STREQ(*Result.ErrorMessage, TEXT("Missing required field: source_path"));
	co_return;
}

// Budget: see the UNTEST_TIMEOUTMS rationale on Unit_MissingSourcePathReturnsStructuredError
// above -- the bare default (0.50ms) is not a deliberate perf assertion.
UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Unit_EmptyDestPathReturnsStructuredError, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_BlueprintDuplicate Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("source_path"), TEXT("/Game/__MCPTests/Foo"));
	Args->SetStringField(TEXT("dest_path"), TEXT(""));

	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_STREQ(*Result.ErrorMessage, TEXT("Missing required field: dest_path"));
	co_return;
}

// Budget: the bare UNTEST_UNIT default is 0.50ms (FUntestUnitFixture::DefaultTimeoutMs),
// which is not a deliberate perf assertion. Too tight now that the tool registry is
// populated process-wide and this test does real work -- do not restore the default.
UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Unit_InvalidDestinationPathPropagatesResolverError, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_BlueprintDuplicate Tool;

	// A source path that cannot be resolved (no /Game/ and no Content/ anchor).
	TSharedPtr<FJsonObject> Args = MakeDuplicateArgs(
		TEXT("Z:\\no\\such\\anchor\\asset"),
		TEXT("/Game/__MCPTests/WhateverDest"));

	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_FALSE(Result.ErrorMessage.IsEmpty());
	co_return;
}

// Root cause of the previous failure: these two asserted the pre-migration
// category spelling "blueprint". The Blueprint tool family is registered under
// kBPCategory == "bp" (Tools/ClaireonBlueprintGraphEditToolBase.h), and
// IClaireonTool::GetName() is sealed to GetCategory() + "_" + GetOperation(),
// so the wire name is "bp_duplicate". No tool in the registry has category
// "blueprint". Test defect, not a product bug -- updated to the shipped names.
// Budget: see the UNTEST_TIMEOUTMS rationale on Unit_MissingSourcePathReturnsStructuredError
// above -- the bare default (0.50ms) is not a deliberate perf assertion.
UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Unit_GetNameReturnsCanonicalString, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_BlueprintDuplicate Tool;
	UNTEST_EXPECT_STREQ(*Tool.GetName(), TEXT("bp_duplicate"));
	co_return;
}

// Budget: see the UNTEST_TIMEOUTMS rationale on Unit_MissingSourcePathReturnsStructuredError
// above -- the bare default (0.50ms) is not a deliberate perf assertion.
UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Unit_CategoryDerivesToBlueprint, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_BlueprintDuplicate Tool;
	UNTEST_EXPECT_STREQ(*Tool.GetCategory(), TEXT("bp"));
	UNTEST_EXPECT_STREQ(*Tool.GetOperation(), TEXT("duplicate"));
	co_return;
}

// Budget: see the UNTEST_TIMEOUTMS rationale on Unit_MissingSourcePathReturnsStructuredError
// above -- the bare default (0.50ms) is not a deliberate perf assertion.
UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Unit_InputSchemaDeclaresRequiredFields, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_BlueprintDuplicate Tool;
	TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_TRUE(Schema.IsValid());

	const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
	UNTEST_ASSERT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));

	bool bHasSource = false;
	bool bHasDest = false;
	for (const TSharedPtr<FJsonValue>& V : *Required)
	{
		FString S;
		if (V->TryGetString(S))
		{
			if (S == TEXT("source_path")) { bHasSource = true; }
			if (S == TEXT("dest_path")) { bHasDest = true; }
		}
	}
	UNTEST_EXPECT_TRUE(bHasSource);
	UNTEST_EXPECT_TRUE(bHasDest);

	// Ensure rename_dependencies is present in properties but NOT required.
	const TSharedPtr<FJsonObject>* Props = nullptr;
	UNTEST_ASSERT_TRUE(Schema->TryGetObjectField(TEXT("properties"), Props));
	UNTEST_EXPECT_TRUE((*Props)->HasField(TEXT("rename_dependencies")));
	co_return;
}

// ============================================================================
// Functional tests -- exercise the full pipeline
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Functional_SourceNotFoundReturnsStructuredError, UNTEST_TIMEOUTMS(30000))
{
	ClaireonTool_BlueprintDuplicate Tool;

	SweepStaleDestinationFixturesOnce();

	const FString MissingSource = TEXT("/Game/Tests/ClaireonBlueprintDuplicate/DoesNotExist_123456");
	const FString DestPath = TEXT("/Game/__MCPTests/BP_DoesNotExist_Clone");

	DeleteIfExists(DestPath + TEXT(".BP_DoesNotExist_Clone"));

	IClaireonTool::FToolResult Result = Tool.Execute(MakeDuplicateArgs(*MissingSource, *DestPath));
	UNTEST_ASSERT_TRUE(Result.bIsError);

	// Error format: "Source not found: <ResolvedSourcePath>" where ResolvedSourcePath
	// is the object-path form produced by ClaireonPathResolver.
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.StartsWith(TEXT("Source not found: ")));
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("DoesNotExist_123456")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Functional_HappyPathDuplicatesPlainBlueprint, UNTEST_TIMEOUTMS(60000))
{
	ClaireonTool_BlueprintDuplicate Tool;

	SweepStaleDestinationFixturesOnce();

	const FString SourcePackage = FString::Printf(TEXT("%s/BP_Plain_Happy"), FixtureFolder);
	const FString SourceObject = SourcePackage + TEXT(".BP_Plain_Happy");
	const FString DestPackage = TEXT("/Game/__MCPTests/BP_Plain_Happy_Clone");
	const FString DestObject = DestPackage + TEXT(".BP_Plain_Happy_Clone");

	// Clean up any previous run.
	DeleteIfExists(DestObject);

	UBlueprint* SourceBP = CreatePlainBlueprintFixture(SourcePackage);
	UNTEST_ASSERT_PTR(SourceBP);

	IClaireonTool::FToolResult Result = Tool.Execute(MakeDuplicateArgs(*SourcePackage, *DestPackage));
	if (Result.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[BlueprintDuplicate] Error: %s"), *Result.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	FString StatusField;
	Result.Data->TryGetStringField(TEXT("status"), StatusField);
	UNTEST_EXPECT_STREQ(*StatusField, TEXT("ok"));

	// Verify asset registry sees the duplicate.
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData DupData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(DestObject));
	UNTEST_EXPECT_TRUE(DupData.IsValid());

	// Verify the .uasset exists on disk.
	const FString DestFileName = FPackageName::LongPackageNameToFilename(
		DestPackage, FPackageName::GetAssetPackageExtension());
	UNTEST_EXPECT_TRUE(FPaths::FileExists(DestFileName));

	// Verify loadable via FSoftObjectPath::TryLoad().
	UObject* LoadedDup = FSoftObjectPath(DestObject).TryLoad();
	UNTEST_EXPECT_PTR(LoadedDup);

	// Cleanup. SourceObject is never saved (CreatePlainBlueprintFixture), so it needs no
	// delete -- only the destination that bp_duplicate wrote to disk does.
	DeleteIfExists(DestObject);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Functional_Branch1AllowlistAcceptsBlueprint, UNTEST_TIMEOUTMS(60000))
{
	ClaireonTool_BlueprintDuplicate Tool;

	SweepStaleDestinationFixturesOnce();

	const FString SourcePackage = FString::Printf(TEXT("%s/BP_Plain_Branch1"), FixtureFolder);
	const FString SourceObject = SourcePackage + TEXT(".BP_Plain_Branch1");
	const FString DestPackage = TEXT("/Game/__MCPTests/BP_Plain_Branch1_Clone");
	const FString DestObject = DestPackage + TEXT(".BP_Plain_Branch1_Clone");

	DeleteIfExists(DestObject);

	UBlueprint* SourceBP = CreatePlainBlueprintFixture(SourcePackage);
	UNTEST_ASSERT_PTR(SourceBP);

	IClaireonTool::FToolResult Result = Tool.Execute(MakeDuplicateArgs(*SourcePackage, *DestPackage));
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	FString AcceptBranch;
	Result.Data->TryGetStringField(TEXT("class_accept_branch"), AcceptBranch);
	UNTEST_EXPECT_STREQ(*AcceptBranch, TEXT("allowlist"));

	FString AssetClass;
	Result.Data->TryGetStringField(TEXT("asset_class"), AssetClass);
	UNTEST_EXPECT_STREQ(*AssetClass, TEXT("Blueprint"));

	// SourceObject is never saved (CreatePlainBlueprintFixture), so it needs no delete.
	DeleteIfExists(DestObject);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Functional_DefaultRenameDependenciesIsFalseInResponse, UNTEST_TIMEOUTMS(60000))
{
	ClaireonTool_BlueprintDuplicate Tool;

	SweepStaleDestinationFixturesOnce();

	const FString SourcePackage = FString::Printf(TEXT("%s/BP_Plain_DefaultRename"), FixtureFolder);
	const FString SourceObject = SourcePackage + TEXT(".BP_Plain_DefaultRename");
	const FString DestPackage = TEXT("/Game/__MCPTests/BP_Plain_DefaultRename_Clone");
	const FString DestObject = DestPackage + TEXT(".BP_Plain_DefaultRename_Clone");

	DeleteIfExists(DestObject);
	UBlueprint* SourceBP = CreatePlainBlueprintFixture(SourcePackage);
	UNTEST_ASSERT_PTR(SourceBP);

	IClaireonTool::FToolResult Result = Tool.Execute(MakeDuplicateArgs(*SourcePackage, *DestPackage));
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	bool bRenameField = true; // start opposite to catch missing field
	UNTEST_ASSERT_TRUE(Result.Data->TryGetBoolField(TEXT("rename_dependencies"), bRenameField));
	UNTEST_EXPECT_FALSE(bRenameField);

	// SourceObject is never saved (CreatePlainBlueprintFixture), so it needs no delete.
	DeleteIfExists(DestObject);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Functional_DestinationAlreadyExistsReturnsStructuredError, UNTEST_TIMEOUTMS(60000))
{
	ClaireonTool_BlueprintDuplicate Tool;

	SweepStaleDestinationFixturesOnce();

	const FString SourcePackage = FString::Printf(TEXT("%s/BP_Plain_DestExists"), FixtureFolder);
	const FString SourceObject = SourcePackage + TEXT(".BP_Plain_DestExists");
	const FString DestPackage = TEXT("/Game/__MCPTests/BP_Plain_DestExists_Clone");
	const FString DestObject = DestPackage + TEXT(".BP_Plain_DestExists_Clone");

	DeleteIfExists(DestObject);

	UBlueprint* SourceBP = CreatePlainBlueprintFixture(SourcePackage);
	UNTEST_ASSERT_PTR(SourceBP);

	// First duplicate succeeds.
	IClaireonTool::FToolResult Result1 = Tool.Execute(MakeDuplicateArgs(*SourcePackage, *DestPackage));
	UNTEST_ASSERT_FALSE(Result1.bIsError);

	// Second duplicate fails with "Destination already exists" error.
	IClaireonTool::FToolResult Result2 = Tool.Execute(MakeDuplicateArgs(*SourcePackage, *DestPackage));
	UNTEST_ASSERT_TRUE(Result2.bIsError);
	UNTEST_EXPECT_TRUE(Result2.ErrorMessage.StartsWith(TEXT("Destination already exists: ")));
	UNTEST_EXPECT_TRUE(Result2.ErrorMessage.Contains(TEXT("class=")));

	// SourceObject is never saved (CreatePlainBlueprintFixture), so it needs no delete.
	DeleteIfExists(DestObject);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Functional_RenameDependenciesTrueEchoesInResponse, UNTEST_TIMEOUTMS(60000))
{
	// Verifies that rename_dependencies=true is reflected in the response payload
	// and the duplication still succeeds (even when there are no self-references
	// to rewrite in a plain fixture, the rewrite pass is a no-op).
	ClaireonTool_BlueprintDuplicate Tool;

	SweepStaleDestinationFixturesOnce();

	const FString SourcePackage = FString::Printf(TEXT("%s/BP_Plain_RenameTrue"), FixtureFolder);
	const FString SourceObject = SourcePackage + TEXT(".BP_Plain_RenameTrue");
	const FString DestPackage = TEXT("/Game/__MCPTests/BP_Plain_RenameTrue_Clone");
	const FString DestObject = DestPackage + TEXT(".BP_Plain_RenameTrue_Clone");

	DeleteIfExists(DestObject);
	UBlueprint* SourceBP = CreatePlainBlueprintFixture(SourcePackage);
	UNTEST_ASSERT_PTR(SourceBP);

	IClaireonTool::FToolResult Result = Tool.Execute(MakeDuplicateArgsWithRename(*SourcePackage, *DestPackage, true));
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	bool bRenameField = false;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetBoolField(TEXT("rename_dependencies"), bRenameField));
	UNTEST_EXPECT_TRUE(bRenameField);

	// SourceObject is never saved (CreatePlainBlueprintFixture), so it needs no delete.
	DeleteIfExists(DestObject);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Functional_AcceptanceCaseMatchesParentProposal, UNTEST_TIMEOUTMS(60000))
{
	// Parent-proposal acceptance case: duplicate a real, on-disk project UBlueprint
	// (auto-discovered) and verify the clone is created. Skip (pass) if the project has
	// no UBlueprint to duplicate.
	//
	// CRASH NOTE: duplicating a Blueprint that references Niagara systems loads those
	// assets into memory, making the end-of-test DeleteIfExists a reproducible trigger for
	// the GetAllReferencersIncludingWeak referencer-scan crash (see the file-level comment
	// and Docs/llm/todo/claireon-untest-harness-reliability.md item 1). bp_duplicate
	// unconditionally saves the destination to disk, so the delete cannot be avoided;
	// SweepStaleDestinationFixturesOnce() clears any crashed-run leftover on the next run.
	SweepStaleDestinationFixturesOnce();

	const FString SourceObject = ClaireonTestAssetDiscovery::FindProjectBlueprintObjectPath();
	if (SourceObject.IsEmpty())
	{
		UE_LOG(LogTemp, Display,
			TEXT("[BlueprintDuplicate] No project UBlueprint available to duplicate; skipping acceptance case."));
		co_return;
	}
	const FString SourcePackage = FPackageName::ObjectPathToPackageName(SourceObject);
	const FString ShortName = FPackageName::GetShortName(SourcePackage);
	const FString DestPackage = FString::Printf(TEXT("/Game/__MCPTests/%s_Clone"), *ShortName);
	const FString DestObject = FString::Printf(TEXT("%s.%s_Clone"), *DestPackage, *ShortName);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	DeleteIfExists(DestObject);

	ClaireonTool_BlueprintDuplicate Tool;
	IClaireonTool::FToolResult Result = Tool.Execute(MakeDuplicateArgs(*SourcePackage, *DestPackage));
	if (Result.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[BlueprintDuplicate] Acceptance case error: %s"), *Result.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	FString StatusField;
	Result.Data->TryGetStringField(TEXT("status"), StatusField);
	UNTEST_EXPECT_STREQ(*StatusField, TEXT("ok"));

	// Assert the duplicate exists via the asset registry.
	FAssetData DupData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(DestObject));
	UNTEST_EXPECT_TRUE(DupData.IsValid());

	DeleteIfExists(DestObject);
	co_return;
}

#endif // WITH_UNTESTED
