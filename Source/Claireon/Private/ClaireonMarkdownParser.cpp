// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonMarkdownParser.h"

#include "GenericPlatform/GenericPlatformHttp.h"
#include "Internationalization/Regex.h"

// FWidgetDecorator creates inline-level runs in SRichTextBlock's text layout.
// Code blocks need block-level behavior. The approach is:
// 1. Place each code block on its own line with surrounding newlines
// 2. Set FWidgetRunInfo::Size explicitly for full-width rendering
// 3. If this approach fails, fallback: hybrid SVerticalBox of alternating
//    SRichTextBlock (prose) and SBorder/SMultiLineEditableText (code blocks)

FString FClaireonMarkdownParser::EscapeXml(const FString& InText)
{
	FString Result = InText;
	// & must be first to avoid double-escaping
	Result.ReplaceInline(TEXT("&"), TEXT("&amp;"));
	Result.ReplaceInline(TEXT("<"), TEXT("&lt;"));
	Result.ReplaceInline(TEXT(">"), TEXT("&gt;"));
	return Result;
}

FString FClaireonMarkdownParser::ConvertInlineMarkdown(const FString& InLine)
{
	FString Result = InLine;

	// 1. Inline code (process first to prevent markdown inside code from being parsed)
	{
		const FRegexPattern Pattern(TEXT("`([^`]+)`"));
		FRegexMatcher Matcher(Pattern, Result);

		TArray<TPair<int32, int32>> Matches;
		TArray<FString> Captures;
		while (Matcher.FindNext())
		{
			Matches.Add(TPair<int32, int32>(Matcher.GetMatchBeginning(), Matcher.GetMatchEnding()));
			Captures.Add(Matcher.GetCaptureGroup(1));
		}

		for (int32 i = Matches.Num() - 1; i >= 0; --i)
		{
			const FString Replacement =
				FString::Printf(TEXT("<RichText.Code>%s</>"), *Captures[i]);
			Result = Result.Left(Matches[i].Key) + Replacement + Result.Mid(Matches[i].Value);
		}
	}

	// 2. Bold italic (***text***)
	{
		const FRegexPattern Pattern(TEXT("\\*\\*\\*([^*]+)\\*\\*\\*"));
		FRegexMatcher Matcher(Pattern, Result);

		TArray<TPair<int32, int32>> Matches;
		TArray<FString> Captures;
		while (Matcher.FindNext())
		{
			Matches.Add(TPair<int32, int32>(Matcher.GetMatchBeginning(), Matcher.GetMatchEnding()));
			Captures.Add(Matcher.GetCaptureGroup(1));
		}

		for (int32 i = Matches.Num() - 1; i >= 0; --i)
		{
			const FString Replacement =
				FString::Printf(TEXT("<RichText.BoldItalic>%s</>"), *Captures[i]);
			Result = Result.Left(Matches[i].Key) + Replacement + Result.Mid(Matches[i].Value);
		}
	}

	// 3. Bold (**text**)
	{
		const FRegexPattern Pattern(TEXT("\\*\\*([^*]+)\\*\\*"));
		FRegexMatcher Matcher(Pattern, Result);

		TArray<TPair<int32, int32>> Matches;
		TArray<FString> Captures;
		while (Matcher.FindNext())
		{
			Matches.Add(TPair<int32, int32>(Matcher.GetMatchBeginning(), Matcher.GetMatchEnding()));
			Captures.Add(Matcher.GetCaptureGroup(1));
		}

		for (int32 i = Matches.Num() - 1; i >= 0; --i)
		{
			const FString Replacement =
				FString::Printf(TEXT("<RichText.Bold>%s</>"), *Captures[i]);
			Result = Result.Left(Matches[i].Key) + Replacement + Result.Mid(Matches[i].Value);
		}
	}

	// 4. Italic (*text*) — single asterisks, not mid-word
	{
		const FRegexPattern Pattern(TEXT("(?<![\\w*])\\*([^*]+)\\*(?![\\w*])"));
		FRegexMatcher Matcher(Pattern, Result);

		TArray<TPair<int32, int32>> Matches;
		TArray<FString> Captures;
		while (Matcher.FindNext())
		{
			Matches.Add(TPair<int32, int32>(Matcher.GetMatchBeginning(), Matcher.GetMatchEnding()));
			Captures.Add(Matcher.GetCaptureGroup(1));
		}

		for (int32 i = Matches.Num() - 1; i >= 0; --i)
		{
			const FString Replacement =
				FString::Printf(TEXT("<RichText.Italic>%s</>"), *Captures[i]);
			Result = Result.Left(Matches[i].Key) + Replacement + Result.Mid(Matches[i].Value);
		}
	}

	return Result;
}

