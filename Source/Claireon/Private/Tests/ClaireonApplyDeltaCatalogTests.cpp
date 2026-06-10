// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Tests for apply_delta across all families.
//
// These tests assert the structural invariants of the `apply_delta`
// field present on every ApplySpecCatalog.json entry.
//
// Invariants asserted:
//   (a) Every catalog entry has an `apply_delta` object with a boolean
//       `supported` field. supported==true entries also carry `tool`
//       (matching the registered <family>_apply_delta tool name) and
//       a non-empty `supported_phases` subset of
//       ["disconnect","remove_nodes","nodes","connect"]. supported==false
//       entries carry a non-empty `reason` string.
//   (b) Catalog-apply_delta-matches-registered (bidirectional): every
//       registered Claireon tool whose GetOperation() == "apply_delta"
//       MUST appear as apply_delta.tool on exactly one catalog entry,
//       and the catalog key MUST equal the tool's GetCategory().
//       Conversely, every catalog entry with apply_delta.supported==true
//       MUST name a registered <family>_apply_delta tool.
//   (c) apply_spec/apply_delta scope agreement: when a catalog entry's
//       apply_delta.supported==true, its `tool` and the entry's top-level
//       `tool` (the apply_spec tool name) MUST belong to the same
//       category (the catalog key).
//   (d) _meta bookkeeping: schema_version >= 3 (the bump that added
//       apply_delta), entry_count equals the actual non-meta key count.

#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tools/IClaireonTool.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "SquidTasks/Task.h"

namespace ClaireonApplyDeltaCatalogTestsNS
{
	// File-local discriminator per feedback_anon_namespace_unity_collision.md
	// (matches existing convention in ClaireonToolSearchExecuteTests etc.).

	static TSharedPtr<FJsonObject> AdCat_LoadCatalogForTests()
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Claireon"));
		if (!Plugin.IsValid()) { return nullptr; }
		const FString Path = FPaths::Combine(Plugin->GetContentDir(),
			TEXT("ApplySpecCatalog.json"));
		FString Raw;
		if (!FFileHelper::LoadFileToString(Raw, *Path)) { return nullptr; }
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return nullptr;
		}
		return Root;
	}

	// (catalog_key, registered tool name) for the 8 in-scope apply_delta
	// families, plus the 9th bp_apply_delta from the bp family. The catalog
	// row for bp lists bp_apply_delta even though the registration lives in
	// ClaireonTool_ApplyBlueprintDelta.
	static const TArray<TPair<FString, FString>>& AdCat_GetExpectedDeltaPairs()
	{
		static const TArray<TPair<FString, FString>> Pairs = {
			{ TEXT("behaviortree"),   TEXT("behaviortree_apply_delta") },
			{ TEXT("bp"),             TEXT("bp_apply_delta") },
			{ TEXT("eqs"),            TEXT("eqs_apply_delta") },
			{ TEXT("level_sequence"), TEXT("level_sequence_apply_delta") },
			{ TEXT("material"),       TEXT("material_apply_delta") },
			{ TEXT("niagara"),        TEXT("niagara_apply_delta") },
			{ TEXT("pcg"),            TEXT("pcg_apply_delta") },
			{ TEXT("statetree"),      TEXT("statetree_apply_delta") },
			{ TEXT("widgetbp"),       TEXT("widgetbp_apply_delta") },
		};
		return Pairs;
	}

	static const TSet<FString>& AdCat_ValidPhases()
	{
		static const TSet<FString> Phases = {
			TEXT("disconnect"),
			TEXT("remove_nodes"),
			TEXT("nodes"),
			TEXT("connect"),
		};
		return Phases;
	}
}

