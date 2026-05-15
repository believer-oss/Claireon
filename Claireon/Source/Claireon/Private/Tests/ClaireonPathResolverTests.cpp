// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonPathResolver.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"

// ===========================================================================
// Basic Format Tests
// ===========================================================================

UNTEST_UNIT(Claireon, PathResolver, CanonicalGamePath)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("/Game/Path/Asset"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/Path/Asset.Asset"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/Path/Asset"));
	UNTEST_EXPECT_EQ(Result.ResolvedPath.Kind, ClaireonPathResolver::EPathKind::PackagePath);
	UNTEST_EXPECT_FALSE(Result.ResolvedPath.bIsClassReference);
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, GamePathWithUasset)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("/Game/Path/Asset.uasset"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/Path/Asset.Asset"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/Path/Asset"));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, GamePathWithUmap)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("/Game/Path/Asset.umap"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/Path/Asset.Asset"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/Path/Asset"));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, DuplicateObjectName)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("/Game/Path/Asset.Asset"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/Path/Asset.Asset"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/Path/Asset"));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, NonDuplicateObjectName)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("/Game/Path/Asset.SubObject"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/Path/Asset.SubObject"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/Path/Asset.SubObject"));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, ContentRelativePath)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("Content/Path/Asset"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/Path/Asset.Asset"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/Path/Asset"));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, ContentRelativeWithExtension)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("Content/Path/Asset.uasset"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/Path/Asset.Asset"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/Path/Asset"));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, AbsoluteFilesystemPath)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("D:/proj/Content/Path/Asset.uasset"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/Path/Asset.Asset"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/Path/Asset"));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, UnixAbsoluteFilesystemPath)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("/home/user/proj/Content/Path/Asset"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/Path/Asset.Asset"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/Path/Asset"));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, GameWithoutLeadingSlash)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("Game/Path/Asset"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/Path/Asset.Asset"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/Path/Asset"));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, BareRelativePath)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("Path/Asset"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/Path/Asset.Asset"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/Path/Asset"));
	co_return;
}

// ===========================================================================
// Special Path Tests
// ===========================================================================

UNTEST_UNIT(Claireon, PathResolver, ScriptEnginePath)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("/Script/Engine.Actor"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Script/Engine.Actor"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Script/Engine.Actor"));
	UNTEST_EXPECT_EQ(Result.ResolvedPath.Kind, ClaireonPathResolver::EPathKind::NativeClassPath);
	UNTEST_EXPECT_FALSE(Result.ResolvedPath.bIsClassReference);
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, ScriptCustomModule)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("/Script/MyModule.MyClass"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Script/MyModule.MyClass"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Script/MyModule.MyClass"));
	UNTEST_EXPECT_EQ(Result.ResolvedPath.Kind, ClaireonPathResolver::EPathKind::NativeClassPath);
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, EngineContentPath)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("/Engine/BasicShapes/Cube"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Engine/BasicShapes/Cube"));
	UNTEST_EXPECT_EQ(Result.ResolvedPath.Kind, ClaireonPathResolver::EPathKind::PackagePath);
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, PluginContentPath)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("/PluginName/SubPath"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_EQ(Result.ResolvedPath.Kind, ClaireonPathResolver::EPathKind::PackagePath);
	co_return;
}

