// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonWordPieceTokenizer.h"

#include "ClaireonLog.h"
#include "Misc/FileHelper.h"

// ---------------------------------------------------------------------------
// File-local helpers -- discriminator prefix Cl005Tok_ guards against unity-
// batch anon-namespace symbol collisions (see feedback_anon_namespace_unity_collision).
//
// All helpers in this namespace are called only from within this translation
// unit. No UNTEST macros; tests live in ClaireonWordPieceTokenizerTests.cpp.
// ---------------------------------------------------------------------------
namespace Cl005Tok_Private
{

// ---------------------------------------------------------------------------
// BERT max_input_chars_per_word (HuggingFace default, not configurable here).
// Tokens longer than this are replaced by [UNK] without attempting WordPiece.
// ---------------------------------------------------------------------------
inline constexpr int32 GMaxInputCharsPerWord = 100;

// ---------------------------------------------------------------------------
// Cl005Tok_IsControl
//
// Returns true for characters treated as "invalid" or control characters that
// BERT's BasicTokenizer._clean_text() removes. Keeps \t (0x09), \n (0x0A),
// \r (0x0D) as whitespace (they are subsequently normalized to spaces).
// Matches huggingface/tokenizers BasicTokenizer behaviour exactly.
// ---------------------------------------------------------------------------
static bool Cl005Tok_IsControl(TCHAR Ch)
{
	// HuggingFace _is_control: skip whitespace class (handled separately), then
	// treat 0x0000, 0xFFFD, and any cat=Cc/Cf char as control. UE does not expose
	// a full Unicode category query in the public API. We handle the ASCII range
	// exactly and use a conservative test for high code points.
	if (Ch == TEXT('\t') || Ch == TEXT('\n') || Ch == TEXT('\r'))
	{
		return false; // kept as whitespace
	}
	// Null byte and Unicode replacement character.
	if (Ch == TEXT('\0') || Ch == TEXT('\xFFFD'))
	{
		return true;
	}
	// ASCII C0 controls (0x00-0x1F) and DEL (0x7F).
	if ((uint32)Ch <= 0x001F || (uint32)Ch == 0x007F)
	{
		return true;
	}
	// C1 controls U+0080..U+009F.
	if ((uint32)Ch >= 0x0080 && (uint32)Ch <= 0x009F)
	{
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Cl005Tok_IsWhitespace
//
// Matches HuggingFace BasicTokenizer._is_whitespace: space, tab, newline,
// carriage return, and any Unicode char with category Zs (space separator).
// For ASCII Zs we only have 0x20; the next Zs is U+00A0 (non-breaking space).
// Tool docs are ASCII, but we handle the common high-codepoint Zs chars too.
// ---------------------------------------------------------------------------
static bool Cl005Tok_IsWhitespace(TCHAR Ch)
{
	if (Ch == TEXT(' ')  || Ch == TEXT('\t') ||
		Ch == TEXT('\n') || Ch == TEXT('\r'))
	{
		return true;
	}
	// Unicode Zs separators that commonly appear in pasted text.
	const uint32 C = (uint32)Ch;
	if (C == 0x00A0 || // NO-BREAK SPACE
		C == 0x1680 || // OGHAM SPACE MARK
		(C >= 0x2000 && C <= 0x200A) || // EN QUAD .. HAIR SPACE
		C == 0x202F || // NARROW NO-BREAK SPACE
		C == 0x205F || // MEDIUM MATHEMATICAL SPACE
		C == 0x3000)   // IDEOGRAPHIC SPACE
	{
		return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Cl005Tok_IsPunct
//
// Returns true for ASCII punctuation and for common Unicode punctuation/symbol
// code points that BERT splits on. HuggingFace's _is_punctuation covers:
//   - ASCII !"#$%&'()*+,-./:;<=>?@[\]^_`{|}~  (0x21-0x2F, 0x3A-0x40,
//     0x5B-0x60, 0x7B-0x7E)
//   - Any Unicode char in category P* or S* (punctuation / symbol).
//
// Since UE5's public API does not directly expose Unicode category queries,
// we handle ASCII punctuation exactly and use a heuristic for high codepoints
// (anything in the common punctuation/symbol Unicode blocks). Tool docs are
// almost entirely ASCII, so this simplification does not affect embeddings in
// practice.
// ---------------------------------------------------------------------------
static bool Cl005Tok_IsPunct(TCHAR Ch)
{
	const uint32 C = (uint32)Ch;
	// ASCII punctuation ranges (exactly what HuggingFace's _is_punctuation uses).
	if ((C >= 0x0021 && C <= 0x002F) || // !"#$%&'()*+,-./
		(C >= 0x003A && C <= 0x0040) || // :;<=>?@
		(C >= 0x005B && C <= 0x0060) || // [\]^_`
		(C >= 0x007B && C <= 0x007E))   // {|}~
	{
		return true;
	}
	// Common Unicode punctuation/symbol blocks (best-effort; covers Latin Extended,
	// General Punctuation, CJK punctuation, currency symbols, etc.).
	// This is conservative relative to full Unicode P*/S* but covers the cases that
	// matter for tool doc text.
	if (C >= 0x00A1 && C <= 0x00BF) return true; // Latin-1 punct/symbols
	if (C >= 0x2010 && C <= 0x2060) return true; // General Punctuation block
	if (C >= 0x2100 && C <= 0x214F) return true; // Letterlike Symbols
	if (C >= 0x2190 && C <= 0x21FF) return true; // Arrows
	if (C >= 0x2200 && C <= 0x22FF) return true; // Mathematical Operators
	if (C >= 0x3000 && C <= 0x303F) return true; // CJK Symbols and Punctuation
	if (C >= 0xFE50 && C <= 0xFE6F) return true; // Small Form Variants
	if (C >= 0xFF01 && C <= 0xFF0F) return true; // Fullwidth ASCII punctuation
	if (C >= 0xFF1A && C <= 0xFF20) return true;
	if (C >= 0xFF3B && C <= 0xFF40) return true;
	if (C >= 0xFF5B && C <= 0xFF65) return true;
	return false;
}

// ---------------------------------------------------------------------------
// Cl005Tok_IsNonspacingMark
//
// Returns true for Unicode Mn (nonspacing mark) code points, used during
// accent stripping after NFD-like decomposition.
//
// The combining diacritical marks block is U+0300..U+036F. We also cover
// U+1AB0..U+1AFF (Combining Diacritical Marks Extended) and
// U+1DC0..U+1DFF (Combining Diacritical Marks Supplement) which NFD can
// produce from Latin Extended characters. This is the typical set encountered
// when lowercasing and stripping accents from Western European text.
//
// NOTE: UE5 does not expose a public FText/ICU NFD decomposition API that
// we can call to convert e.g. 'e' + combining acute into two codepoints before
// calling this function. We therefore perform accent-stripping on already-
// decomposed codepoints only. For fully pre-composed input (most English tool
// docs) the combining marks block is never visited and the behaviour is
// identical to HuggingFace. For pre-composed non-ASCII letters (e.g. 'e' U+00E9)
// we lowercase but do NOT strip the accent because we cannot decompose.
// Reference fixtures using pure ASCII will pass; accented-letter fixtures would
// need UE's ICU layer or a manual decomposition table.
// ---------------------------------------------------------------------------
static bool Cl005Tok_IsNonspacingMark(uint32 Codepoint)
{
	return (Codepoint >= 0x0300 && Codepoint <= 0x036F) ||
		   (Codepoint >= 0x1AB0 && Codepoint <= 0x1AFF) ||
		   (Codepoint >= 0x1DC0 && Codepoint <= 0x1DFF);
}

// ---------------------------------------------------------------------------
// Cl005Tok_IsCjkChar
//
// Returns true for codepoints in CJK Unified Ideographs and related blocks.
// HuggingFace BasicTokenizer adds spaces around CJK chars before whitespace-
// splitting. Tool docs are ASCII, so this path is a documented simplification:
// we implement the check but it will not trigger for typical tool doc input.
// ---------------------------------------------------------------------------
static bool Cl005Tok_IsCjkChar(uint32 C)
{
	return (C >= 0x4E00  && C <= 0x9FFF)  || // CJK Unified Ideographs
		   (C >= 0x3400  && C <= 0x4DBF)  || // CJK Extension A
		   (C >= 0x20000 && C <= 0x2A6DF) || // CJK Extension B
		   (C >= 0x2A700 && C <= 0x2B73F) || // CJK Extension C
		   (C >= 0x2B740 && C <= 0x2B81F) || // CJK Extension D
		   (C >= 0x2B820 && C <= 0x2CEAF) || // CJK Extension E
		   (C >= 0xF900  && C <= 0xFAFF)  || // CJK Compatibility Ideographs
		   (C >= 0x2F800 && C <= 0x2FA1F);   // CJK Compatibility Supplement
}

// ---------------------------------------------------------------------------
// Cl005Tok_CleanAndTokenizeBasic
//
// Implements BERT BasicTokenizer (do_lower_case=true):
//   1. Clean: strip control chars, normalize whitespace.
//   2. Add spaces around CJK chars.
//   3. Lowercase.
//   4. Strip nonspacing-mark code points (accent stripping; see caveat above).
//   5. Whitespace-split.
//   6. Split each token on punctuation into standalone punct chars.
//
// Returns a list of basic tokens ready for WordPiece.
// ---------------------------------------------------------------------------
static TArray<FString> Cl005Tok_CleanAndTokenizeBasic(const FString& Text)
{
	// --- Phase 1: clean + CJK spacing (produce intermediate string) ---

	FString Cleaned;
	Cleaned.Reserve(Text.Len() + 64);

	for (int32 i = 0; i < Text.Len(); ++i)
	{
		const TCHAR Ch = Text[i];
		const uint32 C = (uint32)Ch;

		// Drop control chars.
		if (Cl005Tok_IsControl(Ch))
		{
			continue;
		}
		// Whitespace -> single space.
		if (Cl005Tok_IsWhitespace(Ch))
		{
			Cleaned.AppendChar(TEXT(' '));
			continue;
		}
		// CJK: add surrounding spaces (simplification documented in header).
		if (Cl005Tok_IsCjkChar(C))
		{
			Cleaned.AppendChar(TEXT(' '));
			Cleaned.AppendChar(Ch);
			Cleaned.AppendChar(TEXT(' '));
			continue;
		}
		Cleaned.AppendChar(Ch);
	}

	// --- Step 2: lowercase ---
	Cleaned.ToLowerInline();

	// --- Phase 3: strip combining/accent code points (nonspacing marks) ---
	// We iterate the lowercased string and drop any character whose codepoint
	// falls in a nonspacing-mark range. For pre-composed characters (e.g. U+00E9 'e')
	// no stripping occurs because they are not decomposed (see header caveat).
	{
		FString Stripped;
		Stripped.Reserve(Cleaned.Len());
		for (int32 i = 0; i < Cleaned.Len(); ++i)
		{
			const uint32 C = (uint32)Cleaned[i];
			if (!Cl005Tok_IsNonspacingMark(C))
			{
				Stripped.AppendChar(Cleaned[i]);
			}
		}
		Cleaned = MoveTemp(Stripped);
	}

	// --- Phase 4: whitespace-split into raw tokens ---
	TArray<FString> RawTokens;
	Cleaned.ParseIntoArray(RawTokens, TEXT(" "), /*CullEmpty=*/true);

	// --- Phase 5: split each raw token on punctuation ---
	// Each punctuation character becomes a standalone token element.
	// This replicates HuggingFace BasicTokenizer._run_split_on_punc().
	TArray<FString> BasicTokens;
	BasicTokens.Reserve(RawTokens.Num() * 2);

	for (const FString& RawTok : RawTokens)
	{
		FString Current;
		for (int32 i = 0; i < RawTok.Len(); ++i)
		{
			const TCHAR Ch = RawTok[i];
			if (Cl005Tok_IsPunct(Ch))
			{
				// Flush any accumulated non-punct chars as their own token.
				if (!Current.IsEmpty())
				{
					BasicTokens.Add(MoveTemp(Current));
					Current.Reset();
				}
				// Punctuation char is its own token.
				BasicTokens.Add(FString(1, &Ch));
			}
			else
			{
				Current.AppendChar(Ch);
			}
		}
		if (!Current.IsEmpty())
		{
			BasicTokens.Add(MoveTemp(Current));
		}
	}

	return BasicTokens;
}

// ---------------------------------------------------------------------------
// Cl005Tok_WordPiece
//
// Implements HuggingFace WordpieceTokenizer.tokenize() exactly:
//   - Token length > GMaxInputCharsPerWord -> emit [UNK].
//   - Greedy longest-match-first from left; continuation pieces prefixed "##".
//   - If any position finds no match the whole token is [UNK] (is_bad).
//
// OutPieces receives the piece ids for this token (appended, not reset).
// Returns false when the token maps to [UNK].
// ---------------------------------------------------------------------------
static bool Cl005Tok_WordPiece(
	const FString&            Token,
	const TMap<FString,int32>& VocabMap,
	int32                      UnkId,
	TArray<int32>&             OutPieces)
{
	if (Token.Len() > GMaxInputCharsPerWord)
	{
		OutPieces.Add(UnkId);
		return false;
	}

	TArray<int32> Pieces;
	Pieces.Reserve(Token.Len());

	bool bIsBad = false;
	int32 Start = 0;
	const int32 TokLen = Token.Len();

	while (Start < TokLen)
	{
		// Find longest substring [Start, End) present in vocab.
		// For Start==0 the key is the substring itself; for Start>0 prepend "##".
		int32 End = TokLen;
		FString CurSubstr;
		bool bFound = false;

		while (End > Start)
		{
			const FString Sub = Token.Mid(Start, End - Start);
			const FString Key = (Start == 0) ? Sub : (TEXT("##") + Sub);
			if (VocabMap.Contains(Key))
			{
				CurSubstr = Key;
				bFound = true;
				break;
			}
			--End;
		}

		if (!bFound)
		{
			bIsBad = true;
			break;
		}

		Pieces.Add(VocabMap[CurSubstr]);
		Start = End;
	}

	if (bIsBad)
	{
		OutPieces.Add(UnkId);
		return false;
	}

	OutPieces.Append(Pieces);
	return true;
}

} // namespace Cl005Tok_Private

// ---------------------------------------------------------------------------
// FClaireonWordPieceTokenizer
// ---------------------------------------------------------------------------

bool FClaireonWordPieceTokenizer::LoadVocab(const FString& VocabPath)
{
	bReady = false;
	VocabMap.Empty();

	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *VocabPath))
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[WordPieceTokenizer] Failed to read vocab file '%s'."), *VocabPath);
		return false;
	}

	if (Lines.IsEmpty())
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[WordPieceTokenizer] Vocab file '%s' is empty."), *VocabPath);
		return false;
	}