FString FClaireonMarkdownParser::ConvertToRichText(const FString& InMarkdown)
{
	if (InMarkdown.IsEmpty())
	{
		return FString();
	}

	enum class EState
	{
		Normal,
		InCodeBlock,
		InTable
	};

	EState State = EState::Normal;
	FString CodeBlockBuffer;
	FString CodeBlockLanguage;
	TArray<FString> TableLines;

	TArray<FString> Lines;
	InMarkdown.ParseIntoArrayLines(Lines);

	FString Output;

	auto FlushTable = [&]()
	{
		if (TableLines.Num() > 0)
		{
			Output += ConvertTable(TableLines);
			TableLines.Empty();
		}
	};

	for (int32 LineIdx = 0; LineIdx < Lines.Num(); ++LineIdx)
	{
		const FString& Line = Lines[LineIdx];

		switch (State)
		{
			case EState::InCodeBlock:
			{
				if (Line.TrimStart().StartsWith(TEXT("```")))
				{
					Output += ConvertCodeBlock(CodeBlockBuffer, CodeBlockLanguage);
					CodeBlockBuffer.Empty();
					CodeBlockLanguage.Empty();
					State = EState::Normal;
				}
				else
				{
					if (!CodeBlockBuffer.IsEmpty())
					{
						CodeBlockBuffer += TEXT("\n");
					}
					CodeBlockBuffer += Line;
				}
				break;
			}

			case EState::InTable:
			{
				if (Line.TrimStart().StartsWith(TEXT("|")))
				{
					TableLines.Add(Line);
				}
				else
				{
					FlushTable();
					State = EState::Normal;
					// Fall through to process current line as Normal
					goto ProcessNormal;
				}
				break;
			}

			case EState::Normal:
			{
			ProcessNormal:
				// Fenced code block start
				if (Line.TrimStart().StartsWith(TEXT("```")))
				{
					// Extract language after ```
					FString Trimmed = Line.TrimStart();
					CodeBlockLanguage = Trimmed.Mid(3).TrimStartAndEnd();
					State = EState::InCodeBlock;
				}
				// Table row
				else if (Line.TrimStart().StartsWith(TEXT("|")))
				{
					State = EState::InTable;
					TableLines.Add(Line);
				}
				// Header (# H1, ## H2, ### H3)
				else if (Line.StartsWith(TEXT("### ")))
				{
					FString Text = Line.Mid(4).TrimStartAndEnd();
					// Strip bold markers since headers are already bold
					Text.ReplaceInline(TEXT("**"), TEXT(""));
					if (!Output.IsEmpty())
					{
						Output += TEXT("\n\n");
					}
					Output += FString::Printf(
						TEXT("<RichText.Header3>%s</>"), *EscapeXml(Text));
				}
				else if (Line.StartsWith(TEXT("## ")))
				{
					FString Text = Line.Mid(3).TrimStartAndEnd();
					Text.ReplaceInline(TEXT("**"), TEXT(""));
					if (!Output.IsEmpty())
					{
						Output += TEXT("\n\n");
					}
					Output += FString::Printf(
						TEXT("<RichText.Header2>%s</>"), *EscapeXml(Text));
				}
				else if (Line.StartsWith(TEXT("# ")))
				{
					FString Text = Line.Mid(2).TrimStartAndEnd();
					Text.ReplaceInline(TEXT("**"), TEXT(""));
					if (!Output.IsEmpty())
					{
						Output += TEXT("\n\n");
					}
					Output += FString::Printf(
						TEXT("<RichText.Header1>%s</>"), *EscapeXml(Text));
				}
				// Horizontal rule
				else if (Line.TrimStartAndEnd() == TEXT("---") || Line.TrimStartAndEnd() == TEXT("***") || Line.TrimStartAndEnd() == TEXT("___"))
				{
					if (!Output.IsEmpty())
					{
						Output += TEXT("\n");
					}
					// Emit a blank separator line
					Output += TEXT("\n");
				}
				// Bullet list
				else if (Line.Len() >= 2 && (Line.StartsWith(TEXT("- ")) || Line.StartsWith(TEXT("* "))))
				{
					FString Text = Line.Mid(2).TrimStartAndEnd();
					FString Processed = WrapAssetPaths(ConvertInlineMarkdown(EscapeXml(Text)));
					if (!Output.IsEmpty())
					{
						Output += TEXT("\n");
					}
					Output += FString::Printf(TEXT("  \u2022 %s"), *Processed);
				}
				// Numbered list
				else if (FRegexMatcher(FRegexPattern(TEXT("^\\d+\\. ")), Line).FindNext())
				{
					// Find the ". " separator
					int32 DotPos = INDEX_NONE;
					Line.FindChar(TEXT('.'), DotPos);
					FString Number = Line.Left(DotPos);
					FString Text = Line.Mid(DotPos + 2).TrimStartAndEnd();
					FString Processed = WrapAssetPaths(ConvertInlineMarkdown(EscapeXml(Text)));
					if (!Output.IsEmpty())
					{
						Output += TEXT("\n");
					}
					Output += FString::Printf(TEXT("  %s. %s"), *Number, *Processed);
				}
				// Blockquote
				else if (Line.StartsWith(TEXT("> ")))
				{
					FString Text = Line.Mid(2).TrimStartAndEnd();
					FString Processed = WrapAssetPaths(ConvertInlineMarkdown(EscapeXml(Text)));
					if (!Output.IsEmpty())
					{
						Output += TEXT("\n");
					}
					Output += FString::Printf(TEXT("    %s"), *Processed);
				}
				// Empty line (paragraph break)
				else if (Line.TrimStartAndEnd().IsEmpty())
				{
					if (!Output.IsEmpty())
					{
						Output += TEXT("\n\n");
					}
				}
				// Normal text
				else
				{
					FString Escaped = EscapeXml(Line);
					FString Processed = ConvertInlineMarkdown(Escaped);
					// Apply asset path wrapping on normal prose lines
					Processed = WrapAssetPaths(Processed);
					if (!Output.IsEmpty())
					{
						Output += TEXT("\n");
					}
					Output += Processed;
				}
				break;
			}
		}
	}

	// Flush any open state at end of input
	if (State == EState::InCodeBlock && !CodeBlockBuffer.IsEmpty())
	{
		Output += ConvertCodeBlock(CodeBlockBuffer, CodeBlockLanguage);
	}
	else if (State == EState::InTable)
	{
		FlushTable();
	}

	return Output;
}

