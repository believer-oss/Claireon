// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonToolEmbeddingIndex.h"

#include "ClaireonLog.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "Tools/IClaireonTool.h"

#include "Algo/Sort.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

// ---------------------------------------------------------------------------
// Static storage definitions
// ---------------------------------------------------------------------------

TArray<float>             FClaireonToolEmbeddingIndex::GEmbeddingMatrix;
int32                     FClaireonToolEmbeddingIndex::GEmbeddingDim = 0;
TArray<FString>           FClaireonToolEmbeddingIndex::GToolNames;
TArray<FString>           FClaireonToolEmbeddingIndex::GToolCategories;
FCriticalSection          FClaireonToolEmbeddingIndex::GLock;
FClaireonEmbeddingModel     FClaireonToolEmbeddingIndex::GEmbeddingModel;
FClaireonWordPieceTokenizer FClaireonToolEmbeddingIndex::GTokenizer;
bool                      FClaireonToolEmbeddingIndex::bModelReady = false;

#if WITH_UNTESTED
TOptional<FClaireonEmbedderMeta> FClaireonToolEmbeddingIndex::GModelOverrideForTest;
#endif // WITH_UNTESTED

// ---------------------------------------------------------------------------
// FClaireonToolEmbeddingIndex
//
// Semantic (dense-vector) sibling of FClaireonToolSearchIndex. Mirrors its
// static-singleton + FCriticalSection shape. Holds a row-major [N x Dim] float
// matrix of L2-normalized per-tool embeddings plus parallel name/category
// arrays; similarity is brute-force dot-product (== cosine on unit vectors).
//
// Fallback safety: when the ORT runtime, the ONNX model, or vocab.txt is
// unavailable, the model never loads, the matrix stays empty, IsReady() stays
// false, and the Execute() surface degrades to the lexical path. Nothing here
// crashes on a missing model.
// ---------------------------------------------------------------------------

void FClaireonToolEmbeddingIndex::EnsureModelLoaded_NoLock()
{
	// Idempotent: load the model + tokenizer ONCE and cache across rebuilds. The
	// model/session is expensive to create; OnToolsChanged churn re-embeds tools
	// but must not re-pay the load cost.
	if (bModelReady)
	{
		return;
	}

	// Production: always the default model. The test-only override
	// (SetModelForTest) may install an alternative meta that takes precedence
	// here; non-test builds never compile that branch.
#if WITH_UNTESTED
	const FClaireonEmbedderMeta Meta = GModelOverrideForTest.IsSet()
		? GModelOverrideForTest.GetValue()
		: FClaireonEmbedderMeta::Default();
#else
	const FClaireonEmbedderMeta Meta = FClaireonEmbedderMeta::Default();
#endif // WITH_UNTESTED

	// Resolve the vocab path the same way FClaireonEmbeddingModel::Load resolves the
	// model path: plugin BaseDir / Resources/Models / Meta.ModelDir / Meta.VocabFile.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Claireon"));
	if (!Plugin.IsValid())
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[EmbeddingIndex] Claireon plugin not found; semantic search disabled."));
		return;
	}

	const FString VocabPath = FPaths::Combine(
		Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Models"), Meta.ModelDir, Meta.VocabFile);

	// Load the tokenizer first; Embed(text) needs it. A missing/invalid vocab is a
	// graceful disable, not a crash.
	if (!GTokenizer.LoadVocab(VocabPath))
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[EmbeddingIndex] Failed to load vocab '%s'; semantic search disabled "
			     "(lexical fallback)."), *VocabPath);
		return;
	}

	// Load the ONNX model (ORT CPU). Absent runtime / model -> graceful disable.
	if (!GEmbeddingModel.Load(Meta))
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[EmbeddingIndex] Embedding model failed to load; semantic search disabled "
			     "(lexical fallback)."));
		return;
	}

	// Lend the tokenizer to the model so Embed(text) can tokenize. The index owns
	// both; the model only borrows the tokenizer pointer.
	GEmbeddingModel.SetTokenizer(&GTokenizer);

	bModelReady = GEmbeddingModel.IsReady() && GTokenizer.IsReady();
	if (bModelReady)
	{
		GEmbeddingDim = GEmbeddingModel.Dim();
	}
}