	VocabMap.Reserve(Lines.Num());
	for (int32 Idx = 0; Idx < Lines.Num(); ++Idx)
	{
		// Lines may have trailing \r on Windows; trim whitespace at the end.
		FString Token = Lines[Idx].TrimEnd();
		VocabMap.Add(MoveTemp(Token), Idx);
	}

	// Verify that the four canonical BERT special tokens are at the expected ids.
	// [PAD]=0, [UNK]=100, [CLS]=101, [SEP]=102 (bert-base-uncased convention).
	auto CheckSpecial = [&](const TCHAR* Name, int32 ExpectedId) -> bool
	{
		const int32* Found = VocabMap.Find(Name);
		if (!Found)
		{
			UE_LOG(LogClaireon, Warning,
				TEXT("[WordPieceTokenizer] Special token '%s' missing from vocab '%s'."),
				Name, *VocabPath);
			return false;
		}
		if (*Found != ExpectedId)
		{
			UE_LOG(LogClaireon, Warning,
				TEXT("[WordPieceTokenizer] Special token '%s' has id %d, expected %d "
				     "(vocab file may not be bert-base-uncased)."),
				Name, *Found, ExpectedId);
			return false;
		}
		return true;
	};

	if (!CheckSpecial(TEXT("[PAD]"),  0)   ||
		!CheckSpecial(TEXT("[UNK]"),  100) ||
		!CheckSpecial(TEXT("[CLS]"),  101) ||
		!CheckSpecial(TEXT("[SEP]"),  102))
	{
		VocabMap.Empty();
		return false;
	}

