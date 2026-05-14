// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Functional tests for apply_spec_help (P2). Validates that the
// tool loads ApplySpecCatalog.json correctly, returns the expected entry
// shape, every cataloged tool name is also a registered Claireon tool, and
// the loader reads from disk freshly each invocation.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonTool_ApplySpecHelp.h"
#include "Tools/IClaireonTool.h"
#include "IClaireonToolProvider.h"
#include "Features/IModularFeatures.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

namespace ApplySpecHelpTestHelpers
{
	TSet<FString> CollectRegisteredToolNames()
	{
		TSet<FString> Names;
		TArray<IClaireonToolProvider*> Providers = IModularFeatures::Get()
			.GetModularFeatureImplementations<IClaireonToolProvider>(IClaireonToolProvider::FeatureName);
		for (IClaireonToolProvider* Provider : Providers)
		{
			if (!Provider) { continue; }
			for (const TSharedPtr<IClaireonTool>& Tool : Provider->GetTools())
			{
				if (Tool.IsValid())
				{
					Names.Add(Tool->GetName());
				}
			}
		}
		return Names;
	}

	FString ResolveCatalogPath()
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Claireon"));
		if (!Plugin.IsValid()) { return FString(); }
		return FPaths::Combine(Plugin->GetContentDir(), TEXT("ApplySpecCatalog.json"));
	}
}

// ---------------------------------------------------------------------------
// Step 1+2+3: Load + entry count + field shape
// ---------------------------------------------------------------------------
UNTEST_UNIT(Claireon, ApplySpecHelp, LoadsAndReturnsEightEntries)
{
	ClaireonTool_ApplySpecHelp Tool;
	auto Result = Tool.Execute(MakeShared<FJsonObject>());

	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	int32 EntryCount = 0;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetNumberField(TEXT("entry_count"), EntryCount));
	UNTEST_EXPECT_EQ(EntryCount, 8);

	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetArrayField(TEXT("entries"), Entries));
	UNTEST_ASSERT_TRUE(Entries != nullptr);
	UNTEST_EXPECT_EQ(Entries->Num(), 8);

	// Every entry must have all six load-bearing keys.
	const TArray<FString> RequiredKeys = {
		TEXT("tool"),
		TEXT("spec_entry_types"),
		TEXT("calling_convention"),
		TEXT("creates_asset_if_missing"),
		TEXT("id_mapping_pattern"),
		TEXT("gotchas")
	};

	for (int32 i = 0; i < Entries->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>& Entry = (*Entries)[i]->AsObject();
		UNTEST_ASSERT_TRUE(Entry.IsValid());

		for (const FString& Key : RequiredKeys)
		{
			if (!Entry->HasField(Key))
			{
				UE_LOG(LogTemp, Error, TEXT("[ApplySpecHelp] Entry %d missing required key '%s'"), i, *Key);
				UNTEST_EXPECT_TRUE(false);
			}
		}
	}

	co_return;
}

// ---------------------------------------------------------------------------
// Step 4: Cross-validation -- every cataloged tool name must be registered.
// ---------------------------------------------------------------------------
UNTEST_UNIT(Claireon, ApplySpecHelp, CatalogToolsMatchRegisteredTools)
{
	using namespace ApplySpecHelpTestHelpers;

	ClaireonTool_ApplySpecHelp Tool;
	auto Result = Tool.Execute(MakeShared<FJsonObject>());
	UNTEST_ASSERT_FALSE(Result.bIsError);

	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetArrayField(TEXT("entries"), Entries));

	const TSet<FString> RegisteredNames = CollectRegisteredToolNames();
	UNTEST_ASSERT_TRUE(RegisteredNames.Num() > 0);

	for (const TSharedPtr<FJsonValue>& Val : *Entries)
	{
		const TSharedPtr<FJsonObject>& Entry = Val->AsObject();
		UNTEST_ASSERT_TRUE(Entry.IsValid());
		FString CataloggedTool;
		UNTEST_ASSERT_TRUE(Entry->TryGetStringField(TEXT("tool"), CataloggedTool));
		if (!RegisteredNames.Contains(CataloggedTool))
		{
			UE_LOG(LogTemp, Error,
				TEXT("[ApplySpecHelp] Catalog tool '%s' is not in the registered tool set"),
				*CataloggedTool);
			UNTEST_EXPECT_TRUE(false);
		}
	}
	co_return;
}

