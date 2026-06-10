// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "UObject/StrongObjectPtr.h"

// Forward-declare the NNE model-instance type to keep the NNE headers out of
// the public interface. The full include lives in ClaireonEmbeddingModel.cpp.
namespace UE::NNE
{
	class IModelInstanceCPU;
}

class UNNEModelData;
class FClaireonWordPieceTokenizer;

/**
 * Per-model metadata controlling embedding behaviour.
 *
 * Swapping models means swapping the model binary AND this struct together, so
 * pooling/prefix/normalization settings never silently mismatch the weights.
 */
struct FClaireonEmbedderMeta
{
	/**
	 * Folder under Plugins/Claireon/Resources/Models/ holding model.onnx +
	 * vocab.txt + MODEL_INFO.json + LICENSE.txt (e.g. "bge-small-en-v1.5-int8").
	 */
	FString ModelDir;

	/** Filename of the ONNX model inside ModelDir. */
	FString ModelFile = TEXT("model.onnx");

	/** Filename of the vocab.txt inside ModelDir. */
	FString VocabFile = TEXT("vocab.txt");

	/** Embedding dimension (e.g. 384 for BGE-small). */
	int32 Dim = 384;

	/** Fixed sequence length for inference (tool docs are short; 64 is ample). */
	int32 SeqLen = 64;

	/** Pooling strategy applied over the last-hidden-state tensor. */
	enum class EPooling : uint8
	{
		/** Mean pool over non-padding tokens (sentence-transformer style). */
		Mean,
		/** Take the CLS token (index 0) representation (BGE retrieval). */
		Cls,
	};
	EPooling Pooling = EPooling::Mean;

	/**
	 * Prefix prepended to the query text before encoding (model-specific).
	 * Empty for mean-pool models; BGE-style retrieval models use e.g.
	 * "Represent this sentence for searching relevant passages: ".
	 */
	FString QueryPrefix;

	/**
	 * Prefix prepended to document text before encoding (model-specific).
	 * Empty for most models.
	 */
	FString DocPrefix;

	/**
	 * When true (default) the output vector is L2-normalised to unit length so
	 * dot-product == cosine similarity without a division.
	 */
	bool bNormalize = true;

	/**
	 * Metadata for bge-small-en-v1.5 (int8). CLS pooling, query-side instruction
	 * prefix, 384-d. Matches Resources/Models/bge-small-en-v1.5-int8/MODEL_INFO.json.
	 *
	 * This is the shipped model. To introduce a different model, add a sibling
	 * factory here and vendor its Resources/Models/<dir>/; the struct is model-agnostic.
	 */
	static FClaireonEmbedderMeta BGE();

	/** The active shipped model: bge-small-en-v1.5-int8. */
	static FClaireonEmbedderMeta Default() { return BGE(); }
};

/**
 * Thin wrapper around an NNE/ORT CPU inference session for sentence embedding.
 *
 * A single instance owns one IModelInstanceCPU session. Callers load the model
 * once via Load(), then call Embed() per query/document. Thread safety is the
 * caller's responsibility -- FClaireonToolEmbeddingIndex holds the critical section
 * and serialises all calls into a single instance; this class is NOT internally
 * synchronised (RunSync mutates session state).
 */
class CLAIREON_API FClaireonEmbeddingModel
{
public:
	// Special members are declared here but DEFINED out-of-line in the .cpp.
	// The TStrongObjectPtr<UNNEModelData> member requires UNNEModelData's complete
	// type to instantiate its constructor/destructor; UNNEModelData is only
	// forward-declared in this public header (NNE is a private dependency and must
	// not leak into the public interface). Defining these in the .cpp -- where
	// NNEModelData.h is included -- defers that instantiation to where the type is
	// complete.
	FClaireonEmbeddingModel();
	~FClaireonEmbeddingModel();

	// Non-copyable; moving is fine (TSharedPtr / TStrongObjectPtr are movable).
	FClaireonEmbeddingModel(const FClaireonEmbeddingModel&) = delete;
	FClaireonEmbeddingModel& operator=(const FClaireonEmbeddingModel&) = delete;
	FClaireonEmbeddingModel(FClaireonEmbeddingModel&&);
	FClaireonEmbeddingModel& operator=(FClaireonEmbeddingModel&&);