// ===========================================================================
// (a) Every catalog entry has an apply_delta object with a boolean
//     `supported` field. supported==true requires tool + supported_phases;
//     supported==false requires reason.
// ===========================================================================
UNTEST_UNIT_OPTS(Claireon, ApplyDeltaCatalog, EveryEntryHasApplyDeltaShape, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonApplyDeltaCatalogTestsNS;

	TSharedPtr<FJsonObject> Catalog = AdCat_LoadCatalogForTests();
	UNTEST_ASSERT_TRUE(Catalog.IsValid());

	const TSet<FString>& ValidPhases = AdCat_ValidPhases();

	int32 EntriesChecked = 0;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : Catalog->Values)
	{
		if (KV.Key.StartsWith(TEXT("_"))) { continue; }
		const TSharedPtr<FJsonObject>* EntryObj = nullptr;
		UNTEST_ASSERT_TRUE(KV.Value->TryGetObject(EntryObj));
		UNTEST_ASSERT_TRUE(EntryObj && (*EntryObj).IsValid());

		const TSharedPtr<FJsonObject>* ApplyDeltaObj = nullptr;
		UNTEST_ASSERT_TRUE((*EntryObj)->TryGetObjectField(TEXT("apply_delta"), ApplyDeltaObj));
		UNTEST_ASSERT_TRUE(ApplyDeltaObj && (*ApplyDeltaObj).IsValid());

		bool bSupported = false;
		UNTEST_ASSERT_TRUE((*ApplyDeltaObj)->TryGetBoolField(TEXT("supported"), bSupported));

		if (bSupported)
		{
			FString ToolName;
			UNTEST_EXPECT_TRUE((*ApplyDeltaObj)->TryGetStringField(TEXT("tool"), ToolName));
			UNTEST_EXPECT_FALSE(ToolName.IsEmpty());

			const TArray<TSharedPtr<FJsonValue>>* PhasesArr = nullptr;
			UNTEST_EXPECT_TRUE((*ApplyDeltaObj)->TryGetArrayField(TEXT("supported_phases"), PhasesArr));
			UNTEST_EXPECT_TRUE(PhasesArr && PhasesArr->Num() > 0);
			if (PhasesArr)
			{
				for (const TSharedPtr<FJsonValue>& Val : *PhasesArr)
				{
					FString Phase;
					UNTEST_EXPECT_TRUE(Val->TryGetString(Phase));
					UNTEST_EXPECT_TRUE(ValidPhases.Contains(Phase));
				}
			}
		}
		else
		{
			FString Reason;
			UNTEST_EXPECT_TRUE((*ApplyDeltaObj)->TryGetStringField(TEXT("reason"), Reason));
			UNTEST_EXPECT_FALSE(Reason.IsEmpty());
		}

		++EntriesChecked;
	}
	// We expect the 17 catalog entries to be present.
	UNTEST_EXPECT_EQ(EntriesChecked, 17);

	co_return;
}