FString FClaireonMarkdownParser::WrapAssetPaths(const FString& InText)
{
	// Regex: match paths starting with known Unreal mount points
	const FRegexPattern Pattern(
		TEXT("/(Game|Script|Engine|Plugins)/[A-Za-z0-9_/.]+"));

	FRegexMatcher Matcher(Pattern, InText);

	// Collect matches in reverse order to preserve string offsets during replacement
	TArray<TPair<int32, int32>> Matches;
	while (Matcher.FindNext())
	{
		Matches.Add(TPair<int32, int32>(Matcher.GetMatchBeginning(), Matcher.GetMatchEnding()));
	}

	FString Result = InText;
	for (int32 i = Matches.Num() - 1; i >= 0; --i)
	{
		const int32 Begin = Matches[i].Key;
		const int32 End = Matches[i].Value;
		const FString AssetPath = InText.Mid(Begin, End - Begin);

		const FString Replacement = FString::Printf(
			TEXT("<a id=\"asset\" style=\"RichText.AssetLink\" href=\"%s\">%s</>"), *AssetPath, *AssetPath);

		Result = Result.Left(Begin) + Replacement + Result.Mid(End);
	}

	return Result;
}

bool FClaireonMarkdownParser::ValidateMarkup(const FString& InMarkup)
{
	// Count opening tags and closing tags (</>)
	// Opening: <TagName> or <TagName attr="...">
	// Closing: </>
	int32 OpenCount = 0;
	int32 CloseCount = 0;

	// Match opening tags but exclude self-closing tags (e.g. <img ... />)
	const FRegexPattern OpenPattern(TEXT("<[A-Za-z][A-Za-z0-9.]*[^>]*[^/]>"));
	FRegexMatcher OpenMatcher(OpenPattern, InMarkup);
	while (OpenMatcher.FindNext())
	{
		++OpenCount;
	}

	const FRegexPattern ClosePattern(TEXT("</>"));
	FRegexMatcher CloseMatcher(ClosePattern, InMarkup);
	while (CloseMatcher.FindNext())
	{
		++CloseCount;
	}

	return OpenCount == CloseCount;
}

FString FClaireonMarkdownParser::ConvertCodeBlock(const FString& InCode, const FString& InLanguage)
{
	// URL-encode the code content so it survives XML attribute parsing
	FString EncodedCode = FGenericPlatformHttp::UrlEncode(InCode);
	FString EncodedLang = FGenericPlatformHttp::UrlEncode(InLanguage);

	// FWidgetDecorator matches on the tag name, so use <codeblock> not <img>
	// Content placeholder is required for the text run to be created
	return FString::Printf(
		TEXT("\n<codeblock lang=\"%s\" src=\"%s\"> </>\n"), *EncodedLang, *EncodedCode);
}