// ===========================================================================
// _C Handling Tests (game thread)
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, PathResolver, ClassSuffix_AssetExists, UNTEST_TIMEOUTMS(5000))
{
	// Register a temporary asset in the registry at the stripped path
	UPackage* TestPackage = CreatePackage(TEXT("/Game/ClaireonTest/BP_TestClassSuffix"));
	UBlueprint* TestBP = NewObject<UBlueprint>(TestPackage, TEXT("BP_TestClassSuffix"), RF_Public | RF_Standalone);
	TestBP->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(TestBP);

	auto Result = ClaireonPathResolver::Resolve(TEXT("/Game/ClaireonTest/BP_TestClassSuffix_C"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/ClaireonTest/BP_TestClassSuffix.BP_TestClassSuffix"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/ClaireonTest/BP_TestClassSuffix"));
	UNTEST_EXPECT_TRUE(Result.ResolvedPath.bIsClassReference);

	// Cleanup
	FAssetRegistryModule::AssetDeleted(TestBP);
	TestBP->MarkAsGarbage();
	TestPackage->MarkAsGarbage();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PathResolver, ClassSuffix_ExactAssetExists, UNTEST_TIMEOUTMS(5000))
{
	// Register a temporary asset at the _C path itself
	UPackage* TestPackage = CreatePackage(TEXT("/Game/ClaireonTest/BP_NPC_Guard_C"));
	UBlueprint* TestBP = NewObject<UBlueprint>(TestPackage, TEXT("BP_NPC_Guard_C"), RF_Public | RF_Standalone);
	TestBP->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(TestBP);

	auto Result = ClaireonPathResolver::Resolve(TEXT("/Game/ClaireonTest/BP_NPC_Guard_C"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/ClaireonTest/BP_NPC_Guard_C.BP_NPC_Guard_C"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/ClaireonTest/BP_NPC_Guard_C"));
	UNTEST_EXPECT_FALSE(Result.ResolvedPath.bIsClassReference);

	// Cleanup
	FAssetRegistryModule::AssetDeleted(TestBP);
	TestBP->MarkAsGarbage();
	TestPackage->MarkAsGarbage();
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, ClassSuffix_NeitherExists)
{
	// No assets registered at either path -- best-effort strip
	auto Result = ClaireonPathResolver::Resolve(TEXT("/Game/ClaireonTest/BP_NoSuchAsset_C"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/ClaireonTest/BP_NoSuchAsset.BP_NoSuchAsset"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/ClaireonTest/BP_NoSuchAsset"));
	UNTEST_EXPECT_TRUE(Result.ResolvedPath.bIsClassReference);
	co_return;
}

// ===========================================================================
// Normalization Edge Case Tests
// ===========================================================================

UNTEST_UNIT(Claireon, PathResolver, EmptyString)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT(""));
	UNTEST_ASSERT_FALSE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Result.Error.Contains(TEXT("empty"), ESearchCase::IgnoreCase));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, WhitespaceOnly)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("   "));
	UNTEST_ASSERT_FALSE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Result.Error.Contains(TEXT("empty"), ESearchCase::IgnoreCase));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, OnlyUassetExtension)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT(".uasset"));
	UNTEST_ASSERT_FALSE(Result.bSuccess);
	UNTEST_EXPECT_TRUE(Result.Error.Contains(TEXT("extension"), ESearchCase::IgnoreCase));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, BackslashPaths)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("\\Game\\Path\\Asset"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/Path/Asset.Asset"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/Path/Asset"));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, DoubleSlashes)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("/Game//Path/Asset"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/Path/Asset.Asset"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/Path/Asset"));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, TrailingSlash)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("/Game/Path/"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/Path.Path"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/Path"));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, MultipleDotsDuplicate)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("/Game/BP_V2.0.BP_V2.0"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	// Step 5 strips duplicate object-name portion down to "BP_V2.0"; the final
	// segment already contains a dot, so step 12.5 does not append further.
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/BP_V2.0"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/BP_V2.0"));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, ContentFolder)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("Content"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game.Game"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game"));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, UnicodeInPath)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("/Game/Characters/BP_Player_e"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/Characters/BP_Player_e.BP_Player_e"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/Characters/BP_Player_e"));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, LeadingWhitespace)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("  /Game/Path/Asset  "));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/Path/Asset.Asset"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/Path/Asset"));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, DotDotResolution)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("/Game/Path/../Other/Asset"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/Other/Asset.Asset"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/Other/Asset"));
	co_return;
}

// ===========================================================================
// Stage 0 Object-Path Append Tests (F01)
// ===========================================================================

UNTEST_UNIT(Claireon, PathResolver, ObjectNameAppendedForPackagePath)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("/Game/Foo/BP_Bar"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/Foo/BP_Bar.BP_Bar"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/Foo/BP_Bar"));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, ObjectNameNotAppendedForSubObjectPath)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("/Game/Foo/BP_Bar.SubObject"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Game/Foo/BP_Bar.SubObject"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Game/Foo/BP_Bar.SubObject"));
	co_return;
}

UNTEST_UNIT(Claireon, PathResolver, ObjectNameNotAppendedForNativeClassPath)
{
	auto Result = ClaireonPathResolver::Resolve(TEXT("/Script/Engine.Actor"));
	UNTEST_ASSERT_TRUE(Result.bSuccess);
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.Path, TEXT("/Script/Engine.Actor"));
	UNTEST_EXPECT_STREQ(*Result.ResolvedPath.PackagePath, TEXT("/Script/Engine.Actor"));
	co_return;
}

#endif // WITH_UNTESTED