// ---------------------------------------------------------------------------
// Step 5: Verbatim spec_entry_types match for the three documented spec-rich tools.
// ---------------------------------------------------------------------------
UNTEST_UNIT(Claireon, ApplySpecHelp, SpecEntryTypesMatchMarkdownTable)
{
	ClaireonTool_ApplySpecHelp Tool;
	auto Result = Tool.Execute(MakeShared<FJsonObject>());
	UNTEST_ASSERT_FALSE(Result.bIsError);

	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetArrayField(TEXT("entries"), Entries));

	TMap<FString, TArray<FString>> ByTool;
	for (const TSharedPtr<FJsonValue>& Val : *Entries)
	{
		const TSharedPtr<FJsonObject>& Entry = Val->AsObject();
		UNTEST_ASSERT_TRUE(Entry.IsValid());
		FString ToolName;
		UNTEST_ASSERT_TRUE(Entry->TryGetStringField(TEXT("tool"), ToolName));

		const TArray<TSharedPtr<FJsonValue>>* TypesJson = nullptr;
		UNTEST_ASSERT_TRUE(Entry->TryGetArrayField(TEXT("spec_entry_types"), TypesJson));
		TArray<FString> Types;
		for (const TSharedPtr<FJsonValue>& T : *TypesJson)
		{
			Types.Add(T->AsString());
		}
		ByTool.Add(ToolName, MoveTemp(Types));
	}

	// Cross-reference the per-tool apply_spec table.
	{
		const TArray<FString>* Got = ByTool.Find(TEXT("behaviortree_edit"));
		UNTEST_ASSERT_TRUE(Got != nullptr);
		UNTEST_ASSERT_EQ(Got->Num(), 1);
		UNTEST_EXPECT_EQ((*Got)[0], FString(TEXT("nodes[]")));
	}
	{
		const TArray<FString>* Got = ByTool.Find(TEXT("blueprint_edit_graph"));
		UNTEST_ASSERT_TRUE(Got != nullptr);
		UNTEST_ASSERT_EQ(Got->Num(), 3);
		UNTEST_EXPECT_TRUE(Got->Contains(TEXT("nodes[]")));
		UNTEST_EXPECT_TRUE(Got->Contains(TEXT("connections[]")));
		UNTEST_EXPECT_TRUE(Got->Contains(TEXT("variables[]")));
	}
	{
		const TArray<FString>* Got = ByTool.Find(TEXT("statetree_edit"));
		UNTEST_ASSERT_TRUE(Got != nullptr);
		UNTEST_ASSERT_TRUE(Got->Num() >= 1);
		UNTEST_EXPECT_TRUE(Got->Contains(TEXT("states[]")));
	}

	co_return;
}

// ---------------------------------------------------------------------------
// Error-path coverage: missing catalog file returns clean error, no crash.
// ---------------------------------------------------------------------------
UNTEST_UNIT(Claireon, ApplySpecHelp, MissingCatalogReturnsCleanError)
{
	using namespace ApplySpecHelpTestHelpers;

	const FString CatalogPath = ResolveCatalogPath();
	if (CatalogPath.IsEmpty() || !FPaths::FileExists(CatalogPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("[ApplySpecHelp] Catalog path unresolvable; skipping error-path test"));
		co_return;
	}

	const FString TempPath = CatalogPath + TEXT(".bak");

	// Move catalog aside.
	IFileManager::Get().Move(*TempPath, *CatalogPath, /*bReplace=*/true, /*bEvenIfReadOnly=*/false);
	bool bMoved = !FPaths::FileExists(CatalogPath) && FPaths::FileExists(TempPath);

	if (bMoved)
	{
		ClaireonTool_ApplySpecHelp Tool;
		auto Result = Tool.Execute(MakeShared<FJsonObject>());

		// Restore before any assertion can early-exit.
		IFileManager::Get().Move(*CatalogPath, *TempPath, /*bReplace=*/true, /*bEvenIfReadOnly=*/false);

		UNTEST_EXPECT_TRUE(Result.bIsError);
		UNTEST_EXPECT_FALSE(Result.GetContentAsString().IsEmpty());
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[ApplySpecHelp] Could not move catalog aside; skipping error-path test"));
	}

	co_return;
}

