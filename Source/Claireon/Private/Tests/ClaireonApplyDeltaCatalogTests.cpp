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
//       EXCEPTION: AdCat_ApplyDeltaOnlyFamilies is a safety valve for a
//       hypothetical family that ships an apply_delta tool with NO row at
//       all in this apply_spec-keyed catalog. It is empty today: animbp
//       used to be that case, but now has an apply_delta_only row (see (c)),
//       so it is covered by the ordinary catalog-lookup path below instead.
//   (c) apply_spec/apply_delta scope agreement: a catalog entry either
//       carries a top-level `tool` (the apply_spec/instance_apply_spec wire
//       name), in which case any apply_delta.supported==true `tool` on the
//       same entry MUST belong to the same category (the catalog key); or
//       it is apply_delta_only (no top-level `tool`, `apply_delta_only`:
//       true), in which case apply_delta.supported MUST be true and its
//       `tool` MUST equal "<catalog_key>_apply_delta". A row satisfying
//       neither shape (no top-level `tool` AND no working apply_delta) is
//       malformed and must fail.
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
	// families, plus bp_apply_delta from the bp family and animbp_apply_delta
	// from the apply_delta_only animbp family. The catalog row for bp lists
	// bp_apply_delta even though the registration lives in
	// ClaireonTool_ApplyBlueprintDelta. animbp has a catalog row (apply_delta_only:
	// true) but no top-level `tool`, since no animbp_apply_spec tool exists.
	static const TArray<TPair<FString, FString>>& AdCat_GetExpectedDeltaPairs()
	{
		static const TArray<TPair<FString, FString>> Pairs = {
			{ TEXT("animbp"),         TEXT("animbp_apply_delta") },
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

	// Families that register an <family>_apply_delta tool but have NO catalog
	// row at all -- not even an apply_delta_only one. Empty today: animbp used
	// to be the one example (see Docs/llm/todo/claireon-product-defects.md item 3),
	// but ApplySpecCatalog.json's schema was extended (schema_version 4) to let a
	// row omit the top-level `tool` field for exactly this shape, so animbp now
	// has a proper apply_delta_only row and is covered by the ordinary
	// catalog-lookup path in AdCat_GetExpectedDeltaPairs() above instead.
	//
	// Kept as a safety valve so the registered->catalog direction below still
	// fails loudly if a *new* apply_delta tool ships with no catalog row at all
	// (neither a normal row nor an apply_delta_only one); it is not a licence to
	// skip the check by adding entries here instead of a catalog row.
	static const TSet<FString>& AdCat_ApplyDeltaOnlyFamilies()
	{
		static const TSet<FString> Families = {};
		return Families;
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
	// We expect the 18 catalog entries to be present.
	UNTEST_EXPECT_EQ(EntriesChecked, 18);

	co_return;
}

// ===========================================================================
// (b) Catalog <-> registered apply_delta tools agree bidirectionally.
//
// The registry must be populated before the comparison, and the comparison is
// the headline invariant -- it is NOT optional. FClaireonModule::StartupModule()
// early-returns under IsRunningCommandlet(), and Invoke-UntestTests.ps1 runs
// -run=UntestRunTests in both of its modes, so the registry is never built by
// normal startup here. EnsureServerForTest() is the seam that constructs and
// populates it headlessly (no listener, no proxy). StartServer() is NOT usable:
// it explicitly refuses to construct the registry when StartupModule() was
// skipped, so the old `if (bWeStartedServer) Module.StartServer();` left Server
// null every time.
// ===========================================================================
UNTEST_UNIT_OPTS(Claireon, ApplyDeltaCatalog, CatalogMatchesRegisteredApplyDeltaTools, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonApplyDeltaCatalogTestsNS;

	// Was: StartServer() + GetServer(), which always yielded nullptr in a test
	// run (see the block comment above). Everything below that depended on
	// Server was therefore nested in `if (Server)` and never executed, with no
	// warning -- the test passed on the catalog half alone. Assert the pointer
	// so a broken seam fails loudly instead of silently skipping the invariant.
	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = Module.EnsureServerForTest();
	UNTEST_ASSERT_PTR(Server);

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

	// Catalog half: every supported:true entry MUST name a wire-name of the
	// shape "<catalog_key>_apply_delta", and the expected pair list MUST be a
	// subset of the catalog claims. This half needs no registry.
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

	// Bidirectional invariant between catalog and registered tools. This used to
	// be nested inside `if (Server)`, and Server was always null, so this whole
	// block -- the reason the test exists -- never ran. Un-nested: the registry
	// is guaranteed populated by the seam above.
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
	// (b) registered -> catalog. Every registered apply_delta tool must either
	// have a catalog row naming it (animbp's apply_delta_only row counts here,
	// same as any other), or be a known apply_delta-only family with NO catalog
	// row at all (see AdCat_ApplyDeltaOnlyFamilies; empty today).
	const TSet<FString>& DeltaOnly = AdCat_ApplyDeltaOnlyFamilies();
	int32 CataloguedRegistered = 0;
	for (const TPair<FString, FString>& KV : RegisteredDeltaTools)
	{
		if (DeltaOnly.Contains(KV.Key))
		{
			// Still assert the wire-name convention holds for these.
			UNTEST_EXPECT_EQ(KV.Value, KV.Key + TEXT("_apply_delta"));
			continue;
		}
		const FString* CatalogName = CatalogDeltaTools.Find(KV.Key);
		UNTEST_EXPECT_PTR(CatalogName);
		if (CatalogName)
		{
			UNTEST_EXPECT_EQ(*CatalogName, KV.Value);
		}
		++CataloguedRegistered;
	}
	// The catalogued registered tools must correspond 1:1 with the expected
	// pairs. A new apply_delta tool with no catalog row and no entry in
	// AdCat_ApplyDeltaOnlyFamilies fails the EXPECT_PTR above, so this count
	// cannot be satisfied by silently growing the allowlist.
	UNTEST_EXPECT_EQ(CataloguedRegistered, Expected.Num());
	UNTEST_EXPECT_EQ(RegisteredDeltaTools.Num(), Expected.Num() + DeltaOnly.Num());

	// No StopServer() here: EnsureServerForTest() binds no listener and
	// registers with no proxy, and the registry is shared with every other test
	// in the process -- tearing it down would break their ordering.
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

		const TSharedPtr<FJsonObject>* ApplyDeltaObj = nullptr;
		UNTEST_ASSERT_TRUE((*EntryObj)->TryGetObjectField(TEXT("apply_delta"), ApplyDeltaObj));

		const FString ExpectedPrefix = KV.Key + TEXT("_");

		bool bSupported = false;
		(*ApplyDeltaObj)->TryGetBoolField(TEXT("supported"), bSupported);
		FString DeltaTool;
		const bool bHasDeltaTool = bSupported && (*ApplyDeltaObj)->TryGetStringField(TEXT("tool"), DeltaTool);

		// Top-level `tool` is the apply_spec / instance_apply_spec wire name.
		// It is OPTIONAL as of schema_version 4: an apply_delta_only row (no
		// apply_spec tool exists for the family at all, e.g. "animbp") omits it.
		FString SpecTool;
		if ((*EntryObj)->TryGetStringField(TEXT("tool"), SpecTool))
		{
			// Normal apply_spec row. Spec tool must start with "<catalog_key>_" so
			// the family-scope check below is unambiguous. material_instance is the
			// documented exception: composed name is
			// "material_instance_instance_apply_spec".
			UNTEST_EXPECT_TRUE(SpecTool.StartsWith(ExpectedPrefix));

			if (!bSupported)
			{
				continue;
			}
			UNTEST_ASSERT_TRUE(bHasDeltaTool);

			// Family scope agreement: the apply_delta tool name MUST start with
			// "<catalog_key>_" so spec & delta cover the same family.
			UNTEST_EXPECT_TRUE(DeltaTool.StartsWith(ExpectedPrefix));
			// And the suffix MUST be "apply_delta" exactly.
			UNTEST_EXPECT_EQ(DeltaTool, ExpectedPrefix + TEXT("apply_delta"));
		}
		else
		{
			// apply_delta_only row: no apply_spec tool exists for this family, so
			// apply_delta is the ONLY thing anchoring this entry to a registered
			// tool. A row that reaches here without a working apply_delta
			// (supported==true and a `tool` naming "<catalog_key>_apply_delta")
			// names no registered tool at all -- neither an apply_spec tool nor a
			// working apply_delta one -- and is malformed. Fail loudly rather than
			// silently accepting an orphaned catalog row.
			UNTEST_EXPECT_TRUE(bSupported);
			UNTEST_ASSERT_TRUE(bHasDeltaTool);
			UNTEST_EXPECT_EQ(DeltaTool, ExpectedPrefix + TEXT("apply_delta"));
		}
	}

	co_return;
}

// ===========================================================================
// (d) _meta bookkeeping: schema_version >= 3 (apply_delta field added),
//     entry_count == actual non-meta key count, expected to be 18 today.
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
	UNTEST_EXPECT_EQ(NonMetaCount, 18);

	co_return;
}

#endif // WITH_UNTESTED
