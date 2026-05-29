// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Functional tests for asset_check_inner_name_invariant. The
// tool is registry-driven and stateless, so the test covers (a) the
// discoverable surface, (b) the empty-args happy path, (c) the
// invalid-content-path clean result, and (d) the read-only invariant
// via grep-style code-review on the .cpp.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonTool_AssetCheckInnerNameInvariant.h"
#include "Tools/IClaireonTool.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"

// ---------------------------------------------------------------------------
// 1. Discoverable surface: name, description, schema shape.
// ---------------------------------------------------------------------------
UNTEST_UNIT(Claireon, AssetCheckInnerNameInvariant, ExposesDiscoverableSurface)
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
UNTEST_UNIT(Claireon, AssetCheckInnerNameInvariant, EmptyArgsScansGameDefault)
{
	ClaireonTool_AssetCheckInnerNameInvariant Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	IClaireonTool::FToolResult Result = Tool.Execute(Args);

	UNTEST_EXPECT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	int32 MismatchCount = -1;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetNumberField(TEXT("mismatch_count"), MismatchCount));
	UNTEST_EXPECT_TRUE(MismatchCount >= 0);

	int32 ScannedCount = -1;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetNumberField(TEXT("scanned_count"), ScannedCount));
	UNTEST_EXPECT_TRUE(ScannedCount >= 0);

	const TArray<TSharedPtr<FJsonValue>>* Mismatches = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetArrayField(TEXT("mismatches"), Mismatches));

	co_return;
}

// ---------------------------------------------------------------------------
// 3. Invalid contentPath -> zero scanned/mismatched, no error.
// ---------------------------------------------------------------------------
UNTEST_UNIT(Claireon, AssetCheckInnerNameInvariant, InvalidContentPathReturnsCleanResult)
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
UNTEST_UNIT(Claireon, AssetCheckInnerNameInvariant, ReadOnlyByConstruction)
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