	// Cache the special ids (constants for bert-base-uncased, but kept as members
	// so Encode() never re-queries the map for hot-path lookups).
	IdPad = 0;
	IdUnk = 100;
	IdCls = 101;
	IdSep = 102;

	bReady = true;

	UE_LOG(LogClaireon, Log,
		TEXT("[WordPieceTokenizer] Loaded '%s' (%d tokens)."), *VocabPath, VocabMap.Num());
	return true;
}

bool FClaireonWordPieceTokenizer::IsReady() const
{
	return bReady;
}

void FClaireonWordPieceTokenizer::Encode(
	const FString& Text, int32 MaxLen,
	TArray<int32>& OutIds, TArray<int32>& OutMask) const
{
	OutIds.Reset();
	OutMask.Reset();

	if (!bReady || MaxLen <= 0)
	{
		return;
	}

	// --- Step 1: BasicTokenizer ---
	TArray<FString> BasicTokens = Cl005Tok_Private::Cl005Tok_CleanAndTokenizeBasic(Text);

	// --- Step 2: WordPiece per basic token ---
	// Collect all piece ids (before adding [CLS]/[SEP]).
	TArray<int32> PieceIds;
	PieceIds.Reserve(BasicTokens.Num() * 3);

	for (const FString& BT : BasicTokens)
	{
		Cl005Tok_Private::Cl005Tok_WordPiece(BT, VocabMap, IdUnk, PieceIds);
	}

	// --- Step 3: Assemble [CLS] + pieces + [SEP], truncate, pad ---
	// The sequence budget: MaxLen slots total, 2 taken by [CLS] and [SEP].
	// If MaxLen < 2 there is no room for any real content; emit just [CLS]/[SEP]
	// when MaxLen == 2, or nothing for MaxLen < 2.
	const int32 MaxPieces = FMath::Max(0, MaxLen - 2); // slots for content
	if (PieceIds.Num() > MaxPieces)
	{
		PieceIds.SetNum(MaxPieces); // truncate (matching HuggingFace truncation_side='right')
	}

	const int32 RealTokenCount = 1 + PieceIds.Num() + 1; // [CLS] + pieces + [SEP]
	OutIds.Reserve(MaxLen);
	OutMask.Reserve(MaxLen);

	// [CLS]
	OutIds.Add(IdCls);
	OutMask.Add(1);

	// Piece ids
	for (int32 Id : PieceIds)
	{
		OutIds.Add(Id);
		OutMask.Add(1);
	}

	// [SEP]
	if (MaxLen >= 2)
	{
		OutIds.Add(IdSep);
		OutMask.Add(1);
	}

	// Pad to MaxLen
	while (OutIds.Num() < MaxLen)
	{
		OutIds.Add(IdPad);
		OutMask.Add(0);
	}

	check(OutIds.Num() == MaxLen);
	check(OutMask.Num() == MaxLen);
}