void FClaireonToolEmbeddingIndex::RebuildFromLiveServer()
{
	FScopeLock Lock(&GLock);

	// (Re)load the model+tokenizer once, then cache. No-op when already loaded.
	EnsureModelLoaded_NoLock();

	// Reset the matrix for this rebuild (the model/tokenizer are intentionally kept).
	GEmbeddingMatrix.Reset();
	GToolNames.Reset();
	GToolCategories.Reset();

	if (!bModelReady)
	{
		// Fallback safety: no model/tokenizer -> empty index, IsReady() stays false.
		UE_LOG(LogClaireon, Verbose,
			TEXT("[EmbeddingIndex] RebuildFromLiveServer: model not ready; index left empty "
			     "(lexical fallback)."));
		return;
	}

	FClaireonServer* Server = FClaireonModule::Get().GetServer();
	if (!Server)
	{
		UE_LOG(LogClaireon, Verbose,
			TEXT("[EmbeddingIndex] RebuildFromLiveServer: no live server; index left empty."));
		return;
	}

	const double StartSeconds = FPlatformTime::Seconds();

	const int32 Dim = GEmbeddingDim;
	const TMap<FString, TSharedPtr<IClaireonTool>>& Tools = Server->GetTools();
	GToolNames.Reserve(Tools.Num());
	GToolCategories.Reserve(Tools.Num());
	GEmbeddingMatrix.Reserve(Tools.Num() * Dim);

	int32 Embedded = 0;
	int32 Failed   = 0;
	for (const TPair<FString, TSharedPtr<IClaireonTool>>& Pair : Tools)
	{
		const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
		if (!Tool.IsValid()) { continue; }

		const FString ToolName = Tool->GetName();
		// Skip the meta-tools, mirroring the lexical index.
		if (ToolName == TEXT("python_execute") || ToolName == TEXT("tool_search"))
		{
			continue;
		}
		if (ToolName.IsEmpty()) { continue; }

		// Single-sourced doc string (shared with the FTS5 field extraction; the
		// FlattenParams param-name flattener is single-sourced there too).
		const FString Doc = FClaireonToolSearchIndex::BuildSemanticDocString(Tool);

		TArray<float> Vec;
		if (!GEmbeddingModel.Embed(Doc, /*bIsQuery=*/false, Vec) || Vec.Num() != Dim)
		{
			// One tool failing to embed must not abort the whole index.
			++Failed;
			continue;
		}

		GToolNames.Add(ToolName);
		GToolCategories.Add(Tool->GetCategory());
		GEmbeddingMatrix.Append(Vec);
		++Embedded;
	}

	const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	UE_LOG(LogClaireon, Display,
		TEXT("[EmbeddingIndex] Rebuilt: %d tools embedded (%d failed) in %.1f ms "
		     "(Dim=%d, matrix=%d floats)."),
		Embedded, Failed, ElapsedMs, Dim, GEmbeddingMatrix.Num());
}

void FClaireonToolEmbeddingIndex::Clear()
{
	FScopeLock Lock(&GLock);
	GEmbeddingMatrix.Empty();
	GToolNames.Empty();
	GToolCategories.Empty();
	GEmbeddingDim = 0;

	// Release the model session + vocab cleanly. Re-loading a fresh, default-
	// constructed FClaireonEmbeddingModel drops the cached ORT instance (its move
	// assignment releases the prior TSharedPtr / TStrongObjectPtr). Done so a
	// ShutdownModule-time Clear() frees the model without asserting (mirrors how
	// FClaireonToolSearchIndex::Clear closes the DB before static destruction).
	GEmbeddingModel = FClaireonEmbeddingModel();
	GTokenizer = FClaireonWordPieceTokenizer();
	bModelReady = false;
}

#if WITH_UNTESTED
void FClaireonToolEmbeddingIndex::SetModelForTest(const FClaireonEmbedderMeta& Meta)
{
	FScopeLock Lock(&GLock);

	// Install the override so the next EnsureModelLoaded_NoLock loads this model.
	GModelOverrideForTest = Meta;

	// Force the swap to take effect: tear down the currently-cached model so the
	// cached-load short-circuit (bModelReady) is bypassed and the next
	// RebuildFromLiveServer reloads from the override meta. Mirrors Clear()'s
	// teardown but is duplicated inline to stay under the single GLock acquired
	// here (Clear() takes GLock itself; re-entering FCriticalSection is avoided).
	GEmbeddingMatrix.Empty();
	GToolNames.Empty();
	GToolCategories.Empty();
	GEmbeddingDim = 0;
	GEmbeddingModel = FClaireonEmbeddingModel();
	GTokenizer = FClaireonWordPieceTokenizer();
	bModelReady = false;
}

void FClaireonToolEmbeddingIndex::ResetModelForTest()
{
	FScopeLock Lock(&GLock);

	// Drop the override so EnsureModelLoaded_NoLock reverts to Default() (the
	// shipped bge-small-en-v1.5), and tear down the cached model so the next
	// RebuildFromLiveServer reloads it.
	GModelOverrideForTest.Reset();

	GEmbeddingMatrix.Empty();
	GToolNames.Empty();
	GToolCategories.Empty();
	GEmbeddingDim = 0;
	GEmbeddingModel = FClaireonEmbeddingModel();
	GTokenizer = FClaireonWordPieceTokenizer();
	bModelReady = false;
}
#endif // WITH_UNTESTED

bool FClaireonToolEmbeddingIndex::IsReady()
{
	FScopeLock Lock(&GLock);
	return bModelReady && GEmbeddingModel.IsReady() && GToolNames.Num() > 0;
}

