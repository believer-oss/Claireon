// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonToolCatalogMatcher.h"

#include "ClaireonLog.h"
#include "ClaireonToolCatalogAbbreviations.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

namespace ClaireonToolCatalogMatcherInternal
{
	/** Module-static catalog + inverted index.  Guarded by GMatcherLock. */
	static TArray<FClaireonToolCatalogEntry>                          GCatalogEntries;
	static TMap<FString, TArray<TPair<int32, EFieldMask>>>           GInvertedIndex;
	static FCriticalSection                                          GMatcherLock;

	/** Lazily-built lookups derived from ClaireonToolCatalogAbbreviations::GetTable(). */
	static const TMap<FString, TArray<FString>>& GetForwardMap()
	{
		static const TMap<FString, TArray<FString>> Map = []()
		{
			TMap<FString, TArray<FString>> Out;
			for (const ClaireonToolCatalogAbbreviations::FEntry& E : ClaireonToolCatalogAbbreviations::GetTable())
			{
				TArray<FString> Parts;
				FString(E.Expansion).ParseIntoArray(Parts, TEXT(" "), /*CullEmpty=*/true);
				for (FString& P : Parts) { P.ToLowerInline(); }
				Out.Add(FString(E.Key).ToLower(), MoveTemp(Parts));
			}
			return Out;
		}();
		return Map;
	}

	/** Reverse map: canonical term -> abbreviations that expand to it. */
	static const TMap<FString, TArray<FString>>& GetReverseMap()
	{
		static const TMap<FString, TArray<FString>> Map = []()
		{
			TMap<FString, TArray<FString>> Out;
			for (const ClaireonToolCatalogAbbreviations::FEntry& E : ClaireonToolCatalogAbbreviations::GetTable())
			{
				const FString Key = FString(E.Key).ToLower();
				TArray<FString> Parts;
				FString(E.Expansion).ParseIntoArray(Parts, TEXT(" "), /*CullEmpty=*/true);
				for (FString& P : Parts)
				{
					P.ToLowerInline();
					Out.FindOrAdd(P).AddUnique(Key);
				}
			}
			return Out;
		}();
		return Map;
	}

	/** Tokenise: lowercase + split on whitespace + common punctuation + drop tokens < 2 chars. */
	static void Tokenise(const FString& In, TArray<FString>& OutTokens)
	{
		OutTokens.Reset();
		if (In.IsEmpty())
		{
			return;
		}

		const FString Lower = In.ToLower();
		FString Current;
		Current.Reserve(32);

		auto IsSeparator = [](TCHAR C) -> bool
		{
			// whitespace
			if (C == TEXT(' ') || C == TEXT('\t') || C == TEXT('\n') || C == TEXT('\r') || C == TEXT('\f') || C == TEXT('\v'))
			{
				return true;
			}
			// punctuation per spec: . - _ , : ; / \ ( ) [ ] { }
			switch (C)
			{
				case TEXT('.'): case TEXT('-'): case TEXT('_'): case TEXT(','):
				case TEXT(':'): case TEXT(';'): case TEXT('/'): case TEXT('\\'):
				case TEXT('('): case TEXT(')'): case TEXT('['): case TEXT(']'):
				case TEXT('{'): case TEXT('}'):
					return true;
				default:
					return false;
			}
		};

		for (int32 i = 0; i < Lower.Len(); ++i)
		{
			const TCHAR C = Lower[i];
			if (IsSeparator(C))
			{
				if (Current.Len() >= 2)
				{
					OutTokens.Add(Current);
				}
				Current.Reset();
			}
			else
			{
				Current.AppendChar(C);
			}
		}
		if (Current.Len() >= 2)
		{
			OutTokens.Add(Current);
		}
	}

