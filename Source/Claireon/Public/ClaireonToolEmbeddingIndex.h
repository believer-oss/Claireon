// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "ClaireonEmbeddingModel.h"
#include "ClaireonWordPieceTokenizer.h"
#include "ClaireonToolSearchIndex.h"   // FClaireonToolCatalogMatch

/**
 * Semantic (dense-vector) search index for the Claireon tool catalog.
 *
 * Mirrors FClaireonToolSearchIndex's static-singleton shape:
 *   - Same rebuild trigger (RebuildFromLiveServer called by ClaireonTool_SearchTools).
 *   - Same thread-safety model (internal FCriticalSection).
 *   - FindNearestSemantic() is the public retrieval surface.
 *
 * Internally stores a row-major float matrix [N x Dim] of L2-normalized tool
 * embeddings plus parallel name/category arrays. Similarity is brute-force
 * dot-product (== cosine when both vectors are unit-length), which is sub-ms
 * over ~700 x 384 floats.
 *
 * The model + tokenizer are loaded once on the first RebuildFromLiveServer()
 * call. When the ORT runtime or model files are unavailable, IsReady() returns
 * false and the Execute() surface degrades to the lexical fallback.
 *
 */
class CLAIREON_API FClaireonToolEmbeddingIndex
{
public:
	/**
	 * Atomically rebuild the embedding matrix from the live FClaireonServer
	 * tool registry. Loads the model on the first call (or after Clear()).
	 * Uses the same trigger as FClaireonToolSearchIndex::RebuildFromLiveServer().
	 */
	static void RebuildFromLiveServer();

	/**
	 * Drop the embedding matrix and reset the model/tokenizer state.
	 */
	static void Clear();

	/**
	 * Returns true when the model is loaded and the embedding matrix is
	 * populated. When false, the caller should fall back to the lexical path.
	 */
	static bool IsReady();

	/**
	 * Embed Query, then return the top MaxResults tools ranked by cosine
	 * similarity (brute-force dot-product over the pre-normalized matrix).
	 * CategoryFilter restricts results to a specific category when non-empty.
	 * Returns an empty array when IsReady() is false or no match is found.
	 * FClaireonToolCatalogMatch.Score is the cosine similarity in [0, 1]
	 * (higher = more similar; opposite polarity to raw bm25).
	 */
	static TArray<FClaireonToolCatalogMatch> FindNearestSemantic(
		const FString& Query,
		int32 MaxResults,
		const FString& CategoryFilter = {});

#if WITH_UNTESTED
	/**
	 * Test-only override: installs an alternative embedder meta and FORCES the next
	 * RebuildFromLiveServer() to load it instead of FClaireonEmbedderMeta::Default().
	 * To make the swap take effect it tears down the currently-cached model state
	 * under the lock: clears bModelReady, the ORT session/model, the
	 * tokenizer+vocab, and the embedding matrix, so EnsureModelLoaded_NoLock
	 * re-loads from the override.
	 *
	 * Non-test builds never see this symbol; the production path always uses
	 * Default(). Always pair with ResetModelForTest so subsequent tests revert to
	 * the production default.
	 */
	static void SetModelForTest(const FClaireonEmbedderMeta& Meta);

	/**
	 * Test-only: clear the override meta installed by SetModelForTest and force a
	 * reload back to FClaireonEmbedderMeta::Default(). Tears down the cached model
	 * state the same way so the next RebuildFromLiveServer() reloads the default
	 * model. Safe to call even if no override is active.
	 */
	static void ResetModelForTest();
#endif // WITH_UNTESTED

private:
	/**
	 * Row-major float matrix: GEmbeddingMatrix[i * GEmbeddingDim + d] is the
	 * d-th component of tool i's embedding. Unit-normalized at build time.
	 */
	static TArray<float> GEmbeddingMatrix;

	/**
	 * Embedding dimension (matches FClaireonEmbedderMeta::Dim of the loaded model).
	 */
	static int32 GEmbeddingDim;

	/** Parallel arrays: GToolNames[i] and GToolCategories[i] correspond to row i. */
	static TArray<FString> GToolNames;
	static TArray<FString> GToolCategories;

	/** Guards all static storage. */
	static FCriticalSection GLock;

	/** Singleton embedder (ONNX session + per-model metadata). */
	static FClaireonEmbeddingModel GEmbeddingModel;

	/**
	 * Singleton tokenizer (BERT WordPiece vocab).
	 * Loaded once on the first RebuildFromLiveServer() and cached across rebuilds.
	 */
	static FClaireonWordPieceTokenizer GTokenizer;

	/**
	 * True once the model + tokenizer have loaded successfully. Cached so repeated
	 * RebuildFromLiveServer() calls (OnToolsChanged churn) re-embed without paying
	 * the model/vocab load cost again. Stays false when the ORT runtime / model /
	 * vocab are unavailable -> IsReady() false -> the surface degrades to lexical.
	 */
	static bool bModelReady;

	/**
	 * Internal, NON-locking model+tokenizer load. Idempotent: a no-op once
	 * bModelReady is true. Called under GLock by RebuildFromLiveServer(). Resolves
	 * the vocab path the same way FClaireonEmbeddingModel::Load resolves the model
	 * path (plugin BaseDir / Resources/Models / Meta.ModelDir / Meta.VocabFile).
	 */
	static void EnsureModelLoaded_NoLock();

#if WITH_UNTESTED
	/**
	 * Test-only model override installed by SetModelForTest. When set,
	 * EnsureModelLoaded_NoLock prefers it over FClaireonEmbedderMeta::Default().
	 * When unset (after ResetModelForTest), the production Default() path is used.
	 */
	static TOptional<FClaireonEmbedderMeta> GModelOverrideForTest;
#endif // WITH_UNTESTED
};
