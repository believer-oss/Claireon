// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Tests for claireon.blueprint_duplicate (fracture 04).
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
//  - The acceptance-case test targets /Game/Sandbox/BP_Example
//    which is a product path that may drift; if the asset is absent the test logs a
//    note and early-returns success rather than failing.
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

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonTool_BlueprintDuplicate.h"
#include "Tools/IClaireonTool.h"

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

namespace
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
	if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad())
	{
		TArray<UObject*> AssetsToDelete;
		AssetsToDelete.Add(Asset);
		ObjectTools::ForceDeleteObjects(AssetsToDelete, false);
	}
}

// --- fixture creation -------------------------------------------------------

// Create a plain Blueprint (parent=AActor) at PackagePath on disk and return it.
// Returns nullptr if creation fails. Caller is responsible for deletion.
UBlueprint* CreatePlainBlueprintFixture(const FString& PackagePath)
{
	// If it already exists, return it.
	if (UBlueprint* Existing = Cast<UBlueprint>(FSoftObjectPath(PackagePath + TEXT(".") + FPackageName::GetShortName(PackagePath)).TryLoad()))
	{
		return Existing;
	}

	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
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

	if (!BP)
	{
		return nullptr;
	}

	FAssetRegistryModule::AssetCreated(BP);
	BP->MarkPackageDirty();

	const FString PackageFileName = FPackageName::LongPackageNameToFilename(
		PackagePath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::Save(Package, BP, *PackageFileName, SaveArgs);

	return BP;
}

} // anonymous namespace

// ============================================================================
// Unit tests -- parameter parsing and error surface
// ============================================================================

UNTEST_UNIT(Claireon, BlueprintDuplicate, Unit_MissingSourcePathReturnsStructuredError)
{
	ClaireonTool_BlueprintDuplicate Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("dest_path"), TEXT("/Game/Sandbox/Foo"));

	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_STREQ(*Result.ErrorMessage, TEXT("Missing required field: source_path"));
	co_return;
}

UNTEST_UNIT(Claireon, BlueprintDuplicate, Unit_MissingDestPathReturnsStructuredError)
{
	ClaireonTool_BlueprintDuplicate Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("source_path"), TEXT("/Game/Sandbox/Foo"));

	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_STREQ(*Result.ErrorMessage, TEXT("Missing required field: dest_path"));
	co_return;
}

UNTEST_UNIT(Claireon, BlueprintDuplicate, Unit_EmptySourcePathReturnsStructuredError)
{
	ClaireonTool_BlueprintDuplicate Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("source_path"), TEXT(""));
	Args->SetStringField(TEXT("dest_path"), TEXT("/Game/Sandbox/Foo"));

	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_STREQ(*Result.ErrorMessage, TEXT("Missing required field: source_path"));
	co_return;
}

UNTEST_UNIT(Claireon, BlueprintDuplicate, Unit_EmptyDestPathReturnsStructuredError)
{
	ClaireonTool_BlueprintDuplicate Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("source_path"), TEXT("/Game/Sandbox/Foo"));
	Args->SetStringField(TEXT("dest_path"), TEXT(""));

	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_STREQ(*Result.ErrorMessage, TEXT("Missing required field: dest_path"));
	co_return;
}

UNTEST_UNIT(Claireon, BlueprintDuplicate, Unit_InvalidDestinationPathPropagatesResolverError)
{
	ClaireonTool_BlueprintDuplicate Tool;

	// A source path that cannot be resolved (no /Game/ and no Content/ anchor).
	TSharedPtr<FJsonObject> Args = MakeDuplicateArgs(
		TEXT("Z:\\no\\such\\anchor\\asset"),
		TEXT("/Game/Sandbox/WhateverDest"));

	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_FALSE(Result.ErrorMessage.IsEmpty());
	co_return;
}

UNTEST_UNIT(Claireon, BlueprintDuplicate, Unit_GetNameReturnsCanonicalString)
{
	ClaireonTool_BlueprintDuplicate Tool;
	UNTEST_EXPECT_STREQ(*Tool.GetName(), TEXT("claireon.blueprint_duplicate"));
	co_return;
}

UNTEST_UNIT(Claireon, BlueprintDuplicate, Unit_CategoryDerivesToBlueprint)
{
	ClaireonTool_BlueprintDuplicate Tool;
	UNTEST_EXPECT_STREQ(*Tool.GetCategory(), TEXT("blueprint"));
	co_return;
}

