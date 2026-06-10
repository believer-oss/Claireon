// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

// ===========================================================================
// Contract-snapshot harness for tool_search.
//
// Drives ClaireonTool_SearchTools::Execute for a FIXED query set and splits
// assertions into two classes:
//
//   (A) DETERMINISTIC, RANKER-INDEPENDENT paths -- `select:`, `name=`/`tool_name=`
//       deep-inspect, and `mode=categories`. These do NOT touch the ranker, so
//       their full output (Data JSON + Summary) is serialized to a CANONICAL
//       (recursively key-sorted) JSON string and compared BYTE-FOR-BYTE against
//       a golden fixture. On first run (golden absent) the golden is written;
//       on every subsequent run byte equality is asserted.
//
//   (B) RANKED query paths -- these are NOT byte-compared. Instead we assert
//       INVARIANTS:
//         * result count <= max_results
//         * a brief/standard multi-result query CONTAINS the upgrade footer
//         * a single exact-name match SUPPRESSES the footer
//
// Canonical serialization (key-sorted, recursive) makes the golden immune to
// incidental FJsonObject TMap hash-iteration order; only real shape/value
// changes can break byte equality.
//
// v2/Linux hygiene:
//   - file-local namespace discriminator (avoids anon-NS unity collision)
//   - UNTEST_ASSERT_*/UNTEST_EXPECT_* never appear inside lambdas
//   - unique .cpp basename (ClaireonToolSearchContractTests.cpp)
// ===========================================================================

#include "Untest.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "ClaireonBridge.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tools/ClaireonTool_SearchTools.h"
#include "Tools/IClaireonTool.h"