// ===========================================================================
// (b) Catalog <-> registered apply_delta tools agree bidirectionally.
//
// Must ensure the Claireon server is running before querying the modular
// feature registry: in commandlet mode (Invoke-UntestTests.ps1) the server is
// not auto-started, so the providers list is empty until StartServer() runs.
// This matches the pattern in ClaireonToolSearchExecuteTests::EnsureServer.
// ===========================================================================
UNTEST_UNIT_OPTS(Claireon, ApplyDeltaCatalog, CatalogMatchesRegisteredApplyDeltaTools, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonApplyDeltaCatalogTestsNS;

	// The server registry is only populated when StartupModule() runs (full
	// editor / PIE). Under the commandlet-mode runner used by
	// Invoke-UntestTests.ps1, the Claireon module's StartupModule is skipped
	// and FClaireonModule::GetServer() returns nullptr -- the same
	// environment quirk affects main's
	// ClaireonToolSearchExecuteTests::CatalogToolsMatchRegisteredTools test.
	// When the server is unavailable, we still verify the catalog half (every
	// supported:true entry names a well-formed <category>_apply_delta wire
	// name) but skip the bidirectional registry comparison.
	FClaireonModule& Module = FClaireonModule::Get();
	const bool bWeStartedServer = !Module.IsServerRunning();
	if (bWeStartedServer)
	{
		Module.StartServer();
	}
	FClaireonServer* Server = Module.GetServer();

	TSharedPtr<FJsonObject> Catalog = AdCat_LoadCatalogForTests();
	UNTEST_ASSERT_TRUE(Catalog.IsValid());

	// Walk the catalog and collect every entry with apply_delta.supported==true.
	TMap<FString, FString> CatalogDeltaTools; // catalog_key -> apply_delta.tool
	for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : Catalog->Values)
	{
		if (KV.Key.StartsWith(TEXT("_"))) { continue; }
		const TSharedPtr<FJsonObject>* EntryObj = nullptr;
		if (!KV.Value->TryGetObject(EntryObj) || !EntryObj || !(*EntryObj).IsValid()) { continue; }

		const TSharedPtr<FJsonObject>* ApplyDeltaObj = nullptr;
		if (!(*EntryObj)->TryGetObjectField(TEXT("apply_delta"), ApplyDeltaObj)
			|| !ApplyDeltaObj || !(*ApplyDeltaObj).IsValid())
		{
			continue;
		}
		bool bSupported = false;
		if (!(*ApplyDeltaObj)->TryGetBoolField(TEXT("supported"), bSupported) || !bSupported) { continue; }

		FString ToolName;
		if ((*ApplyDeltaObj)->TryGetStringField(TEXT("tool"), ToolName))
		{
			CatalogDeltaTools.Add(KV.Key, ToolName);
		}
	}

	// Always verify the catalog half: every supported:true entry MUST name
	// a wire-name of the shape "<catalog_key>_apply_delta", and the expected
	// 9-pair list MUST be a subset of the catalog claims (catches drift even
	// when the registry can't be queried).
	const TArray<TPair<FString, FString>>& Expected = AdCat_GetExpectedDeltaPairs();
	UNTEST_EXPECT_EQ(CatalogDeltaTools.Num(), Expected.Num());
	for (const TPair<FString, FString>& E : Expected)
	{
		const FString* CatalogName = CatalogDeltaTools.Find(E.Key);
		UNTEST_EXPECT_PTR(CatalogName);
		if (CatalogName)
		{
			UNTEST_EXPECT_EQ(*CatalogName, E.Value);
		}
	}

	// If the server is available (full editor / PIE), additionally verify the
	// bidirectional invariant between catalog and registered tools. Under
	// commandlet-mode the server is null (StartupModule does not run); skip
	// the registry half rather than fail the test, mirroring the limitation
	// of main's CatalogToolsMatchRegisteredTools.
	if (Server)
	{
		TMap<FString, FString> RegisteredDeltaTools; // category -> wire-name
		for (const TPair<FString, TSharedPtr<IClaireonTool>>& KV : Server->GetTools())
		{
			if (!KV.Value.IsValid()) { continue; }
			if (KV.Value->GetOperation() != TEXT("apply_delta")) { continue; }
			RegisteredDeltaTools.Add(KV.Value->GetCategory(), KV.Key);
		}

		// (a) catalog -> registered.
		for (const TPair<FString, FString>& KV : CatalogDeltaTools)
		{
			const FString* RegisteredName = RegisteredDeltaTools.Find(KV.Key);
			UNTEST_EXPECT_PTR(RegisteredName);
			if (RegisteredName)
			{
				UNTEST_EXPECT_EQ(*RegisteredName, KV.Value);
			}
		}
		// (b) registered -> catalog.
		for (const TPair<FString, FString>& KV : RegisteredDeltaTools)
		{
			const FString* CatalogName = CatalogDeltaTools.Find(KV.Key);
			UNTEST_EXPECT_PTR(CatalogName);
			if (CatalogName)
			{
				UNTEST_EXPECT_EQ(*CatalogName, KV.Value);
			}
		}
		UNTEST_EXPECT_EQ(RegisteredDeltaTools.Num(), Expected.Num());
	}

	if (bWeStartedServer)
	{
		Module.StopServer();
	}
	co_return;
}