// ---------------------------------------------------------------------------
// Round-trip: edit the catalog on disk, reload, verify the marker is visible.
// Confirms the loader reads from disk freshly each invocation.
// ---------------------------------------------------------------------------
UNTEST_UNIT(Claireon, ApplySpecHelp, RoundTripReadsFreshFromDisk)
{
	using namespace ApplySpecHelpTestHelpers;

	const FString CatalogPath = ResolveCatalogPath();
	if (CatalogPath.IsEmpty() || !FPaths::FileExists(CatalogPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("[ApplySpecHelp] Catalog path unresolvable; skipping round-trip test"));
		co_return;
	}

	FString OriginalRaw;
	UNTEST_ASSERT_TRUE(FFileHelper::LoadFileToString(OriginalRaw, *CatalogPath));

	const FString Marker = TEXT("__roundtrip_marker_apply_spec_help__");
	// Inject a marker by replacing the very first occurrence of the gotchas key
	// with one that includes the marker as an extra string.
	FString Modified = OriginalRaw;
	const FString InjectionAnchor = TEXT("\"gotchas\": [");
	const FString InjectionReplacement = FString::Printf(TEXT("\"gotchas\": [\"%s\","), *Marker);
	const int32 AnchorIdx = Modified.Find(InjectionAnchor);
	if (AnchorIdx == INDEX_NONE)
	{
		UE_LOG(LogTemp, Warning, TEXT("[ApplySpecHelp] Could not find injection anchor; skipping round-trip"));
		co_return;
	}
	Modified = Modified.Left(AnchorIdx) + InjectionReplacement + Modified.RightChop(AnchorIdx + InjectionAnchor.Len());

	UNTEST_ASSERT_TRUE(FFileHelper::SaveStringToFile(Modified, *CatalogPath));

	ClaireonTool_ApplySpecHelp Tool;
	auto Result = Tool.Execute(MakeShared<FJsonObject>());

	// Restore original before assertions can early-exit.
	const bool bRestored = FFileHelper::SaveStringToFile(OriginalRaw, *CatalogPath);

	UNTEST_ASSERT_FALSE(Result.bIsError);
	const FString Serialized = Result.GetContentAsString();
	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetArrayField(TEXT("entries"), Entries));

	// Walk all entries' gotchas[] looking for the marker.
	bool bMarkerFound = false;
	for (const TSharedPtr<FJsonValue>& EntryVal : *Entries)
	{
		const TSharedPtr<FJsonObject>& Entry = EntryVal->AsObject();
		if (!Entry.IsValid()) { continue; }
		const TArray<TSharedPtr<FJsonValue>>* Gotchas = nullptr;
		if (!Entry->TryGetArrayField(TEXT("gotchas"), Gotchas)) { continue; }
		for (const TSharedPtr<FJsonValue>& G : *Gotchas)
		{
			if (G->AsString() == Marker)
			{
				bMarkerFound = true;
				break;
			}
		}
		if (bMarkerFound) { break; }
	}

	UNTEST_EXPECT_TRUE(bMarkerFound);
	UNTEST_EXPECT_TRUE(bRestored);

	co_return;
}

#endif // WITH_UNTESTED
