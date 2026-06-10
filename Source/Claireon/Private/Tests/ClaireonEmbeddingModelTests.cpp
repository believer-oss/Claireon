// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

// ===========================================================================
// Smoke tests for the NNE/ORT ONNX sentence embedder.
//
// Validates the NNE inference path in isolation from the WordPiece tokenizer:
// feeds known HuggingFace bert-base-uncased token ids via EmbedTokenIds() and
// asserts the model produces a correctly-shaped, nonzero, unit-L2 vector, plus
// a directional cosine sanity check.
//
// Token ids below are looked up from the committed vocab.txt (id = 0-indexed
// line number; specials [CLS]=101 [SEP]=102 [PAD]=0 are standard bert-uncased):
//   create=3443  table=2795  list=2862  open=2330  system=2291  file=5371
//
// If the NNERuntimeORTCpu runtime is unavailable (e.g. a commandlet without the
// ORT plugin), the test logs a skip and passes -- a true pass/fail requires a
// live editor run where ORT is present.
//
// v2/Linux hygiene: file-local namespace discriminator; no UNTEST_* macros
// inside lambdas; bool-returning helpers.
// ===========================================================================

#include "Untest.h"
#include "ClaireonEmbeddingModel.h"
#include "Math/UnrealMathUtility.h"

namespace Cl628EmbTestNS
{
	// Build an attention mask of N ones (one per real token); EmbedTokenIds
	// zero-pads ids+mask to SeqLen internally, so we only pass the real tokens.
	static void Ones(int32 N, TArray<int32>& OutMask)
	{
		OutMask.Reset();
		OutMask.Init(1, N);
	}

	// Plain cosine similarity (no normalization assumption).
	static float Cosine(const TArray<float>& A, const TArray<float>& B)
	{
		if (A.Num() != B.Num() || A.Num() == 0) { return 0.0f; }
		double Dot = 0.0, NA = 0.0, NB = 0.0;
		for (int32 i = 0; i < A.Num(); ++i)
		{
			Dot += static_cast<double>(A[i]) * B[i];
			NA  += static_cast<double>(A[i]) * A[i];
			NB  += static_cast<double>(B[i]) * B[i];
		}
		if (NA <= 0.0 || NB <= 0.0) { return 0.0f; }
		return static_cast<float>(Dot / (FMath::Sqrt(NA) * FMath::Sqrt(NB)));
	}

	static bool IsAllZero(const TArray<float>& V)
	{
		for (float F : V) { if (F != 0.0f) { return false; } }
		return true;
	}
}

// ---------------------------------------------------------------------------
// Smoke 1: model loads and produces a shaped, nonzero, unit-norm vector.
// ---------------------------------------------------------------------------
UNTEST_UNIT(Claireon, Embedder, LoadsAndEmbeds)
{
	using namespace Cl628EmbTestNS;

	FClaireonEmbeddingModel Model;
	const bool bLoaded = Model.Load(FClaireonEmbedderMeta::Default());
	if (!bLoaded || !Model.IsReady())
	{
		UE_LOG(LogTemp, Display,
			TEXT("[Embedder] NNERuntimeORTCpu/model unavailable -- skipping smoke "
			     "(run in a live editor with the ORT plugin to validate)."));
		co_return;
	}

	UNTEST_EXPECT_EQ(Model.Dim(), 384);

	// "[CLS] create table [SEP]"
	const TArray<int32> Ids = { 101, 3443, 2795, 102 };
	TArray<int32> Mask; Ones(Ids.Num(), Mask);

	TArray<float> Vec;
	const bool bEmbedded = Model.EmbedTokenIds(Ids, Mask, Vec);
	UNTEST_ASSERT_TRUE(bEmbedded);
	UNTEST_EXPECT_EQ(Vec.Num(), 384);
	UNTEST_EXPECT_FALSE(IsAllZero(Vec));

	// Meta defaults to L2-normalized output -> unit length.
	double Norm = 0.0;
	for (float F : Vec) { Norm += static_cast<double>(F) * F; }
	Norm = FMath::Sqrt(Norm);
	UNTEST_EXPECT_TRUE(FMath::IsNearlyEqual(static_cast<float>(Norm), 1.0f, 1e-3f));

	co_return;
}

// ---------------------------------------------------------------------------
// Smoke 2: directional cosine sanity -- a sentence is more similar to a near
// duplicate than to an unrelated one. cos(A,B) > cos(A,C).
//   A = "create table"        B = "create table list"     C = "open system file"
// ---------------------------------------------------------------------------
UNTEST_UNIT(Claireon, Embedder, RelativeCosineSane)
{
	using namespace Cl628EmbTestNS;

	FClaireonEmbeddingModel Model;
	if (!Model.Load(FClaireonEmbedderMeta::Default()) || !Model.IsReady())
	{
		UE_LOG(LogTemp, Display,
			TEXT("[Embedder] runtime unavailable -- skipping cosine sanity."));
		co_return;
	}

	const TArray<int32> A = { 101, 3443, 2795, 102 };          // create table
	const TArray<int32> B = { 101, 3443, 2795, 2862, 102 };    // create table list
	const TArray<int32> C = { 101, 2330, 2291, 5371, 102 };    // open system file

	TArray<int32> MA, MB, MC;
	Ones(A.Num(), MA); Ones(B.Num(), MB); Ones(C.Num(), MC);

	TArray<float> VA, VB, VC;
	const bool bA = Model.EmbedTokenIds(A, MA, VA);
	const bool bB = Model.EmbedTokenIds(B, MB, VB);
	const bool bC = Model.EmbedTokenIds(C, MC, VC);
	UNTEST_ASSERT_TRUE(bA && bB && bC);

	const float SimAB = Cosine(VA, VB);
	const float SimAC = Cosine(VA, VC);
	UE_LOG(LogTemp, Display,
		TEXT("[Embedder] cosine(A,B near-dup)=%.4f  cosine(A,C unrelated)=%.4f"),
		SimAB, SimAC);
	UNTEST_EXPECT_TRUE(SimAB > SimAC);

	co_return;
}

#endif // WITH_UNTESTED