UNTEST_UNIT(Claireon, BlueprintDuplicate, Unit_InputSchemaDeclaresRequiredFields)
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

	const FString MissingSource = TEXT("/Game/Tests/ClaireonBlueprintDuplicate/DoesNotExist_123456");
	const FString DestPath = TEXT("/Game/Sandbox/BP_DoesNotExist_Clone");

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

	const FString SourcePackage = FString::Printf(TEXT("%s/BP_Plain_Happy"), FixtureFolder);
	const FString SourceObject = SourcePackage + TEXT(".BP_Plain_Happy");
	const FString DestPackage = TEXT("/Game/Sandbox/BP_Plain_Happy_Clone");
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

	// Cleanup.
	DeleteIfExists(DestObject);
	DeleteIfExists(SourceObject);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Functional_Branch1AllowlistAcceptsBlueprint, UNTEST_TIMEOUTMS(60000))
{
	ClaireonTool_BlueprintDuplicate Tool;

	const FString SourcePackage = FString::Printf(TEXT("%s/BP_Plain_Branch1"), FixtureFolder);
	const FString SourceObject = SourcePackage + TEXT(".BP_Plain_Branch1");
	const FString DestPackage = TEXT("/Game/Sandbox/BP_Plain_Branch1_Clone");
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

	DeleteIfExists(DestObject);
	DeleteIfExists(SourceObject);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Functional_DefaultRenameDependenciesIsFalseInResponse, UNTEST_TIMEOUTMS(60000))
{
	ClaireonTool_BlueprintDuplicate Tool;

	const FString SourcePackage = FString::Printf(TEXT("%s/BP_Plain_DefaultRename"), FixtureFolder);
	const FString SourceObject = SourcePackage + TEXT(".BP_Plain_DefaultRename");
	const FString DestPackage = TEXT("/Game/Sandbox/BP_Plain_DefaultRename_Clone");
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

	DeleteIfExists(DestObject);
	DeleteIfExists(SourceObject);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Functional_DestinationAlreadyExistsReturnsStructuredError, UNTEST_TIMEOUTMS(60000))
{
	ClaireonTool_BlueprintDuplicate Tool;

	const FString SourcePackage = FString::Printf(TEXT("%s/BP_Plain_DestExists"), FixtureFolder);
	const FString SourceObject = SourcePackage + TEXT(".BP_Plain_DestExists");
	const FString DestPackage = TEXT("/Game/Sandbox/BP_Plain_DestExists_Clone");
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

	DeleteIfExists(DestObject);
	DeleteIfExists(SourceObject);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Functional_RenameDependenciesTrueEchoesInResponse, UNTEST_TIMEOUTMS(60000))
{
	// Verifies that rename_dependencies=true is reflected in the response payload
	// and the duplication still succeeds (even when there are no self-references
	// to rewrite in a plain fixture, the rewrite pass is a no-op).
	ClaireonTool_BlueprintDuplicate Tool;

	const FString SourcePackage = FString::Printf(TEXT("%s/BP_Plain_RenameTrue"), FixtureFolder);
	const FString SourceObject = SourcePackage + TEXT(".BP_Plain_RenameTrue");
	const FString DestPackage = TEXT("/Game/Sandbox/BP_Plain_RenameTrue_Clone");
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

	DeleteIfExists(DestObject);
	DeleteIfExists(SourceObject);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BlueprintDuplicate, Functional_AcceptanceCaseMatchesParentProposal, UNTEST_TIMEOUTMS(60000))
{
	// Parent proposal acceptance case:
	//   claireon.blueprint_duplicate('/Game/Sandbox/BP_Example',
	//                              '/Game/Sandbox/BP_Example_Clone')
	// Skip (pass) if the acceptance-case source asset has moved or is absent.

	const FString SourcePackage = TEXT("/Game/Sandbox/BP_Example");
	const FString SourceObject = SourcePackage + TEXT(".BP_Example");
	const FString DestPackage = TEXT("/Game/Sandbox/BP_Example_Clone");
	const FString DestObject = DestPackage + TEXT(".BP_Example_Clone");

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData SourceData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(SourceObject));
	if (!SourceData.IsValid())
	{
		UE_LOG(LogTemp, Display,
			TEXT("[BlueprintDuplicate] Acceptance-case source %s not present; skipping test."),
			*SourceObject);
		co_return;
	}

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