// ===========================================================================
// (c) apply_spec and apply_delta agree on per-family scope (catalog key).
// ===========================================================================
UNTEST_UNIT_OPTS(Claireon, ApplyDeltaCatalog, ApplySpecAndApplyDeltaAgreeOnFamily, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonApplyDeltaCatalogTestsNS;

	TSharedPtr<FJsonObject> Catalog = AdCat_LoadCatalogForTests();
	UNTEST_ASSERT_TRUE(Catalog.IsValid());

	for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : Catalog->Values)
	{
		if (KV.Key.StartsWith(TEXT("_"))) { continue; }
		const TSharedPtr<FJsonObject>* EntryObj = nullptr;
		UNTEST_ASSERT_TRUE(KV.Value->TryGetObject(EntryObj));
		UNTEST_ASSERT_TRUE(EntryObj && (*EntryObj).IsValid());

		// Top-level `tool` is the apply_spec / instance_apply_spec wire name.
		FString SpecTool;
		UNTEST_ASSERT_TRUE((*EntryObj)->TryGetStringField(TEXT("tool"), SpecTool));
		// Spec tool must start with "<catalog_key>_" so the family-scope check
		// below is unambiguous. material_instance is the documented exception:
		// composed name is "material_instance_instance_apply_spec".
		const FString ExpectedPrefix = KV.Key + TEXT("_");
		UNTEST_EXPECT_TRUE(SpecTool.StartsWith(ExpectedPrefix));

		const TSharedPtr<FJsonObject>* ApplyDeltaObj = nullptr;
		UNTEST_ASSERT_TRUE((*EntryObj)->TryGetObjectField(TEXT("apply_delta"), ApplyDeltaObj));
		bool bSupported = false;
		if (!(*ApplyDeltaObj)->TryGetBoolField(TEXT("supported"), bSupported) || !bSupported)
		{
			continue;
		}
		FString DeltaTool;
		UNTEST_ASSERT_TRUE((*ApplyDeltaObj)->TryGetStringField(TEXT("tool"), DeltaTool));

		// Family scope agreement: the apply_delta tool name MUST start with
		// "<catalog_key>_" so spec & delta cover the same family.
		UNTEST_EXPECT_TRUE(DeltaTool.StartsWith(ExpectedPrefix));
		// And the suffix MUST be "apply_delta" exactly.
		UNTEST_EXPECT_EQ(DeltaTool, ExpectedPrefix + TEXT("apply_delta"));
	}

	co_return;
}

// ===========================================================================
// (d) _meta bookkeeping: schema_version >= 3 (apply_delta field added),
//     entry_count == actual non-meta key count, expected to be 17 today.
// ===========================================================================
UNTEST_UNIT_OPTS(Claireon, ApplyDeltaCatalog, MetaBookkeepingIsConsistent, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonApplyDeltaCatalogTestsNS;

	TSharedPtr<FJsonObject> Catalog = AdCat_LoadCatalogForTests();
	UNTEST_ASSERT_TRUE(Catalog.IsValid());

	int32 NonMetaCount = 0;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : Catalog->Values)
	{
		if (KV.Key.StartsWith(TEXT("_"))) { continue; }
		++NonMetaCount;
	}

	const TSharedPtr<FJsonObject>* MetaObj = nullptr;
	UNTEST_ASSERT_TRUE(Catalog->TryGetObjectField(TEXT("_meta"), MetaObj));
	UNTEST_ASSERT_TRUE(MetaObj && (*MetaObj).IsValid());

	double SchemaVersion = 0.0;
	UNTEST_ASSERT_TRUE((*MetaObj)->TryGetNumberField(TEXT("schema_version"), SchemaVersion));
	UNTEST_EXPECT_TRUE(static_cast<int32>(SchemaVersion) >= 3);

	double EntryCount = 0.0;
	UNTEST_ASSERT_TRUE((*MetaObj)->TryGetNumberField(TEXT("entry_count"), EntryCount));
	UNTEST_EXPECT_EQ(static_cast<int32>(EntryCount), NonMetaCount);
	UNTEST_EXPECT_EQ(NonMetaCount, 17);

	co_return;
}

#endif // WITH_UNTESTED