FString FClaireonMarkdownParser::ConvertTable(const TArray<FString>& InTableLines)
{
	// Tables are rendered as monospace text for column alignment
	FString Result;
	for (const FString& Line : InTableLines)
	{
		// Skip separator rows (|---|---|)
		if (Line.Contains(TEXT("---")))
		{
			continue;
		}
		if (!Result.IsEmpty())
		{
			Result += TEXT("\n");
		}
		// Process inline markdown within cells, then wrap in monospace style
		FString Processed = ConvertInlineMarkdown(EscapeXml(Line));
		Result += FString::Printf(TEXT("<RichText.Code>%s</>"), *Processed);
	}
	return Result;
}

// ---------------------------------------------------------------------------
// Block parser — structured output (no XML markup)
// ---------------------------------------------------------------------------

FClaireonStyledLine FClaireonMarkdownParser::ParseInlineSegments(
	const FString& InLine, const FString& InDefaultStyle)
{
	FClaireonStyledLine Result;

	if (InLine.IsEmpty())
	{
		return Result;
	}

	// A format region in the source text
	struct FFormatRegion
	{
		int32 OrigStart;
		int32 OrigEnd;
		int32 InnerStart;
		int32 InnerEnd;
		FString StyleName;
	};

	TArray<FFormatRegion> Regions;
	TSet<int32> ClaimedPositions;

	auto IsOverlapping = [&ClaimedPositions](int32 Start, int32 End) -> bool
	{
		for (int32 i = Start; i < End; ++i)
		{
			if (ClaimedPositions.Contains(i))
			{
				return true;
			}
		}
		return false;
	};

	auto ClaimRange = [&ClaimedPositions](int32 Start, int32 End)
	{
		for (int32 i = Start; i < End; ++i)
		{
			ClaimedPositions.Add(i);
		}
	};

	// 1a. Double-backtick inline code (``code``) — handle first
	{
		const FRegexPattern Pattern(TEXT("``([^`]+)``"));
		FRegexMatcher Matcher(Pattern, InLine);
		while (Matcher.FindNext())
		{
			const int32 MatchStart = Matcher.GetMatchBeginning();
			const int32 MatchEnd = Matcher.GetMatchEnding();
			if (!IsOverlapping(MatchStart, MatchEnd))
			{
				FFormatRegion Region;
				Region.OrigStart = MatchStart;
				Region.OrigEnd = MatchEnd;
				Region.InnerStart = MatchStart + 2;
				Region.InnerEnd = MatchEnd - 2;
				Region.StyleName = TEXT("RichText.Code");
				Regions.Add(Region);
				ClaimRange(MatchStart, MatchEnd);
			}
		}
	}

	// 1b. Single-backtick inline code (`code`)
	{
		const FRegexPattern Pattern(TEXT("`([^`]+)`"));
		FRegexMatcher Matcher(Pattern, InLine);
		while (Matcher.FindNext())
		{
			const int32 MatchStart = Matcher.GetMatchBeginning();
			const int32 MatchEnd = Matcher.GetMatchEnding();
			if (!IsOverlapping(MatchStart, MatchEnd))
			{
				FFormatRegion Region;
				Region.OrigStart = MatchStart;
				Region.OrigEnd = MatchEnd;
				Region.InnerStart = MatchStart + 1;
				Region.InnerEnd = MatchEnd - 1;
				Region.StyleName = TEXT("RichText.Code");
				Regions.Add(Region);
				ClaimRange(MatchStart, MatchEnd);
			}
		}
	}

	// 2. Bold italic (***text***)
	{
		const FRegexPattern Pattern(TEXT("\\*\\*\\*([^*]+)\\*\\*\\*"));
		FRegexMatcher Matcher(Pattern, InLine);
		while (Matcher.FindNext())
		{
			const int32 MatchStart = Matcher.GetMatchBeginning();
			const int32 MatchEnd = Matcher.GetMatchEnding();
			if (!IsOverlapping(MatchStart, MatchEnd))
			{
				FFormatRegion Region;
				Region.OrigStart = MatchStart;
				Region.OrigEnd = MatchEnd;
				Region.InnerStart = MatchStart + 3;
				Region.InnerEnd = MatchEnd - 3;
				Region.StyleName = TEXT("RichText.BoldItalic");
				Regions.Add(Region);
				ClaimRange(MatchStart, MatchEnd);
			}
		}
	}

	// 3. Bold (**text**)
	{
		const FRegexPattern Pattern(TEXT("\\*\\*([^*]+)\\*\\*"));
		FRegexMatcher Matcher(Pattern, InLine);
		while (Matcher.FindNext())
		{
			const int32 MatchStart = Matcher.GetMatchBeginning();
			const int32 MatchEnd = Matcher.GetMatchEnding();
			if (!IsOverlapping(MatchStart, MatchEnd))
			{
				FFormatRegion Region;
				Region.OrigStart = MatchStart;
				Region.OrigEnd = MatchEnd;
				Region.InnerStart = MatchStart + 2;
				Region.InnerEnd = MatchEnd - 2;
				Region.StyleName = TEXT("RichText.Bold");
				Regions.Add(Region);
				ClaimRange(MatchStart, MatchEnd);
			}
		}
	}

	// 4. Italic (*text*) — single asterisks, not mid-word
	{
		const FRegexPattern Pattern(TEXT("(?<![\\w*])\\*([^*]+)\\*(?![\\w*])"));
		FRegexMatcher Matcher(Pattern, InLine);
		while (Matcher.FindNext())
		{
			const int32 MatchStart = Matcher.GetMatchBeginning();
			const int32 MatchEnd = Matcher.GetMatchEnding();
			if (!IsOverlapping(MatchStart, MatchEnd))
			{
				FFormatRegion Region;
				Region.OrigStart = MatchStart;
				Region.OrigEnd = MatchEnd;
				Region.InnerStart = MatchStart + 1;
				Region.InnerEnd = MatchEnd - 1;
				Region.StyleName = TEXT("RichText.Italic");
				Regions.Add(Region);
				ClaimRange(MatchStart, MatchEnd);
			}
		}
	}

	// Sort regions by position in source text
	Regions.Sort([](const FFormatRegion& A, const FFormatRegion& B)
	{
		return A.OrigStart < B.OrigStart;
	});

	// Build plain text and segments by walking through source text
	FString PlainText;
	TArray<FClaireonTextSegment> Segments;

	int32 SrcCursor = 0;
	int32 DstCursor = 0;

	for (const FFormatRegion& Region : Regions)
	{
		// Copy unclaimed text before this region
		if (SrcCursor < Region.OrigStart)
		{
			const FString Before = InLine.Mid(SrcCursor, Region.OrigStart - SrcCursor);
			if (!Before.IsEmpty())
			{
				const int32 BeforeStart = DstCursor;
				PlainText += Before;
				DstCursor += Before.Len();

				FClaireonTextSegment Seg;
				Seg.StyleName = InDefaultStyle;
				Seg.StartIndex = BeforeStart;
				Seg.EndIndex = DstCursor;
				Segments.Add(Seg);
			}
		}

		// Copy inner text of the formatted region (markers stripped)
		const FString Inner = InLine.Mid(Region.InnerStart, Region.InnerEnd - Region.InnerStart);
		if (!Inner.IsEmpty())
		{
			const int32 InnerStart = DstCursor;
			PlainText += Inner;
			DstCursor += Inner.Len();

			FClaireonTextSegment Seg;
			Seg.StyleName = Region.StyleName;
			Seg.StartIndex = InnerStart;
			Seg.EndIndex = DstCursor;
			Segments.Add(Seg);
		}

		SrcCursor = Region.OrigEnd;
	}

	// Copy remaining text after the last region
	if (SrcCursor < InLine.Len())
	{
		const FString After = InLine.Mid(SrcCursor);
		if (!After.IsEmpty())
		{
			const int32 AfterStart = DstCursor;
			PlainText += After;
			DstCursor += After.Len();

			FClaireonTextSegment Seg;
			Seg.StyleName = InDefaultStyle;
			Seg.StartIndex = AfterStart;
			Seg.EndIndex = DstCursor;
			Segments.Add(Seg);
		}
	}

	// Post-process: detect asset paths in default-styled segments and split
	const FRegexPattern AssetPathPattern(
		TEXT("/(Game|Script|Engine|Plugins)/[A-Za-z0-9_/.]+"));

	TArray<FClaireonTextSegment> FinalSegments;
	for (const FClaireonTextSegment& Seg : Segments)
	{
		if (Seg.StyleName != InDefaultStyle)
		{
			FinalSegments.Add(Seg);
			continue;
		}

		// Check this segment's text for asset paths
		const FString SegText = PlainText.Mid(Seg.StartIndex, Seg.EndIndex - Seg.StartIndex);
		FRegexMatcher AssetMatcher(AssetPathPattern, SegText);

		TArray<TPair<int32, int32>> AssetMatches;
		while (AssetMatcher.FindNext())
		{
			AssetMatches.Add(TPair<int32, int32>(
				AssetMatcher.GetMatchBeginning(), AssetMatcher.GetMatchEnding()));
		}

		if (AssetMatches.Num() == 0)
		{
			FinalSegments.Add(Seg);
			continue;
		}

		// Split the segment around asset paths
		int32 LocalCursor = 0;
		for (const TPair<int32, int32>& Match : AssetMatches)
		{
			if (LocalCursor < Match.Key)
			{
				FClaireonTextSegment BeforeSeg;
				BeforeSeg.StyleName = InDefaultStyle;
				BeforeSeg.StartIndex = Seg.StartIndex + LocalCursor;
				BeforeSeg.EndIndex = Seg.StartIndex + Match.Key;
				FinalSegments.Add(BeforeSeg);
			}

			FClaireonTextSegment AssetSeg;
			AssetSeg.StyleName = TEXT("RichText.AssetLink");
			AssetSeg.StartIndex = Seg.StartIndex + Match.Key;
			AssetSeg.EndIndex = Seg.StartIndex + Match.Value;
			FinalSegments.Add(AssetSeg);

			LocalCursor = Match.Value;
		}

		if (LocalCursor < SegText.Len())
		{
			FClaireonTextSegment AfterSeg;
			AfterSeg.StyleName = InDefaultStyle;
			AfterSeg.StartIndex = Seg.StartIndex + LocalCursor;
			AfterSeg.EndIndex = Seg.EndIndex;
			FinalSegments.Add(AfterSeg);
		}
	}

	// Final cleanup: strip leftover markdown markers (**, *) from default-styled
	// segments. This handles edge cases like bold wrapping code: **`Foo`**
	// where the bold markers can't be claimed because the code region overlaps.
	FString CleanText;
	CleanText.Reserve(PlainText.Len());
	TArray<FClaireonTextSegment> CleanSegments;
	int32 CleanCursor = 0;

	for (const FClaireonTextSegment& Seg : FinalSegments)
	{
		const FString SegText = PlainText.Mid(Seg.StartIndex, Seg.EndIndex - Seg.StartIndex);

		if (Seg.StyleName == InDefaultStyle)
		{
			// Strip ** and * from default segments
			FString Cleaned = SegText;
			Cleaned.ReplaceInline(TEXT("**"), TEXT(""));
			Cleaned.ReplaceInline(TEXT("__"), TEXT(""));

			if (!Cleaned.IsEmpty())
			{
				FClaireonTextSegment CleanSeg;
				CleanSeg.StyleName = Seg.StyleName;
				CleanSeg.StartIndex = CleanCursor;
				CleanCursor += Cleaned.Len();
				CleanSeg.EndIndex = CleanCursor;
				CleanSegments.Add(CleanSeg);
				CleanText += Cleaned;
			}
		}
		else
		{
			FClaireonTextSegment CleanSeg;
			CleanSeg.StyleName = Seg.StyleName;
			CleanSeg.StartIndex = CleanCursor;
			CleanCursor += SegText.Len();
			CleanSeg.EndIndex = CleanCursor;
			CleanSegments.Add(CleanSeg);
			CleanText += SegText;
		}
	}

	Result.Text = CleanText;
	Result.Segments = MoveTemp(CleanSegments);
	return Result;
}