	/**
	 * Tokenise a raw field value and record (token -> field-bit) into the
	 * per-entry mask map.  Also stamps abbreviation expansions and reverse
	 * abbreviations onto the same field bit so a query for `gas` matches a
	 * tool whose Operation field is `gas` (forward) and a tool whose Name
	 * field is `gameplay_ability_system` (reverse).
	 */
	static void IndexField(const FString& FieldValue, EFieldMask FieldBit, TMap<FString, EFieldMask>& OutTokenMaskForEntry)
	{
		TArray<FString> Tokens;
		Tokenise(FieldValue, Tokens);

		const TMap<FString, TArray<FString>>& Forward = GetForwardMap();
		const TMap<FString, TArray<FString>>& Reverse = GetReverseMap();

		for (const FString& Tok : Tokens)
		{
			OutTokenMaskForEntry.FindOrAdd(Tok, EFieldMask::None) |= FieldBit;

			// Forward: field contains an abbreviation (e.g. `gas`) -> also index its expansions.
			if (const TArray<FString>* Expansions = Forward.Find(Tok))
			{
				for (const FString& Exp : *Expansions)
				{
					if (Exp.Len() >= 2)
					{
						OutTokenMaskForEntry.FindOrAdd(Exp, EFieldMask::None) |= FieldBit;
					}
				}
			}

			// Reverse: field contains a canonical term (e.g. `blueprint`) -> also index abbreviations that expand to it.
			if (const TArray<FString>* Abbrevs = Reverse.Find(Tok))
			{
				for (const FString& Abbr : *Abbrevs)
				{
					if (Abbr.Len() >= 2)
					{
						OutTokenMaskForEntry.FindOrAdd(Abbr, EFieldMask::None) |= FieldBit;
					}
				}
			}
		}
	}

	/** Expand a tokenised query in place using forward + reverse abbreviation maps. */
	static void ExpandQueryTokens(TArray<FString>& InOutTokens)
	{
		const TMap<FString, TArray<FString>>& Forward = GetForwardMap();
		const TMap<FString, TArray<FString>>& Reverse = GetReverseMap();

		TArray<FString> Additions;
		for (const FString& Tok : InOutTokens)
		{
			if (const TArray<FString>* Expansions = Forward.Find(Tok))
			{
				for (const FString& Exp : *Expansions)
				{
					Additions.Add(Exp);
				}
			}
			if (const TArray<FString>* Abbrevs = Reverse.Find(Tok))
			{
				for (const FString& Abbr : *Abbrevs)
				{
					Additions.Add(Abbr);
				}
			}
		}
		InOutTokens.Append(MoveTemp(Additions));
	}

	// Overload: expand tokens from Input into Output without modifying Input.
	// Expansion-derived tokens are appended to Output; no length filter is applied.
	static void ExpandQueryTokens(const TArray<FString>& Input, TArray<FString>& Output)
	{
		TArray<FString> Scratch = Input;
		ExpandQueryTokens(Scratch);  // existing in-place expand
		for (const FString& T : Scratch)
		{
			if (!Input.Contains(T))
			{
				Output.AddUnique(T);
			}
		}
	}
}

void FClaireonToolCatalogMatcher::BuildCatalog(const TArray<FClaireonToolCatalogEntry>& Entries)
{
	using namespace ClaireonToolCatalogMatcherInternal;

	FScopeLock Lock(&GMatcherLock);

	GCatalogEntries = Entries;
	GInvertedIndex.Reset();

	for (int32 EntryIdx = 0; EntryIdx < GCatalogEntries.Num(); ++EntryIdx)
	{
		const FClaireonToolCatalogEntry& E = GCatalogEntries[EntryIdx];

		// Tokenise each field separately; OR the bits when a token appears in multiple fields.
		// Each call also stamps abbreviation expansions / reverse abbreviations onto the same field bit.
		TMap<FString, EFieldMask> TokenMaskForEntry;
		IndexField(E.Name,        EFieldMask::Name,        TokenMaskForEntry);
		IndexField(E.Category,    EFieldMask::Category,    TokenMaskForEntry);
		for (const FString& Keyword : E.Keywords)
		{
			IndexField(Keyword, EFieldMask::Keywords, TokenMaskForEntry);
		}
		IndexField(E.Operation,   EFieldMask::Operation,   TokenMaskForEntry);
		IndexField(E.Description, EFieldMask::Description, TokenMaskForEntry);

		for (const TPair<FString, EFieldMask>& KV : TokenMaskForEntry)
		{
			GInvertedIndex.FindOrAdd(KV.Key).Add(TPair<int32, EFieldMask>(EntryIdx, KV.Value));
		}
	}

	UE_LOG(LogClaireon, Verbose,
		TEXT("[ToolCatalogMatcher] BuildCatalog: %d entries, %d unique tokens"),
		GCatalogEntries.Num(), GInvertedIndex.Num());
}

