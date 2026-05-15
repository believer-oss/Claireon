// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonToolCatalogMatcher.h"

#include "ClaireonLog.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

namespace ClaireonToolCatalogMatcherInternal
{
	/** Module-static catalog + inverted index.  Guarded by GMatcherLock. */
	static TArray<FClaireonToolCatalogEntry>  GCatalogEntries;
	static TMap<FString, TArray<int32>>     GInvertedIndex;
	static FCriticalSection                 GMatcherLock;

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
	 * Bounded Levenshtein distance with early-exit at MaxDistance.  Returns
	 * MaxDistance + 1 as a sentinel when the true distance exceeds the bound.
	 * Standard DP with two rolling rows.
	 */
	static int32 LevenshteinBounded(const FString& A, const FString& B, int32 MaxDistance)
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
}

void FClaireonToolCatalogMatcher::BuildCatalog(const TArray<FClaireonToolCatalogEntry>& Entries)
{
	using namespace ClaireonToolCatalogMatcherInternal;

	FScopeLock Lock(&GMatcherLock);

	GCatalogEntries = Entries;
	GInvertedIndex.Reset();

	TArray<FString> Tokens;
	for (int32 EntryIdx = 0; EntryIdx < GCatalogEntries.Num(); ++EntryIdx)
	{
		Tokenise(GCatalogEntries[EntryIdx].EnrichedText, Tokens);

		// Deduplicate per-entry so an entry only appears once per unique token.
		TSet<FString> Seen;
		Seen.Reserve(Tokens.Num());
		for (const FString& Tok : Tokens)
		{
			bool bWasInSet = false;
			Seen.Add(Tok, &bWasInSet);
			if (bWasInSet)
			{
				continue;
			}
			GInvertedIndex.FindOrAdd(Tok).Add(EntryIdx);
		}
	}

	UE_LOG(LogClaireon, Verbose,
		TEXT("[ToolCatalogMatcher] BuildCatalog: %d entries, %d unique tokens"),
		GCatalogEntries.Num(), GInvertedIndex.Num());
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

	TArray<FString> RawQueryTokens;
	Tokenise(Query, RawQueryTokens);

	// Unique-ify query tokens (preserving stable iteration order).
	TArray<FString> QueryTokens;
	{
		TSet<FString> Seen;
		Seen.Reserve(RawQueryTokens.Num());
		for (const FString& Tok : RawQueryTokens)
		{
			bool bWasInSet = false;
			Seen.Add(Tok, &bWasInSet);
			if (!bWasInSet)
			{
				QueryTokens.Add(Tok);
			}
		}
	}

	if (QueryTokens.Num() == 0)
	{
		return Out;
	}

	// Per-entry per-query-token hit counts, accumulated into raw score at end.
	// Using parallel arrays indexed by entry index.
	TArray<int32> ExactHits;  ExactHits.Init(0, GCatalogEntries.Num());
	TArray<int32> PrefixHits; PrefixHits.Init(0, GCatalogEntries.Num());
	TArray<int32> FuzzyHits;  FuzzyHits.Init(0, GCatalogEntries.Num());

	for (const FString& QToken : QueryTokens)
	{
		// (a) exact matches
		TArray<int32>* ExactEntries = GInvertedIndex.Find(QToken);
		TSet<int32> TokenMatchedEntries; // entries this token already credited

		if (ExactEntries)
		{
			for (int32 Idx : *ExactEntries)
			{
				if (!TokenMatchedEntries.Contains(Idx))
				{
					++ExactHits[Idx];
					TokenMatchedEntries.Add(Idx);
				}
			}
		}

		// (b) prefix matches: any index token that StartsWith(QToken) and != QToken
		for (const auto& Pair : GInvertedIndex)
		{
			const FString& IndexTok = Pair.Key;
			if (IndexTok.Len() <= QToken.Len())
			{
				continue; // strict prefix requires IndexTok longer than QToken, and StartsWith already excludes equality when combined with !=.
			}
			if (!IndexTok.StartsWith(QToken, ESearchCase::CaseSensitive))
			{
				continue;
			}
			for (int32 Idx : Pair.Value)
			{
				if (!TokenMatchedEntries.Contains(Idx))
				{
					++PrefixHits[Idx];
					TokenMatchedEntries.Add(Idx);
				}
			}
		}

		// (c) fuzzy Levenshtein pass only when neither exact nor prefix produced credit.
		if (TokenMatchedEntries.Num() == 0)
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
					const int32 Dist = LevenshteinBounded(QToken, IndexTok, 2);
					if (Dist <= 2)
					{
						for (int32 Idx : Pair.Value)
						{
							if (!TokenMatchedEntries.Contains(Idx))
							{
								++FuzzyHits[Idx];
								TokenMatchedEntries.Add(Idx);
							}
						}
					}
				}
			}
		}
	}

	const float InvDenom = 1.0f / static_cast<float>(FMath::Max(1, QueryTokens.Num()));

	struct FScoredEntry
	{
		int32  EntryIdx;
		float  Score;
	};
	TArray<FScoredEntry> Scored;
	Scored.Reserve(GCatalogEntries.Num());

	for (int32 i = 0; i < GCatalogEntries.Num(); ++i)
	{
		const int32 E = ExactHits[i];
		const int32 P = PrefixHits[i];
		const int32 F = FuzzyHits[i];
		if (E == 0 && P == 0 && F == 0)
		{
			continue;
		}
		const float Raw = (static_cast<float>(E) * 2.0f)
		                + (static_cast<float>(P) * 1.0f)
		                + (static_cast<float>(F) * 0.5f);
		Scored.Add({ i, Raw * InvDenom });
	}

	if (Scored.Num() == 0)
	{
		return Out;
	}

	Scored.Sort([](const FScoredEntry& A, const FScoredEntry& B)
	{
		if (A.Score != B.Score)
		{
			return A.Score > B.Score;
		}
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