TArray<FClaireonMarkdownBlock> FClaireonMarkdownParser::ParseToBlocks(const FString& InMarkdown)
{
	TArray<FClaireonMarkdownBlock> Blocks;

	if (InMarkdown.IsEmpty())
	{
		return Blocks;
	}

	enum class EState
	{
		Normal,
		InCodeBlock,
		InTable
	};

	EState State = EState::Normal;
	FString CodeBlockBuffer;
	FString CodeBlockLanguage;
	TArray<FString> TableLines;
	int32 CurrentProseIndex = INDEX_NONE;

	auto FlushProse = [&]()
	{
		CurrentProseIndex = INDEX_NONE;
	};

	auto EnsureProse = [&]() -> FClaireonMarkdownBlock&
	{
		if (CurrentProseIndex == INDEX_NONE)
		{
			FClaireonMarkdownBlock Block;
			Block.Type = EClaireonBlockType::Prose;
			CurrentProseIndex = Blocks.Add(MoveTemp(Block));
		}
		return Blocks[CurrentProseIndex];
	};

	auto ParseTableRow = [](const FString& Line) -> TArray<FString>
	{
		TArray<FString> Cells;
		FString Trimmed = Line.TrimStartAndEnd();
		// Strip leading and trailing pipe
		if (Trimmed.StartsWith(TEXT("|")))
		{
			Trimmed.RemoveAt(0);
		}
		if (Trimmed.EndsWith(TEXT("|")))
		{
			Trimmed.RemoveAt(Trimmed.Len() - 1);
		}
		// Split on pipe
		Trimmed.ParseIntoArray(Cells, TEXT("|"));
		for (FString& Cell : Cells)
		{
			Cell.TrimStartAndEndInline();
		}
		return Cells;
	};

	auto IsSeparatorRow = [](const FString& Line) -> bool
	{
		// Separator rows contain only |, -, :, and whitespace
		for (const TCHAR Ch : Line)
		{
			if (Ch != TEXT('|') && Ch != TEXT('-') && Ch != TEXT(':') && Ch != TEXT(' ') && Ch != TEXT('\t'))
			{
				return false;
			}
		}
		return Line.Contains(TEXT("-"));
	};

	auto FlushTable = [&]()
	{
		if (TableLines.Num() > 0)
		{
			FClaireonMarkdownBlock Block;
			Block.Type = EClaireonBlockType::Table;

			bool bHeaderParsed = false;
			for (const FString& TableLine : TableLines)
			{
				if (IsSeparatorRow(TableLine))
				{
					continue;
				}
				TArray<FString> Cells = ParseTableRow(TableLine);
				if (!bHeaderParsed)
				{
					Block.TableHeaders = MoveTemp(Cells);
					bHeaderParsed = true;
				}
				else
				{
					Block.TableRows.Add(MoveTemp(Cells));
				}
			}
			Blocks.Add(MoveTemp(Block));
			TableLines.Empty();
		}
	};

	TArray<FString> Lines;
	InMarkdown.ParseIntoArrayLines(Lines);

	for (int32 LineIdx = 0; LineIdx < Lines.Num(); ++LineIdx)
	{
		const FString& Line = Lines[LineIdx];

		switch (State)
		{
			case EState::InCodeBlock:
			{
				if (Line.TrimStart().StartsWith(TEXT("```")))
				{
					FClaireonMarkdownBlock Block;
					Block.Type = EClaireonBlockType::CodeBlock;
					Block.CodeContent = CodeBlockBuffer;
					Block.CodeLanguage = CodeBlockLanguage;
					Blocks.Add(MoveTemp(Block));
					CodeBlockBuffer.Empty();
					CodeBlockLanguage.Empty();
					State = EState::Normal;
				}
				else
				{
					if (!CodeBlockBuffer.IsEmpty())
					{
						CodeBlockBuffer += TEXT("\n");
					}
					CodeBlockBuffer += Line;
				}
				break;
			}

			case EState::InTable:
			{
				if (Line.TrimStart().StartsWith(TEXT("|")))
				{
					TableLines.Add(Line);
				}
				else
				{
					FlushTable();
					FlushProse();
					State = EState::Normal;
					goto ProcessNormalBlock;
				}
				break;
			}

			case EState::Normal:
			{
			ProcessNormalBlock:
				// Fenced code block start
				if (Line.TrimStart().StartsWith(TEXT("```")))
				{
					FlushProse();
					FString Trimmed = Line.TrimStart();
					CodeBlockLanguage = Trimmed.Mid(3).TrimStartAndEnd();
					State = EState::InCodeBlock;
				}
				// Table row
				else if (Line.TrimStart().StartsWith(TEXT("|")))
				{
					FlushProse();
					State = EState::InTable;
					TableLines.Add(Line);
				}
				// Horizontal rule
				else if (Line.TrimStartAndEnd() == TEXT("---") ||
						 Line.TrimStartAndEnd() == TEXT("***") ||
						 Line.TrimStartAndEnd() == TEXT("___"))
				{
					FlushProse();
					FClaireonMarkdownBlock Block;
					Block.Type = EClaireonBlockType::Separator;
					Blocks.Add(MoveTemp(Block));
				}
				// Empty line (paragraph break)
				else if (Line.TrimStartAndEnd().IsEmpty())
				{
					FlushProse();
				}
				// Header ### H3
				else if (Line.StartsWith(TEXT("### ")))
				{
					FlushProse();
					FString Text = Line.Mid(4).TrimStartAndEnd();
					Text.ReplaceInline(TEXT("**"), TEXT(""));

					FClaireonMarkdownBlock Block;
					Block.Type = EClaireonBlockType::Prose;
					FClaireonStyledLine StyledLine;
					StyledLine.Text = Text;
					FClaireonTextSegment Seg;
					Seg.StyleName = TEXT("RichText.Header3");
					Seg.StartIndex = 0;
					Seg.EndIndex = Text.Len();
					StyledLine.Segments.Add(Seg);
					Block.Lines.Add(MoveTemp(StyledLine));
					Blocks.Add(MoveTemp(Block));
				}
				// Header ## H2
				else if (Line.StartsWith(TEXT("## ")))
				{
					FlushProse();
					FString Text = Line.Mid(3).TrimStartAndEnd();
					Text.ReplaceInline(TEXT("**"), TEXT(""));

					FClaireonMarkdownBlock Block;
					Block.Type = EClaireonBlockType::Prose;
					FClaireonStyledLine StyledLine;
					StyledLine.Text = Text;
					FClaireonTextSegment Seg;
					Seg.StyleName = TEXT("RichText.Header2");
					Seg.StartIndex = 0;
					Seg.EndIndex = Text.Len();
					StyledLine.Segments.Add(Seg);
					Block.Lines.Add(MoveTemp(StyledLine));
					Blocks.Add(MoveTemp(Block));
				}
				// Header # H1
				else if (Line.StartsWith(TEXT("# ")))
				{
					FlushProse();
					FString Text = Line.Mid(2).TrimStartAndEnd();
					Text.ReplaceInline(TEXT("**"), TEXT(""));

					FClaireonMarkdownBlock Block;
					Block.Type = EClaireonBlockType::Prose;
					FClaireonStyledLine StyledLine;
					StyledLine.Text = Text;
					FClaireonTextSegment Seg;
					Seg.StyleName = TEXT("RichText.Header1");
					Seg.StartIndex = 0;
					Seg.EndIndex = Text.Len();
					StyledLine.Segments.Add(Seg);
					Block.Lines.Add(MoveTemp(StyledLine));
					Blocks.Add(MoveTemp(Block));
				}
				// Bullet list
				else if (Line.Len() >= 2 &&
						 (Line.StartsWith(TEXT("- ")) || Line.StartsWith(TEXT("* "))))
				{
					const FString Text = Line.Mid(2).TrimStartAndEnd();
					const FString Prefix = TEXT("  \u2022 ");
					FClaireonStyledLine Parsed = ParseInlineSegments(
						Text, TEXT("RichText.Default"));

					// Prepend bullet prefix
					Parsed.Text = Prefix + Parsed.Text;
					for (FClaireonTextSegment& Seg : Parsed.Segments)
					{
						Seg.StartIndex += Prefix.Len();
						Seg.EndIndex += Prefix.Len();
					}
					FClaireonTextSegment PrefixSeg;
					PrefixSeg.StyleName = TEXT("RichText.Default");
					PrefixSeg.StartIndex = 0;
					PrefixSeg.EndIndex = Prefix.Len();
					Parsed.Segments.Insert(PrefixSeg, 0);

					EnsureProse().Lines.Add(MoveTemp(Parsed));
				}
				// Numbered list
				else if (FRegexMatcher(FRegexPattern(TEXT("^\\d+\\. ")), Line).FindNext())
				{
					int32 DotPos = INDEX_NONE;
					Line.FindChar(TEXT('.'), DotPos);
					const FString Number = Line.Left(DotPos);
					const FString Text = Line.Mid(DotPos + 2).TrimStartAndEnd();
					const FString Prefix = FString::Printf(TEXT("  %s. "), *Number);
					FClaireonStyledLine Parsed = ParseInlineSegments(
						Text, TEXT("RichText.Default"));

					Parsed.Text = Prefix + Parsed.Text;
					for (FClaireonTextSegment& Seg : Parsed.Segments)
					{
						Seg.StartIndex += Prefix.Len();
						Seg.EndIndex += Prefix.Len();
					}
					FClaireonTextSegment PrefixSeg;
					PrefixSeg.StyleName = TEXT("RichText.Default");
					PrefixSeg.StartIndex = 0;
					PrefixSeg.EndIndex = Prefix.Len();
					Parsed.Segments.Insert(PrefixSeg, 0);

					EnsureProse().Lines.Add(MoveTemp(Parsed));
				}
				// Blockquote
				else if (Line.StartsWith(TEXT("> ")))
				{
					const FString Text = Line.Mid(2).TrimStartAndEnd();
					const FString Prefix = TEXT("    ");
					FClaireonStyledLine Parsed = ParseInlineSegments(
						Text, TEXT("RichText.Default"));

					Parsed.Text = Prefix + Parsed.Text;
					for (FClaireonTextSegment& Seg : Parsed.Segments)
					{
						Seg.StartIndex += Prefix.Len();
						Seg.EndIndex += Prefix.Len();
					}
					FClaireonTextSegment PrefixSeg;
					PrefixSeg.StyleName = TEXT("RichText.Default");
					PrefixSeg.StartIndex = 0;
					PrefixSeg.EndIndex = Prefix.Len();
					Parsed.Segments.Insert(PrefixSeg, 0);

					EnsureProse().Lines.Add(MoveTemp(Parsed));
				}
				// Normal text
				else
				{
					EnsureProse().Lines.Add(
						ParseInlineSegments(Line, TEXT("RichText.Default")));
				}
				break;
			}
		}
	}

	// Flush any open state at end of input
	if (State == EState::InCodeBlock && !CodeBlockBuffer.IsEmpty())
	{
		FClaireonMarkdownBlock Block;
		Block.Type = EClaireonBlockType::CodeBlock;
		Block.CodeContent = CodeBlockBuffer;
		Block.CodeLanguage = CodeBlockLanguage;
		Blocks.Add(MoveTemp(Block));
	}
	else if (State == EState::InTable)
	{
		FlushTable();
	}

	return Blocks;
}