int32 FClaireonToolCatalogMatcher::DistanceBounded(const FString& A, const FString& B, int32 MaxDistance)
{
	const int32 LenA = A.Len();
	const int32 LenB = B.Len();

	// Early exit on gross length mismatch.
	if (FMath::Abs(LenA - LenB) > MaxDistance)
	{
		return MaxDistance + 1;
	}
	if (LenA == 0) { return LenB; }
	if (LenB == 0) { return LenA; }

	TArray<int32> Prev;
	TArray<int32> Curr;
	Prev.SetNumUninitialized(LenB + 1);
	Curr.SetNumUninitialized(LenB + 1);

	for (int32 j = 0; j <= LenB; ++j)
	{
		Prev[j] = j;
	}

	for (int32 i = 1; i <= LenA; ++i)
	{
		Curr[0] = i;
		int32 RowMin = Curr[0];
		const TCHAR Ca = A[i - 1];
		for (int32 j = 1; j <= LenB; ++j)
		{
			const TCHAR Cb = B[j - 1];
			const int32 Cost = (Ca == Cb) ? 0 : 1;
			const int32 Del = Prev[j] + 1;
			const int32 Ins = Curr[j - 1] + 1;
			const int32 Sub = Prev[j - 1] + Cost;
			int32 V = Del < Ins ? Del : Ins;
			if (Sub < V) { V = Sub; }
			Curr[j] = V;
			if (V < RowMin) { RowMin = V; }
		}
		if (RowMin > MaxDistance)
		{
			// No cell in this row can descend below MaxDistance -> give up.
			return MaxDistance + 1;
		}
		Swap(Prev, Curr);
	}

	return Prev[LenB];
}

namespace ClaireonToolCatalogMatcherInternal
{
	// Field weights compiled in for v1.
	static constexpr float WeightName        = 8.0f;
	static constexpr float WeightCategory    = 4.0f;
	static constexpr float WeightKeywords    = 3.0f;
	static constexpr float WeightOperation   = 3.0f;
	static constexpr float WeightDescription = 1.0f;

	/** Return the MAX of compiled-in field weights for every bit set in Mask. */
	static float MaxFieldWeight(EFieldMask Mask)
	{
		float Best = 0.0f;
		if (EnumHasAnyFlags(Mask, EFieldMask::Name))        { Best = FMath::Max(Best, WeightName); }
		if (EnumHasAnyFlags(Mask, EFieldMask::Category))    { Best = FMath::Max(Best, WeightCategory); }
		if (EnumHasAnyFlags(Mask, EFieldMask::Keywords))    { Best = FMath::Max(Best, WeightKeywords); }
		if (EnumHasAnyFlags(Mask, EFieldMask::Operation))   { Best = FMath::Max(Best, WeightOperation); }
		if (EnumHasAnyFlags(Mask, EFieldMask::Description)) { Best = FMath::Max(Best, WeightDescription); }
		return Best;
	}
}

