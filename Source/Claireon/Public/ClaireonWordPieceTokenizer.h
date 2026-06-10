// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

/**
 * Lightweight BERT WordPiece tokenizer for sentence embedding.
 *
 * Loads a `vocab.txt` (30522-token HuggingFace BERT vocabulary), then encodes
 * text strings into token-id / attention-mask pairs compatible with BERT-family
 * ONNX models (BGE-small-en-v1.5 and similar).
 *
 * Encoding steps (bert-base-uncased pipeline):
 *   1. BasicTokenizer (do_lower_case=true):
 *      - Strip control chars (keep \t \n \r as whitespace); normalize whitespace runs.
 *      - Lowercase via FString::ToLower().
 *      - Strip combining/accent chars: text is NFD-decomposed via FText and code-points
 *        in the Unicode Mn (nonspacing mark) range [0x0300, 0x036F] are dropped.
 *        Full ICU NFD is not exposed in UE5's public API; we rely on the BERT
 *        bert-base-uncased vocab being trained on pre-normalized English tool docs
 *        so accent decomposition rarely matters in practice (documented simplification).
 *      - Whitespace-split into raw tokens.
 *      - Split each raw token on punctuation (ASCII !"#$%&'()*+,-./:;<=>?@[\]^_`{|}~
 *        plus any Unicode codepoint >= 0x80 in the P (punctuation) / S (symbol) categories that UE can
 *        detect cheaply). Each punctuation char becomes its own standalone token.
 *   2. WordPiece greedy longest-match per basic token:
 *      - Tokens longer than MaxInputCharsPerWord (100) -> emit [UNK] for the whole word.
 *      - Greedy: find longest prefix in vocab; continuation sub-words use "##" prefix.
 *      - If any start position finds no match the whole token collapses to [UNK].
 *   3. Assemble: [CLS] + piece-ids + [SEP]; truncate pieces so total <= MaxLen, then
 *      pad with [PAD]=0 to exactly MaxLen. attention_mask=1 for real tokens, 0 for pad.
 *
 * Fully unit-testable against known HuggingFace token-id fixtures.
 */
class CLAIREON_API FClaireonWordPieceTokenizer
{
public:
	FClaireonWordPieceTokenizer() = default;
	~FClaireonWordPieceTokenizer() = default;

	// Non-copyable (vocab map can be large).
	FClaireonWordPieceTokenizer(const FClaireonWordPieceTokenizer&) = delete;
	FClaireonWordPieceTokenizer& operator=(const FClaireonWordPieceTokenizer&) = delete;
	FClaireonWordPieceTokenizer(FClaireonWordPieceTokenizer&&) = default;
	FClaireonWordPieceTokenizer& operator=(FClaireonWordPieceTokenizer&&) = default;

	/**
	 * Load vocabulary from a `vocab.txt` file (one token per line, line index ==
	 * token id). Must be called before Encode(). Returns false on IO or parse error.
	 * Verifies that [PAD]=0, [UNK]=100, [CLS]=101, [SEP]=102 are present.
	 */
	bool LoadVocab(const FString& VocabPath);

	/**
	 * Returns true when LoadVocab() succeeded and the vocabulary is ready.
	 */
	bool IsReady() const;

	/**
	 * Encode Text into token ids and an attention mask, both of length MaxLen.
	 * Clears OutIds and OutMask before filling them. When the tokenizer is not
	 * ready (IsReady() == false) both arrays are left empty.
	 *
	 * Signature matches ClaireonEmbeddingModel.cpp line 305:
	 *   Tokenizer->Encode(Prefixed, Meta.SeqLen, Ids, Mask)
	 */
	void Encode(const FString& Text, int32 MaxLen,
		TArray<int32>& OutIds, TArray<int32>& OutMask) const;

private:
	/** Token -> id lookup. Populated by LoadVocab(). */
	TMap<FString, int32> VocabMap;

	/** Cached special-token ids. Set by LoadVocab(). */
	int32 IdPad  = 0;
	int32 IdUnk  = 100;
	int32 IdCls  = 101;
	int32 IdSep  = 102;

	/** True after a successful LoadVocab(). */
	bool bReady  = false;
};