TArray<FClaireonToolCatalogMatch> FClaireonToolEmbeddingIndex::FindNearestSemantic(
	const FString& Query,
	int32 MaxResults,
	const FString& CategoryFilter)
{
	FScopeLock Lock(&GLock);

	TArray<FClaireonToolCatalogMatch> Out;

	// Fallback safety: not ready -> empty (caller routes to the lexical path).
	if (!bModelReady || !GEmbeddingModel.IsReady() || GToolNames.Num() == 0 || GEmbeddingDim <= 0)
	{
		return Out;
	}
	if (MaxResults <= 0 || Query.IsEmpty())
	{
		return Out;
	}

	const int32 Dim = GEmbeddingDim;

	// Canonicalize the query the SAME way the lexical channel does BEFORE embedding:
	// strip standalone boolean operators (AND/OR/NOT), drop boolean grouping
	// punctuation, collapse whitespace. Without this the semantic channel embeds the
	// RAW query text, so a boolean-decorated query ("blueprint AND (chooser OR
	// \"chooser\")") would embed to a different vector than the plain query
	// ("blueprint chooser") and rank differently, breaking the boolean-equivalence
	// contract. This is the SINGLE shared spot the hybrid path and the semantic-only
	// harness both go through, so both agree with the lexical path's strip from one
	// source of truth.
	const FString NormalizedQuery = FClaireonToolSearchIndex::NormalizeQueryForRetrieval(Query);
	if (NormalizedQuery.IsEmpty())
	{
		return Out;
	}

	// Embed the query once (query-side prefix applied by the model metadata).
	TArray<float> QueryVec;
	if (!GEmbeddingModel.Embed(NormalizedQuery, /*bIsQuery=*/true, QueryVec) || QueryVec.Num() != Dim)
	{
		return Out;
	}

	// Guard a zero-norm query vector (degenerate input) -> no meaningful cosine.
	double QNormSq = 0.0;
	for (int32 d = 0; d < Dim; ++d)
	{
		QNormSq += static_cast<double>(QueryVec[d]) * static_cast<double>(QueryVec[d]);
	}
	if (QNormSq <= SMALL_NUMBER)
	{
		return Out;
	}

	const bool bFilter = !CategoryFilter.IsEmpty();
	const int32 NumRows = GToolNames.Num();

	// Relevance floor (honesty gate). Brute-force cosine ALWAYS produces a score for
	// every one of the ~700 tools, so without a floor the semantic channel would
	// "recall" the full catalog for ANY query -- even gibberish -- and flood the fused
	// result set. That silently defeats the honest "no genuine retrieval" and "single
	// exact match" contracts the Execute footer + total_matching depend on.
	//
	// L2-normalized BGE embeddings put genuinely-related tool docs well above this
	// floor (~0.3-0.7) while near-orthogonal/unrelated tools sit near 0.0-0.15, so
	// this drops only padding noise and never a genuine semantic match.
	// File-local discriminator + inline constexpr (Linux-strict / unity hygiene).
	constexpr float Cl628_SemanticMinCosine = 0.20f;

	// Brute-force cosine over every row. Vectors are L2-normalized by the model, so
	// the dot product IS the cosine similarity. NOTE: cosine is POSITIVE and
	// higher-is-better -- the OPPOSITE polarity of raw bm25 (negative, lower-better).
	TArray<FClaireonToolCatalogMatch> Scored;
	Scored.Reserve(NumRows);
	for (int32 i = 0; i < NumRows; ++i)
	{
		if (bFilter && !GToolCategories[i].Equals(CategoryFilter, ESearchCase::CaseSensitive))
		{
			continue;
		}

		const float* Row = GEmbeddingMatrix.GetData() + static_cast<int64>(i) * Dim;
		double Dot = 0.0;
		for (int32 d = 0; d < Dim; ++d)
		{
			Dot += static_cast<double>(QueryVec[d]) * static_cast<double>(Row[d]);
		}

		// Honesty floor: skip near-orthogonal/unrelated tools so the semantic channel
		// recalls only genuinely-similar tools, not the whole catalog.
		if (static_cast<float>(Dot) < Cl628_SemanticMinCosine)
		{
			continue;
		}

		FClaireonToolCatalogMatch M;
		M.Name     = GToolNames[i];
		M.Category = GToolCategories[i];
		M.Score    = static_cast<float>(Dot); // cosine in ~[-1, 1]; higher = closer.
		// RankSource is promoted to HybridRRF or LexicalOnlyFallback at RRF fusion.
		M.RankSource = EClaireonRankSource::HybridRRF;
		Scored.Add(MoveTemp(M));
	}

	// Sort by cosine DESC; tie-break by Name ASC for cross-platform determinism
	// (floating-point ties can flip between Win and Linux CI).
	Algo::Sort(Scored, [](const FClaireonToolCatalogMatch& A, const FClaireonToolCatalogMatch& B)
	{
		if (A.Score != B.Score) { return A.Score > B.Score; }
		return A.Name < B.Name;
	});

	const int32 NumOut = FMath::Min(MaxResults, Scored.Num());
	Out.Reserve(NumOut);
	for (int32 i = 0; i < NumOut; ++i)
	{
		Out.Add(MoveTemp(Scored[i]));
	}
	return Out;
}