TArray<FClaireonToolCatalogMatch> FClaireonToolCatalogMatcher::FindNearest(const FString& Query, int32 MaxResults)
{
	using namespace ClaireonToolCatalogMatcherInternal;

	TArray<FClaireonToolCatalogMatch> Out;
	if (MaxResults <= 0)
	{
		return Out;
	}

	FScopeLock Lock(&GMatcherLock);

	if (GCatalogEntries.Num() == 0 || GInvertedIndex.Num() == 0)
	{
		return Out;
	}

	TArray<FString> TokeniseRawTokens;
	Tokenise(Query, TokeniseRawTokens);

	// Query-side min-length cutoff: drop fuzzy-match terms of length <= 2
	// unless every token in the query is short. The asymmetry vs the index
	// (which keeps 2-char tokens) is intentional -- short tokens like `ing`
	// would otherwise match hundreds of unrelated tools.
	TArray<FString> KeptTokens;
	TArray<FString> DroppedTokens;
	for (const FString& T : TokeniseRawTokens)
	{
		(T.Len() > 2 ? KeptTokens : DroppedTokens).Add(T);
	}

	// Expand AFTER cutoff so expansion-derived short tokens (e.g. "bp" from
	// "blueprint") bypass the > 2 filter. Expansion runs on KeptTokens only
	// (long user-typed tokens), not on DroppedTokens, to avoid noisy matches.
	TArray<FString> ExpansionTokens;
	ExpandQueryTokens(KeptTokens, ExpansionTokens);

	// Scorer sees: KeptTokens (long user tokens) + ExpansionTokens (derived).
	// DroppedTokens is the all-short fallback path only.
	const TArray<FString>& RawQueryTokens = (KeptTokens.Num() > 0) ? KeptTokens : DroppedTokens;

	// Unique-ify query tokens (preserving stable iteration order).
	// Also merge ExpansionTokens (expansion-derived short tokens that bypassed the length filter).
	TArray<FString> QueryTokens;
	{
		TSet<FString> Seen;
		Seen.Reserve(RawQueryTokens.Num() + ExpansionTokens.Num());
		for (const FString& Tok : RawQueryTokens)
		{
			bool bWasInSet = false;
			Seen.Add(Tok, &bWasInSet);
			if (!bWasInSet)
			{
				QueryTokens.Add(Tok);
			}
		}
		for (const FString& Tok : ExpansionTokens)
		{
			bool bWasInSet = false;
			Seen.Add(Tok, &bWasInSet);
			if (!bWasInSet)
			{
				QueryTokens.Add(Tok);
			}
		}
	}

	if (QueryTokens.Num() == 0 && DroppedTokens.Num() == 0)
	{
		return Out;
	}

	// Short-token whole-word match bucket: user-typed short tokens (length <= 2)
	// that are NOT already covered by KeptTokens or ExpansionTokens. These tokens
	// match ONLY tool name tokens and GetSearchKeywords tokens -- NOT description
	// text -- using exact whole-word (case-insensitive) comparison. Substring
	// matching against description body is explicitly forbidden for short tokens.
	//
	// Note: only populated when KeptTokens is non-empty. When KeptTokens is empty
	// (all-short fallback), DroppedTokens are already in QueryTokens via RawQueryTokens
	// and must NOT be double-scored via ShortMatchTokens.
	TArray<FString> ShortMatchTokens;
	if (KeptTokens.Num() > 0)
	{
		for (const FString& Tok : DroppedTokens)
		{
			if (!KeptTokens.Contains(Tok) && !ExpansionTokens.Contains(Tok))
			{
				ShortMatchTokens.AddUnique(Tok);
			}
		}
	}

	if (QueryTokens.Num() == 0 && ShortMatchTokens.Num() == 0)
	{
		return Out;
	}

	// Per-entry weighted score accumulator + distinct-query-tokens-matched
	// counter. Each per-token contribution = MAX_OVER_FIELDS(weight) * hit-class
	// multiplier (exact 2.0, prefix 1.0, fuzzy 0.5). "Name prefix = 4.0" falls
	// out naturally as Name's exact weight (8) * the prefix multiplier (0.5).
	TArray<float> WeightedScore; WeightedScore.Init(0.0f, GCatalogEntries.Num());
	TArray<int32> DistinctTokenHits; DistinctTokenHits.Init(0, GCatalogEntries.Num());

	for (const FString& QToken : QueryTokens)
	{
		// Track which entries this query token has contributed to, and with
		// which best (per-entry) max-field weight, so a token that hits the
		// same entry via exact AND prefix only counts once (the higher class).
		TMap<int32, float> EntryBestContribution;

		auto Credit = [&](int32 Idx, float Contribution)
		{
			float& Existing = EntryBestContribution.FindOrAdd(Idx, 0.0f);
			if (Contribution > Existing)
			{
				Existing = Contribution;
			}
		};

		// (a) exact matches: per posting, MAX over set fields * 2.0.
		if (TArray<TPair<int32, EFieldMask>>* ExactEntries = GInvertedIndex.Find(QToken))
		{
			for (const TPair<int32, EFieldMask>& Posting : *ExactEntries)
			{
				const float Contribution = MaxFieldWeight(Posting.Value) * 2.0f;
				Credit(Posting.Key, Contribution);
			}
		}

		// (b) prefix matches: strict prefix, * 1.0.
		for (const auto& Pair : GInvertedIndex)
		{
			const FString& IndexTok = Pair.Key;
			if (IndexTok.Len() <= QToken.Len())
			{
				continue;
			}
			if (!IndexTok.StartsWith(QToken, ESearchCase::CaseSensitive))
			{
				continue;
			}
			for (const TPair<int32, EFieldMask>& Posting : Pair.Value)
			{
				const float Contribution = MaxFieldWeight(Posting.Value) * 1.0f;
				Credit(Posting.Key, Contribution);
			}
		}

		// (c) fuzzy Levenshtein pass only when neither exact nor prefix
		// produced credit for this query token. * 0.5.
		if (EntryBestContribution.Num() == 0)
		{
			const int32 QLen = QToken.Len();
			if (QLen >= 4)
			{
				for (const auto& Pair : GInvertedIndex)
				{
					const FString& IndexTok = Pair.Key;
					const int32 ILen = IndexTok.Len();
					if (FMath::Min(QLen, ILen) < 4)
					{
						continue;
					}
					if (FMath::Abs(QLen - ILen) > 2)
					{
						continue;
					}
					const int32 Dist = FClaireonToolCatalogMatcher::DistanceBounded(QToken, IndexTok, 2);
					if (Dist <= 2)
					{
						for (const TPair<int32, EFieldMask>& Posting : Pair.Value)
						{
							const float Contribution = MaxFieldWeight(Posting.Value) * 0.5f;
							Credit(Posting.Key, Contribution);
						}
					}
				}
			}
		}

		// Fold this token's contributions into per-entry accumulators.
		for (const TPair<int32, float>& KV : EntryBestContribution)
		{
			WeightedScore[KV.Key] += KV.Value;
			DistinctTokenHits[KV.Key] += 1;
		}
	}

	// Short-token scoring: exact whole-word match against Name and Keywords ONLY.
	// Description text is NOT scanned -- substring matching against descriptions
	// is explicitly forbidden for short tokens to prevent "ed" matching "edit blueprint graph".
	// Weight mirrors the existing exact name-token hit: WeightName * 2.0.
	for (const FString& QToken : ShortMatchTokens)
	{
		TMap<int32, float> EntryBestContribution;

		auto CreditShort = [&](int32 Idx, float Contribution)
		{
			float& Existing = EntryBestContribution.FindOrAdd(Idx, 0.0f);
			if (Contribution > Existing)
			{
				Existing = Contribution;
			}
		};

		// Exact match in the inverted index, but filter to Name + Keywords fields only.
		// A posting with EFieldMask::Description (and no Name/Keywords bit) is excluded.
		if (TArray<TPair<int32, EFieldMask>>* ExactEntries = GInvertedIndex.Find(QToken))
		{
			for (const TPair<int32, EFieldMask>& Posting : *ExactEntries)
			{
				// Only credit if the token appears in Name or Keywords (not solely in Description/Operation).
				const EFieldMask NameKeywordMask = EFieldMask::Name | EFieldMask::Keywords;
				if (!EnumHasAnyFlags(Posting.Value, NameKeywordMask))
				{
					continue;
				}
				// Use the field weight for only the Name/Keywords bits to exclude Description weight.
				float Best = 0.0f;
				if (EnumHasAnyFlags(Posting.Value, EFieldMask::Name))     { Best = FMath::Max(Best, WeightName); }
				if (EnumHasAnyFlags(Posting.Value, EFieldMask::Keywords)) { Best = FMath::Max(Best, WeightKeywords); }
				const float Contribution = Best * 2.0f;
				CreditShort(Posting.Key, Contribution);
			}
		}

		for (const TPair<int32, float>& KV : EntryBestContribution)
		{
			WeightedScore[KV.Key] += KV.Value;
			DistinctTokenHits[KV.Key] += 1;
		}
	}

	// Denominator includes both long query tokens and short match tokens so
	// normalization accounts for the full effective query length.
	const int32 TotalTokenCount = QueryTokens.Num() + ShortMatchTokens.Num();
	const float InvDenom = 1.0f / static_cast<float>(FMath::Max(1, TotalTokenCount));

	struct FScoredEntry
	{
		int32  EntryIdx;
		float  Score;
		int32  DistinctTokens;
	};
	TArray<FScoredEntry> Scored;
	Scored.Reserve(GCatalogEntries.Num());

	for (int32 i = 0; i < GCatalogEntries.Num(); ++i)
	{
		if (DistinctTokenHits[i] == 0)
		{
			continue;
		}
		Scored.Add({ i, WeightedScore[i] * InvDenom, DistinctTokenHits[i] });
	}

	if (Scored.Num() == 0)
	{
		return Out;
	}

	Scored.Sort([](const FScoredEntry& A, const FScoredEntry& B)
	{
		// 1st: more distinct query tokens hit wins.
		if (A.DistinctTokens != B.DistinctTokens)
		{
			return A.DistinctTokens > B.DistinctTokens;
		}
		// 2nd: higher score wins.
		if (A.Score != B.Score)
		{
			return A.Score > B.Score;
		}
		// 3rd: name ascending for cross-platform determinism.
		return FCString::Strcmp(*GCatalogEntries[A.EntryIdx].Name, *GCatalogEntries[B.EntryIdx].Name) < 0;
	});

	const int32 Take = FMath::Min(MaxResults, Scored.Num());
	Out.Reserve(Take);
	for (int32 i = 0; i < Take; ++i)
	{
		const FClaireonToolCatalogEntry& Entry = GCatalogEntries[Scored[i].EntryIdx];
		FClaireonToolCatalogMatch M;
		M.Name = Entry.Name;
		M.Category = Entry.Category;
		M.Score = Scored[i].Score;
		// Distinct query tokens that produced any exact/prefix/fuzzy hit for
		// this entry. Surfaced to the tool_search JSON payload as
		// `query_tokens_matched` so future zero-result regressions are
		// diagnosable without re-running the matcher.
		M.TokensMatched = Scored[i].DistinctTokens;
		Out.Add(MoveTemp(M));
	}

	return Out;
}

void FClaireonToolCatalogMatcher::Clear()
{
	using namespace ClaireonToolCatalogMatcherInternal;
	FScopeLock Lock(&GMatcherLock);
	GCatalogEntries.Reset();
	GInvertedIndex.Reset();
}