namespace ClaireonToolSearchContractTestsNS
{
	// -----------------------------------------------------------------------
	// Golden fixture path (committed). Lives beside the test under Fixtures/.
	// -----------------------------------------------------------------------
	static FString GetGoldenPath()
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::ProjectDir(),
				TEXT("Plugins/Claireon/Source/Claireon/Private/Tests/Fixtures/tool_search_contract_snapshot.json")));
	}

	// -----------------------------------------------------------------------
	// Ensure the tool registry + Python bridge are available headlessly.
	// Mirrors the corpus harness bootstrap (EnsureServerForTest + EnsureRegistered).
	// -----------------------------------------------------------------------
	static FClaireonServer* EnsureServerAndBridge(FClaireonModule& Module)
	{
		FClaireonServer* Server = Module.EnsureServerForTest();
		if (Server)
		{
			FClaireonBridge::EnsureRegistered();
		}
		return Server;
	}

	// -----------------------------------------------------------------------
	// Recursively canonicalize a JSON value: objects get their keys sorted
	// ascending (byte-wise) so the serialized form is independent of TMap
	// hash-iteration order. Arrays preserve order (order is semantically
	// meaningful for tools[]/categories[]/not_found[]). Scalars pass through.
	// -----------------------------------------------------------------------
	static TSharedPtr<FJsonValue> Canonicalize(const TSharedPtr<FJsonValue>& In)
	{
		if (!In.IsValid())
		{
			return In;
		}

		if (In->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject>& Obj = In->AsObject();
			TSharedPtr<FJsonObject> OutObj = MakeShared<FJsonObject>();
			if (Obj.IsValid())
			{
				TArray<FString> Keys;
				Obj->Values.GetKeys(Keys);
				Keys.Sort();
				for (const FString& Key : Keys)
				{
					OutObj->SetField(Key, Canonicalize(Obj->Values[Key]));
				}
			}
			return MakeShared<FJsonValueObject>(OutObj);
		}

		if (In->Type == EJson::Array)
		{
			TArray<TSharedPtr<FJsonValue>> OutArr;
			for (const TSharedPtr<FJsonValue>& Elem : In->AsArray())
			{
				OutArr.Add(Canonicalize(Elem));
			}
			return MakeShared<FJsonValueArray>(OutArr);
		}

		return In;
	}

	// -----------------------------------------------------------------------
	// Serialize a tool result to a canonical, stable string capturing the full
	// deterministic contract: the Data JSON (key-sorted) plus the Summary text
	// and the error flag. Returns a pretty-printed string so golden diffs are
	// human-readable.
	// -----------------------------------------------------------------------
	static FString CanonicalSnapshot(const IClaireonTool::FToolResult& Result)
	{
		TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetBoolField(TEXT("is_error"), Result.bIsError);
		Envelope->SetStringField(TEXT("summary"), Result.Summary);
		Envelope->SetStringField(TEXT("error_message"), Result.ErrorMessage);
		if (Result.Data.IsValid())
		{
			Envelope->SetObjectField(TEXT("data"), Result.Data);
		}

		const TSharedPtr<FJsonValue> Canon =
			Canonicalize(MakeShared<FJsonValueObject>(Envelope));

		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Canon->AsObject().ToSharedRef(), Writer);
		Writer->Close();
		return Out;
	}

	// -----------------------------------------------------------------------
	// Drive Execute with an explicit argument object.
	// -----------------------------------------------------------------------
	static IClaireonTool::FToolResult ExecCase(ClaireonTool_SearchTools& Tool,
	                                    const TSharedPtr<FJsonObject>& Args)
	{
		return Tool.Execute(Args);
	}

	static TSharedPtr<FJsonObject> ArgsQuery(const FString& Query, int32 MaxResults,
	                                         const FString& Detail = TEXT("standard"))
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("query"), Query);
		Args->SetNumberField(TEXT("max_results"), static_cast<double>(MaxResults));
		Args->SetStringField(TEXT("detail"), Detail);
		return Args;
	}

	// -----------------------------------------------------------------------
	// Count the flat tools[] entries on a ranked-query result. Post-swap the
	// query shape is a flat tools[]; pre-swap it is grouped categories[]. We
	// count leaf tool entries in BOTH shapes so the count invariant is
	// expressible against either ranker.
	// -----------------------------------------------------------------------
	static int32 CountToolEntries(const IClaireonTool::FToolResult& Result)
	{
		if (!Result.Data.IsValid())
		{
			return 0;
		}
		// Flat shape: data.tools[]
		const TArray<TSharedPtr<FJsonValue>>* FlatTools = nullptr;
		if (Result.Data->TryGetArrayField(TEXT("tools"), FlatTools) && FlatTools)
		{
			return FlatTools->Num();
		}
		// Grouped shape: data.categories[].tools[]
		int32 Count = 0;
		const TArray<TSharedPtr<FJsonValue>>* Cats = nullptr;
		if (Result.Data->TryGetArrayField(TEXT("categories"), Cats) && Cats)
		{
			for (const TSharedPtr<FJsonValue>& CatVal : *Cats)
			{
				const TSharedPtr<FJsonObject>* CatObj = nullptr;
				if (!CatVal->TryGetObject(CatObj) || !CatObj || !(*CatObj).IsValid()) { continue; }
				const TArray<TSharedPtr<FJsonValue>>* CatTools = nullptr;
				if ((*CatObj)->TryGetArrayField(TEXT("tools"), CatTools) && CatTools)
				{
					Count += CatTools->Num();
				}
			}
		}
		return Count;
	}

	static bool SummaryHasFooter(const IClaireonTool::FToolResult& Result)
	{
		return Result.Summary.Contains(TEXT("detail=\"full\""));
	}

	// Read the rank_source of the rank-0 tool in a flat tools[] result. Returns
	// empty when there is no first tool / no rank_source field. The ranked path
	// emits rank_source on every tool (exact_pin|near_exact_boost|hybrid_rrf|
	// lexical_only_fallback); the contract keys footer/discovery semantics off it.
	static FString FirstRankSource(const IClaireonTool::FToolResult& Result)
	{
		if (!Result.Data.IsValid()) { return FString(); }
		const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
		if (!Result.Data->TryGetArrayField(TEXT("tools"), Tools) || !Tools || Tools->Num() == 0)
		{
			return FString();
		}
		const TSharedPtr<FJsonObject>* ToolObj = nullptr;
		if (!(*Tools)[0]->TryGetObject(ToolObj) || !ToolObj || !(*ToolObj).IsValid())
		{
			return FString();
		}
		FString RankSource;
		(*ToolObj)->TryGetStringField(TEXT("rank_source"), RankSource);
		return RankSource;
	}

	// Name of the rank-0 tool in a flat tools[] result (empty when none).
	static FString FirstName(const IClaireonTool::FToolResult& Result)
	{
		if (!Result.Data.IsValid()) { return FString(); }
		const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
		if (!Result.Data->TryGetArrayField(TEXT("tools"), Tools) || !Tools || Tools->Num() == 0)
		{
			return FString();
		}
		const TSharedPtr<FJsonObject>* ToolObj = nullptr;
		if (!(*Tools)[0]->TryGetObject(ToolObj) || !ToolObj || !(*ToolObj).IsValid())
		{
			return FString();
		}
		FString N;
		(*ToolObj)->TryGetStringField(TEXT("name"), N);
		return N;
	}
}