	/**
	 * Load the ONNX model described by Meta via the NNERuntimeORTCpu runtime.
	 * Resolves paths relative to the Claireon plugin base dir.
	 * Returns false when the runtime, model file, or session creation fails;
	 * the surface degrades to the lexical fallback in that case.
	 *
	 * Note: this loads the embedding *model* only. The WordPiece tokenizer is a
	 * separate component loaded by the caller; Embed() requires it,
	 * EmbedTokenIds() does not.
	 */
	bool Load(const FClaireonEmbedderMeta& InMeta);

	/** Returns true when Load() succeeded and the session is ready for inference. */
	bool IsReady() const;

	/**
	 * Set a non-owning WordPiece tokenizer used by Embed(text). The caller (the
	 * embedding index) owns the tokenizer + its vocab; this model only borrows it
	 * so Embed(text) can tokenize. When unset (or the tokenizer is not ready),
	 * Embed(text) returns false. EmbedTokenIds() never uses this.
	 */
	void SetTokenizer(const FClaireonWordPieceTokenizer* InTokenizer) { Tokenizer = InTokenizer; }

	/**
	 * Embed a single string into a dense float vector of length Dim().
	 * bIsQuery controls whether Meta.QueryPrefix or Meta.DocPrefix is applied.
	 * OutVec is reset and filled; returns false on any failure.
	 *
	 * Requires a ready WordPiece tokenizer. Until the tokenizer is available this
	 * returns false. The NNE inference path can be validated independently via
	 * EmbedTokenIds() without a tokenizer.
	 */
	bool Embed(const FString& Text, bool bIsQuery, TArray<float>& OutVec);

	/**
	 * Embed a pre-tokenized id/mask pair. Both arrays are interpreted at the
	 * model's fixed Meta.SeqLen: shorter inputs are zero-padded (id=0, mask=0),
	 * longer inputs are truncated. token_type_ids (if the model declares one) are
	 * zeroed. OutVec is reset and filled with Dim() floats; returns false on any
	 * failure.
	 *
	 * This is the tokenizer-independent path for feeding known HuggingFace token
	 * ids and validating NNE inference + pooling + normalization in isolation.
	 * It works whenever IsReady() is true.
	 */
	bool EmbedTokenIds(TConstArrayView<int32> Ids, TConstArrayView<int32> Mask, TArray<float>& OutVec);

	/**
	 * The embedding dimension specified by the loaded model's FClaireonEmbedderMeta.
	 * Returns 0 when the model is not loaded.
	 */
	int32 Dim() const;

private:
	/** Per-model pooling/prefix/normalization metadata set on Load(). */
	FClaireonEmbedderMeta Meta;

	/**
	 * Owns the imported ONNX bytes as a UObject. UNNEModelData is a UObject and
	 * FClaireonEmbeddingModel is a plain C++ class, so it cannot be a UPROPERTY.
	 * TStrongObjectPtr keeps it rooted against GC for the lifetime of this model
	 * (a raw UObject* here would be collected and crash). Kept resident because
	 * the runtime may reference it; cleared only on destruction / re-Load.
	 */
	TStrongObjectPtr<UNNEModelData> ModelData;

	/**
	 * Cached NNE CPU model instance. Null until Load() succeeds.
	 * IModelInstanceCPU is non-copyable; held via shared pointer so the embedding
	 * index can hold FClaireonEmbeddingModel by value inside a critical-section guard.
	 */
	TSharedPtr<UE::NNE::IModelInstanceCPU> ModelInstance;

	/**
	 * Cached per-input-tensor element type (in declaration order), learned from
	 * GetInputTensorDescs() at Load(). The model decides int32 vs int64 ids; we
	 * must marshal to the exact declared type or RunSync produces garbage.
	 */
	TArray<uint8> InputElemTypes;

	/**
	 * Index of each known input within the model's declared input list, or
	 * INDEX_NONE when the model does not declare it. token_type_ids is optional
	 * (BERT-family ONNX exports usually include it but some do not).
	 */
	int32 InputIdxInputIds = INDEX_NONE;
	int32 InputIdxAttentionMask = INDEX_NONE;
	int32 InputIdxTokenTypeIds = INDEX_NONE;

	/**
	 * Non-owning tokenizer for Embed(text). Owned by the embedding index; may be
	 * null (then Embed(text) returns false). Never used by EmbedTokenIds().
	 */
	const FClaireonWordPieceTokenizer* Tokenizer = nullptr;

	/**
	 * True when the model's single output is the unpooled last_hidden_state
	 * [1, SeqLen, Dim] (mean/CLS pooling applied here). False when the export
	 * already emits a pooled sentence embedding [1, Dim] (used directly).
	 * Determined from GetOutputTensorDescs() at Load().
	 */
	bool bOutputIsLastHiddenState = true;
};