// ===========================================================================
// (A) Deterministic byte-stable snapshot: select: / name= / mode=categories.
// ===========================================================================
UNTEST_UNIT_OPTS(Claireon, ToolSearchContract, DeterministicSnapshot, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonToolSearchContractTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = EnsureServerAndBridge(Module);
	UNTEST_ASSERT_PTR(Server);

	ClaireonTool_SearchTools Tool;

	// Fixed deterministic query set (ranker-independent paths only).
	TArray<TSharedPtr<FJsonObject>> Cases;

	// 1) select: two real tools + one bad name (exercises not_found).
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("query"),
			TEXT("select:chooser_create,chooser_duplicate,definitely_not_a_real_tool_xyz"));
		Cases.Add(A);
	}
	// 2) name= deep-inspect (preferred alias), detail=full.
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("name"), TEXT("chooser_create"));
		A->SetStringField(TEXT("detail"), TEXT("full"));
		Cases.Add(A);
	}
	// 3) tool_name= deprecated-alias deep-inspect.
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("tool_name"), TEXT("chooser_duplicate"));
		Cases.Add(A);
	}
	// 4) mode=categories grouped catalog listing.
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("mode"), TEXT("categories"));
		Cases.Add(A);
	}

	// Build the combined snapshot string across all deterministic cases.
	FString Combined;
	for (int32 i = 0; i < Cases.Num(); ++i)
	{
		const IClaireonTool::FToolResult Result = ExecCase(Tool, Cases[i]);
		Combined += FString::Printf(TEXT("=== case %d ===\n"), i);
		Combined += CanonicalSnapshot(Result);
		Combined += TEXT("\n");
	}

	const FString GoldenPath = GetGoldenPath();
	const bool bGoldenExists = FPaths::FileExists(GoldenPath);

	if (!bGoldenExists)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(GoldenPath), /*Tree=*/true);
		const bool bWrote = FFileHelper::SaveStringToFile(
			Combined, *GoldenPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		UE_LOG(LogTemp, Display,
			TEXT("[ToolSearchContract] golden absent -- captured snapshot to '%s' (wrote=%d)"),
			*GoldenPath, bWrote ? 1 : 0);
		UNTEST_EXPECT_TRUE(bWrote);
		co_return;
	}

	FString Golden;
	const bool bRead = FFileHelper::LoadFileToString(Golden, *GoldenPath);
	UNTEST_ASSERT_TRUE(bRead);

	// Normalize line endings so a CRLF/LF checkout difference does not falsely
	// fail byte-equality (the canonical JSON itself uses \n internally).
	Golden.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	FString CombinedNorm = Combined;
	CombinedNorm.ReplaceInline(TEXT("\r\n"), TEXT("\n"));

	const bool bByteEqual = CombinedNorm.Equals(Golden, ESearchCase::CaseSensitive);
	if (!bByteEqual)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[ToolSearchContract] DETERMINISTIC SNAPSHOT MISMATCH -- a ranker-independent "
			     "path changed. Golden len=%d, actual len=%d."),
			Golden.Len(), CombinedNorm.Len());
	}
	UNTEST_EXPECT_TRUE(bByteEqual);

	co_return;
}

// ===========================================================================
// (B) Ranked-query invariants: flat tools[] shape, count<=max_results, footer
//     presence/suppression, rank_source semantics. NOT byte-compared (the ranked
//     shape and the RRF/exact-pin scores are intentionally non-deterministic
//     across platforms; we assert invariants instead of a golden).
//
// Invariants this contract pins (hybrid RRF ranker):
//
//   - rank_source: every ranked tool carries a rank_source string. The rank-0
//     result of an exact tool-name lookup is "exact_pin". The score field's
//     polarity is rank_source-dependent (rrf/boost: higher=better; exact_pin:
//     reserved sentinel); consumers MUST read rank_source before score.
//
//   - EXACT-NAME PIN BYPASSES CATEGORY: if the normalized query is an exact tool
//     name, that tool is surfaced at rank 0 with rank_source=exact_pin EVEN WHEN a
//     category filter excludes its category and even when BM25/semantic recalled
//     nothing. Category filters are otherwise respected (near-exact and ordinary
//     ranking honor them); the exact-name pin is the single documented bypass.
//
//   - FOOTER honesty: the upgrade footer is suppressed for a GENUINE exact-name
//     lookup (rank-0 rank_source=exact_pin) and for genuine zero retrieval
//     (no results), NOT merely because exactly one result happens to exist. A
//     multi-hit ordinary search shows the footer.
// ===========================================================================
UNTEST_UNIT_OPTS(Claireon, ToolSearchContract, RankedInvariants, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonToolSearchContractTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = EnsureServerAndBridge(Module);
	UNTEST_ASSERT_PTR(Server);

	ClaireonTool_SearchTools Tool;

	// --- Multi-result standard query: count<=max_results, footer present. ---
	{
		const int32 MaxResults = 5;
		const IClaireonTool::FToolResult Result =
			ExecCase(Tool, ArgsQuery(TEXT("chooser"), MaxResults, TEXT("standard")));
		UNTEST_EXPECT_FALSE(Result.bIsError);
		const int32 Count = CountToolEntries(Result);
		UNTEST_EXPECT_TRUE(Count <= MaxResults);
		UNTEST_EXPECT_TRUE(Count > 1);
		const bool bHasFooter = SummaryHasFooter(Result);
		UNTEST_EXPECT_TRUE(bHasFooter);
	}

	// --- Brief multi-result query: count<=max_results, footer present. ---
	{
		const int32 MaxResults = 8;
		const IClaireonTool::FToolResult Result =
			ExecCase(Tool, ArgsQuery(TEXT("blueprint"), MaxResults, TEXT("brief")));
		UNTEST_EXPECT_FALSE(Result.bIsError);
		const int32 Count = CountToolEntries(Result);
		UNTEST_EXPECT_TRUE(Count <= MaxResults);
		const bool bHasFooter = SummaryHasFooter(Result);
		UNTEST_EXPECT_TRUE(bHasFooter);
	}

	// --- Single exact-name match: rank-0 is exact_pin, footer SUPPRESSED. ---
	// Query == canonical tool name, max_results=1 so only the pinned exact
	// match surfaces. The footer is suppressed because the rank-0 rank_source is
	// exact_pin (GENUINE exact-name lookup), not merely because Count==1.
	{
		const IClaireonTool::FToolResult Result =
			ExecCase(Tool, ArgsQuery(TEXT("chooser_create"), /*MaxResults=*/1, TEXT("standard")));
		UNTEST_EXPECT_FALSE(Result.bIsError);
		const int32 Count = CountToolEntries(Result);
		UNTEST_EXPECT_TRUE(Count == 1);
		UNTEST_EXPECT_STREQ(*FirstName(Result), TEXT("chooser_create"));
		UNTEST_EXPECT_STREQ(*FirstRankSource(Result), TEXT("exact_pin"));
		const bool bFooterSuppressed = !SummaryHasFooter(Result);
		UNTEST_EXPECT_TRUE(bFooterSuppressed);
	}

	// --- Exact-name pin BYPASSES a mismatched category filter (documented rule). ---
	// chooser_create's category is "chooser"; filtering by "anim" must NOT hide it.
	// The exact-name pin surfaces it at rank 0 with rank_source=exact_pin even
	// though the category filter excludes its category and BM25/semantic recall is
	// empty under that filter. (Operator rule: category respected EXCEPT the
	// documented exact-name bypass.)
	{
		TSharedPtr<FJsonObject> Args = ArgsQuery(TEXT("chooser_create"), /*MaxResults=*/5, TEXT("standard"));
		Args->SetStringField(TEXT("category"), TEXT("anim"));
		const IClaireonTool::FToolResult Result = ExecCase(Tool, Args);
		UNTEST_EXPECT_FALSE(Result.bIsError);
		UNTEST_EXPECT_STREQ(*FirstName(Result), TEXT("chooser_create"));
		UNTEST_EXPECT_STREQ(*FirstRankSource(Result), TEXT("exact_pin"));
		// Footer suppressed: genuine exact-name lookup.
		UNTEST_EXPECT_TRUE(!SummaryHasFooter(Result));
	}

	// --- Genuine zero retrieval: no results, footer SUPPRESSED (not defeated by
	//     semantic flooding). A gibberish, non-tool-name query must recall nothing
	//     (the semantic honesty floor drops near-orthogonal padding) so the
	//     no-recall footer stays suppressed. ---
	{
		const IClaireonTool::FToolResult Result =
			ExecCase(Tool, ArgsQuery(TEXT("qwzxkbvnmfjghdplrt"), /*MaxResults=*/5, TEXT("standard")));
		UNTEST_EXPECT_FALSE(Result.bIsError);
		const int32 Count = CountToolEntries(Result);
		UNTEST_EXPECT_TRUE(Count == 0);
		UNTEST_EXPECT_TRUE(!SummaryHasFooter(Result));
	}

	co_return;
}

#endif // WITH_UNTESTED
