// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonMarkdownParser.h"
#include "ClaireonRichTextStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateTypes.h"

// ---------------------------------------------------------------------------
// Plain text & XML escaping
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, PlainText, UNTEST_TIMEOUTMS(5000))
{
	const FString Result = FClaireonMarkdownParser::ConvertToRichText(TEXT("Hello world"));
	UNTEST_ASSERT_STREQ(*Result, TEXT("Hello world"));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, XmlEscape, UNTEST_TIMEOUTMS(5000))
{
	const FString Result =
		FClaireonMarkdownParser::ConvertToRichText(TEXT("if (a < b && c > d)"));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("&lt;")));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("&amp;")));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("&gt;")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, EmptyInput, UNTEST_TIMEOUTMS(5000))
{
	const FString Result = FClaireonMarkdownParser::ConvertToRichText(TEXT(""));
	UNTEST_ASSERT_TRUE(Result.IsEmpty());
	co_return;
}

// ---------------------------------------------------------------------------
// Headers
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, Header1, UNTEST_TIMEOUTMS(5000))
{
	const FString Result = FClaireonMarkdownParser::ConvertToRichText(TEXT("# Title"));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("<RichText.Header1>Title</>")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, Header2, UNTEST_TIMEOUTMS(5000))
{
	const FString Result = FClaireonMarkdownParser::ConvertToRichText(TEXT("## Subtitle"));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("<RichText.Header2>Subtitle</>")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, Header3, UNTEST_TIMEOUTMS(5000))
{
	const FString Result = FClaireonMarkdownParser::ConvertToRichText(TEXT("### Section"));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("<RichText.Header3>Section</>")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, MultipleHeaders, UNTEST_TIMEOUTMS(5000))
{
	const FString Result =
		FClaireonMarkdownParser::ConvertToRichText(TEXT("# H1\n\n## H2\n\ntext"));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("<RichText.Header1>")));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("<RichText.Header2>")));
	co_return;
}

// ---------------------------------------------------------------------------
// Inline formatting
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, Bold, UNTEST_TIMEOUTMS(5000))
{
	const FString Result =
		FClaireonMarkdownParser::ConvertToRichText(TEXT("**bold text**"));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("<RichText.Bold>bold text</>")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, Italic, UNTEST_TIMEOUTMS(5000))
{
	const FString Result =
		FClaireonMarkdownParser::ConvertToRichText(TEXT("*italic text*"));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("<RichText.Italic>italic text</>")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, BoldItalic, UNTEST_TIMEOUTMS(5000))
{
	const FString Result =
		FClaireonMarkdownParser::ConvertToRichText(TEXT("***both***"));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("<RichText.BoldItalic>both</>")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, InlineCode, UNTEST_TIMEOUTMS(5000))
{
	const FString Result =
		FClaireonMarkdownParser::ConvertToRichText(TEXT("`foo()`"));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("<RichText.Code>foo()</>")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, MixedInline, UNTEST_TIMEOUTMS(5000))
{
	const FString Result =
		FClaireonMarkdownParser::ConvertToRichText(TEXT("Use **bold** and *italic*"));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("<RichText.Bold>")));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("<RichText.Italic>")));
	co_return;
}

// ---------------------------------------------------------------------------
// Lists
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, BulletList, UNTEST_TIMEOUTMS(5000))
{
	const FString Result =
		FClaireonMarkdownParser::ConvertToRichText(TEXT("- item one\n- item two"));
	// U+2022 is the bullet character
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("\u2022")));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("item one")));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("item two")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, NumberedList, UNTEST_TIMEOUTMS(5000))
{
	const FString Result =
		FClaireonMarkdownParser::ConvertToRichText(TEXT("1. first\n2. second"));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("1.")));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("first")));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("2.")));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("second")));
	co_return;
}

// ---------------------------------------------------------------------------
// Code blocks
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, CodeBlockPassthrough, UNTEST_TIMEOUTMS(5000))
{
	const FString Result = FClaireonMarkdownParser::ConvertToRichText(
		TEXT("```\n**not bold**\n```"));
	// Code block content should NOT have bold markup applied
	UNTEST_ASSERT_FALSE(Result.Contains(TEXT("<RichText.Bold>")));
	co_return;
}

// ---------------------------------------------------------------------------
// Markup validation
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, MarkupValidation, UNTEST_TIMEOUTMS(5000))
{
	UNTEST_ASSERT_TRUE(
		FClaireonMarkdownParser::ValidateMarkup(TEXT("<RichText.Bold>balanced</>")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, MarkupValidationFail, UNTEST_TIMEOUTMS(5000))
{
	UNTEST_ASSERT_FALSE(
		FClaireonMarkdownParser::ValidateMarkup(TEXT("<RichText.Bold>unbalanced")));
	co_return;
}

// ---------------------------------------------------------------------------
// Style set registration (Stage 006)
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, StyleSetRegistered, UNTEST_TIMEOUTMS(5000))
{
	// Ensure style set is initialized (may not be in commandlet mode)
	FClaireonRichTextStyle::Initialize();

	const FSlateStyleSet& StyleSet = FClaireonRichTextStyle::Get();

	// Verify all expected styles are registered
	const TCHAR* ExpectedStyles[] = {
		TEXT("RichText.Default"),
		TEXT("RichText.Bold"),
		TEXT("RichText.Italic"),
		TEXT("RichText.BoldItalic"),
		TEXT("RichText.Header1"),
		TEXT("RichText.Header2"),
		TEXT("RichText.Header3"),
		TEXT("RichText.Code"),
		TEXT("RichText.AssetLink"),
	};

	for (const TCHAR* StyleName : ExpectedStyles)
	{
		UNTEST_ASSERT_TRUE(StyleSet.HasWidgetStyle<FTextBlockStyle>(StyleName));
	}

	co_return;
}

// ---------------------------------------------------------------------------
// Code blocks (Stage 008)
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, CodeBlockBasic, UNTEST_TIMEOUTMS(5000))
{
	const FString Result =
		FClaireonMarkdownParser::ConvertToRichText(TEXT("```\nint x = 1;\n```"));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("<codeblock")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, CodeBlockWithLang, UNTEST_TIMEOUTMS(5000))
{
	const FString Result =
		FClaireonMarkdownParser::ConvertToRichText(TEXT("```cpp\nint x = 1;\n```"));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("lang=\"cpp\"")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, CodeBlockPreservesContent, UNTEST_TIMEOUTMS(5000))
{
	const FString Result = FClaireonMarkdownParser::ConvertToRichText(
		TEXT("```\n**not bold**\n<html>\n```"));
	// No markdown processing inside code blocks
	UNTEST_ASSERT_FALSE(Result.Contains(TEXT("<RichText.Bold>")));
	// Content is URL-encoded in src attribute
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("src=\"")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, CodeBlockMultiline, UNTEST_TIMEOUTMS(5000))
{
	const FString Result = FClaireonMarkdownParser::ConvertToRichText(
		TEXT("```\nline1\nline2\nline3\n```"));
	// All lines should be encoded in src attribute
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("<codeblock")));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("src=\"")));
	co_return;
}

// ---------------------------------------------------------------------------
// Asset path detection (Stage 010)
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, AssetPathGame, UNTEST_TIMEOUTMS(5000))
{
	const FString Result =
		FClaireonMarkdownParser::ConvertToRichText(TEXT("Check /Game/Blueprints/MyBP"));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("<a id=\"asset\"")));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("href=\"/Game/Blueprints/MyBP\"")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, AssetPathScript, UNTEST_TIMEOUTMS(5000))
{
	const FString Result =
		FClaireonMarkdownParser::ConvertToRichText(TEXT("See /Script/Engine.MyClass"));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("<a id=\"asset\"")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, AssetPathEngine, UNTEST_TIMEOUTMS(5000))
{
	const FString Result =
		FClaireonMarkdownParser::ConvertToRichText(TEXT("From /Engine/BasicShapes/Cube"));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("<a id=\"asset\"")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, AssetPathMultiple, UNTEST_TIMEOUTMS(5000))
{
	const FString Result =
		FClaireonMarkdownParser::ConvertToRichText(TEXT("Use /Game/A and /Game/B"));
	// Count occurrences of <a id="asset"
	int32 Count = 0;
	int32 Pos = 0;
	while ((Pos = Result.Find(TEXT("<a id=\"asset\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos)) != INDEX_NONE)
	{
		++Count;
		Pos += 1;
	}
	UNTEST_ASSERT_EQ(Count, 2);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, AssetPathInCodeBlock, UNTEST_TIMEOUTMS(5000))
{
	const FString Result = FClaireonMarkdownParser::ConvertToRichText(
		TEXT("```\n/Game/NotALink\n```"));
	// Code blocks should NOT have asset links
	UNTEST_ASSERT_FALSE(Result.Contains(TEXT("<a id=\"asset\"")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, NoAssetPath, UNTEST_TIMEOUTMS(5000))
{
	const FString Result =
		FClaireonMarkdownParser::ConvertToRichText(TEXT("Regular text without paths"));
	UNTEST_ASSERT_FALSE(Result.Contains(TEXT("<a id=\"asset\"")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, AssetPathWithDot, UNTEST_TIMEOUTMS(5000))
{
	const FString Result =
		FClaireonMarkdownParser::ConvertToRichText(TEXT("/Game/UI/WBP_HUD.WBP_HUD"));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("<a id=\"asset\"")));
	UNTEST_ASSERT_TRUE(Result.Contains(TEXT("WBP_HUD.WBP_HUD")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, NotAnAssetPath, UNTEST_TIMEOUTMS(5000))
{
	const FString Result = FClaireonMarkdownParser::ConvertToRichText(
		TEXT("See Source/MyModule/Private/MyFile.cpp"));
	UNTEST_ASSERT_FALSE(Result.Contains(TEXT("<a id=\"asset\"")));
	co_return;
}

// ---------------------------------------------------------------------------
// ParseToBlocks — structured output (Stage 013+)
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, BlocksEmpty, UNTEST_TIMEOUTMS(5000))
{
	const TArray<FClaireonMarkdownBlock> Blocks =
		FClaireonMarkdownParser::ParseToBlocks(TEXT(""));
	UNTEST_ASSERT_EQ(Blocks.Num(), 0);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, BlocksPlainText, UNTEST_TIMEOUTMS(5000))
{
	const TArray<FClaireonMarkdownBlock> Blocks =
		FClaireonMarkdownParser::ParseToBlocks(TEXT("Hello world"));
	UNTEST_ASSERT_EQ(Blocks.Num(), 1);
	UNTEST_ASSERT_TRUE(Blocks[0].Type == EClaireonBlockType::Prose);
	UNTEST_ASSERT_EQ(Blocks[0].Lines.Num(), 1);
	UNTEST_ASSERT_STREQ(*Blocks[0].Lines[0].Text, TEXT("Hello world"));
	UNTEST_ASSERT_EQ(Blocks[0].Lines[0].Segments.Num(), 1);
	UNTEST_ASSERT_STREQ(*Blocks[0].Lines[0].Segments[0].StyleName, TEXT("RichText.Default"));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, BlocksHeader, UNTEST_TIMEOUTMS(5000))
{
	const TArray<FClaireonMarkdownBlock> Blocks =
		FClaireonMarkdownParser::ParseToBlocks(TEXT("# Title"));
	UNTEST_ASSERT_EQ(Blocks.Num(), 1);
	UNTEST_ASSERT_TRUE(Blocks[0].Type == EClaireonBlockType::Prose);
	UNTEST_ASSERT_STREQ(*Blocks[0].Lines[0].Text, TEXT("Title"));
	UNTEST_ASSERT_STREQ(*Blocks[0].Lines[0].Segments[0].StyleName, TEXT("RichText.Header1"));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, BlocksBold, UNTEST_TIMEOUTMS(5000))
{
	const TArray<FClaireonMarkdownBlock> Blocks =
		FClaireonMarkdownParser::ParseToBlocks(TEXT("Use **bold** text"));
	UNTEST_ASSERT_EQ(Blocks.Num(), 1);
	// Plain text should have markers stripped
	UNTEST_ASSERT_STREQ(*Blocks[0].Lines[0].Text, TEXT("Use bold text"));
	// Should have 3 segments: "Use " (default), "bold" (bold), " text" (default)
	UNTEST_ASSERT_EQ(Blocks[0].Lines[0].Segments.Num(), 3);
	UNTEST_ASSERT_STREQ(*Blocks[0].Lines[0].Segments[1].StyleName, TEXT("RichText.Bold"));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, BlocksCodeBlock, UNTEST_TIMEOUTMS(5000))
{
	const TArray<FClaireonMarkdownBlock> Blocks =
		FClaireonMarkdownParser::ParseToBlocks(TEXT("```cpp\nint x = 1;\n```"));
	// Should produce a CodeBlock
	bool bFoundCodeBlock = false;
	for (const FClaireonMarkdownBlock& Block : Blocks)
	{
		if (Block.Type == EClaireonBlockType::CodeBlock)
		{
			bFoundCodeBlock = true;
			UNTEST_ASSERT_STREQ(*Block.CodeLanguage, TEXT("cpp"));
			UNTEST_ASSERT_TRUE(Block.CodeContent.Contains(TEXT("int x = 1;")));
		}
	}
	UNTEST_ASSERT_TRUE(bFoundCodeBlock);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, BlocksSeparator, UNTEST_TIMEOUTMS(5000))
{
	const TArray<FClaireonMarkdownBlock> Blocks =
		FClaireonMarkdownParser::ParseToBlocks(TEXT("above\n\n---\n\nbelow"));
	bool bFoundSeparator = false;
	for (const FClaireonMarkdownBlock& Block : Blocks)
	{
		if (Block.Type == EClaireonBlockType::Separator)
		{
			bFoundSeparator = true;
		}
	}
	UNTEST_ASSERT_TRUE(bFoundSeparator);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, BlocksMixed, UNTEST_TIMEOUTMS(5000))
{
	const TArray<FClaireonMarkdownBlock> Blocks =
		FClaireonMarkdownParser::ParseToBlocks(
			TEXT("# Title\n\nSome text\n\n```\ncode\n```\n\nMore text"));
	// Should have at least: prose (header), prose (text), code, prose (more text)
	UNTEST_ASSERT_GE(Blocks.Num(), 3);
	// First block should be the header (prose)
	UNTEST_ASSERT_TRUE(Blocks[0].Type == EClaireonBlockType::Prose);
	// Check that at least one code block exists
	bool bFoundCode = false;
	for (const FClaireonMarkdownBlock& Block : Blocks)
	{
		if (Block.Type == EClaireonBlockType::CodeBlock)
		{
			bFoundCode = true;
		}
	}
	UNTEST_ASSERT_TRUE(bFoundCode);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, BlocksAssetPath, UNTEST_TIMEOUTMS(5000))
{
	const TArray<FClaireonMarkdownBlock> Blocks =
		FClaireonMarkdownParser::ParseToBlocks(TEXT("Check /Game/Blueprints/MyBP here"));
	UNTEST_ASSERT_EQ(Blocks.Num(), 1);
	// Should have an AssetLink segment
	bool bFoundAssetLink = false;
	for (const FClaireonTextSegment& Seg : Blocks[0].Lines[0].Segments)
	{
		if (Seg.StyleName == TEXT("RichText.AssetLink"))
		{
			bFoundAssetLink = true;
			// The asset path should be preserved in the plain text
			const FString AssetText =
				Blocks[0].Lines[0].Text.Mid(Seg.StartIndex, Seg.EndIndex - Seg.StartIndex);
			UNTEST_ASSERT_TRUE(AssetText.Contains(TEXT("/Game/Blueprints/MyBP")));
		}
	}
	UNTEST_ASSERT_TRUE(bFoundAssetLink);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, BlocksBulletList, UNTEST_TIMEOUTMS(5000))
{
	const TArray<FClaireonMarkdownBlock> Blocks =
		FClaireonMarkdownParser::ParseToBlocks(TEXT("- item one\n- item two"));
	UNTEST_ASSERT_EQ(Blocks.Num(), 1);
	UNTEST_ASSERT_EQ(Blocks[0].Lines.Num(), 2);
	// Lines should contain bullet character
	UNTEST_ASSERT_TRUE(Blocks[0].Lines[0].Text.Contains(TEXT("\u2022")));
	UNTEST_ASSERT_TRUE(Blocks[0].Lines[0].Text.Contains(TEXT("item one")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, BlocksInlineCode, UNTEST_TIMEOUTMS(5000))
{
	const TArray<FClaireonMarkdownBlock> Blocks =
		FClaireonMarkdownParser::ParseToBlocks(TEXT("Use `foo()` here"));
	UNTEST_ASSERT_EQ(Blocks.Num(), 1);
	// Plain text should have backticks stripped
	UNTEST_ASSERT_STREQ(*Blocks[0].Lines[0].Text, TEXT("Use foo() here"));
	// Should have a Code segment for "foo()"
	bool bFoundCode = false;
	for (const FClaireonTextSegment& Seg : Blocks[0].Lines[0].Segments)
	{
		if (Seg.StyleName == TEXT("RichText.Code"))
		{
			bFoundCode = true;
			const FString CodeText =
				Blocks[0].Lines[0].Text.Mid(Seg.StartIndex, Seg.EndIndex - Seg.StartIndex);
			UNTEST_ASSERT_STREQ(*CodeText, TEXT("foo()"));
		}
	}
	UNTEST_ASSERT_TRUE(bFoundCode);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, BlocksNoAssetInCode, UNTEST_TIMEOUTMS(5000))
{
	const TArray<FClaireonMarkdownBlock> Blocks =
		FClaireonMarkdownParser::ParseToBlocks(TEXT("```\n/Game/NotALink\n```"));
	// The code block should contain the path as plain code, not as an asset link
	for (const FClaireonMarkdownBlock& Block : Blocks)
	{
		if (Block.Type == EClaireonBlockType::CodeBlock)
		{
			UNTEST_ASSERT_TRUE(Block.CodeContent.Contains(TEXT("/Game/NotALink")));
		}
		// Prose blocks should NOT have asset links from code block content
		if (Block.Type == EClaireonBlockType::Prose)
		{
			for (const FClaireonStyledLine& Line : Block.Lines)
			{
				for (const FClaireonTextSegment& Seg : Line.Segments)
				{
					UNTEST_ASSERT_FALSE(Seg.StyleName == TEXT("RichText.AssetLink"));
				}
			}
		}
	}
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, BlocksSegmentRanges, UNTEST_TIMEOUTMS(5000))
{
	const TArray<FClaireonMarkdownBlock> Blocks =
		FClaireonMarkdownParser::ParseToBlocks(TEXT("A **B** C"));
	UNTEST_ASSERT_EQ(Blocks.Num(), 1);
	const FClaireonStyledLine& Line = Blocks[0].Lines[0];
	UNTEST_ASSERT_STREQ(*Line.Text, TEXT("A B C"));
	// Verify segment ranges cover the full text without gaps
	UNTEST_ASSERT_GE(Line.Segments.Num(), 3);
	UNTEST_ASSERT_EQ(Line.Segments[0].StartIndex, 0);
	UNTEST_ASSERT_EQ(Line.Segments.Last().EndIndex, Line.Text.Len());
	co_return;
}

// ---------------------------------------------------------------------------
// Table parsing (Fix 2)
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, BlocksTableBasic, UNTEST_TIMEOUTMS(5000))
{
	const TArray<FClaireonMarkdownBlock> Blocks =
		FClaireonMarkdownParser::ParseToBlocks(
			TEXT("| Name | Value |\n|---|---|\n| foo | 42 |"));
	bool bFoundTable = false;
	for (const FClaireonMarkdownBlock& Block : Blocks)
	{
		if (Block.Type == EClaireonBlockType::Table)
		{
			bFoundTable = true;
			UNTEST_ASSERT_EQ(Block.TableHeaders.Num(), 2);
			UNTEST_ASSERT_STREQ(*Block.TableHeaders[0], TEXT("Name"));
			UNTEST_ASSERT_STREQ(*Block.TableHeaders[1], TEXT("Value"));
			UNTEST_ASSERT_EQ(Block.TableRows.Num(), 1);
			UNTEST_ASSERT_STREQ(*Block.TableRows[0][0], TEXT("foo"));
			UNTEST_ASSERT_STREQ(*Block.TableRows[0][1], TEXT("42"));
		}
	}
	UNTEST_ASSERT_TRUE(bFoundTable);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, BlocksTableMultiRow, UNTEST_TIMEOUTMS(5000))
{
	const TArray<FClaireonMarkdownBlock> Blocks =
		FClaireonMarkdownParser::ParseToBlocks(
			TEXT("| A | B |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |"));
	bool bFoundTable = false;
	for (const FClaireonMarkdownBlock& Block : Blocks)
	{
		if (Block.Type == EClaireonBlockType::Table)
		{
			bFoundTable = true;
			UNTEST_ASSERT_EQ(Block.TableHeaders.Num(), 2);
			UNTEST_ASSERT_EQ(Block.TableRows.Num(), 2);
			UNTEST_ASSERT_STREQ(*Block.TableRows[1][0], TEXT("3"));
			UNTEST_ASSERT_STREQ(*Block.TableRows[1][1], TEXT("4"));
		}
	}
	UNTEST_ASSERT_TRUE(bFoundTable);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, BlocksTableSeparatorSkipped, UNTEST_TIMEOUTMS(5000))
{
	// Separator rows with colons (alignment markers) should be skipped
	const TArray<FClaireonMarkdownBlock> Blocks =
		FClaireonMarkdownParser::ParseToBlocks(
			TEXT("| H1 | H2 |\n|:---|---:|\n| d1 | d2 |"));
	bool bFoundTable = false;
	for (const FClaireonMarkdownBlock& Block : Blocks)
	{
		if (Block.Type == EClaireonBlockType::Table)
		{
			bFoundTable = true;
			// Separator should not appear as a data row
			UNTEST_ASSERT_EQ(Block.TableRows.Num(), 1);
		}
	}
	UNTEST_ASSERT_TRUE(bFoundTable);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, MarkdownParser, BlocksTableFollowedByProse, UNTEST_TIMEOUTMS(5000))
{
	const TArray<FClaireonMarkdownBlock> Blocks =
		FClaireonMarkdownParser::ParseToBlocks(
			TEXT("| A | B |\n|---|---|\n| 1 | 2 |\n\nSome text after"));
	bool bFoundTable = false;
	bool bFoundProse = false;
	for (const FClaireonMarkdownBlock& Block : Blocks)
	{
		if (Block.Type == EClaireonBlockType::Table)
		{
			bFoundTable = true;
		}
		if (Block.Type == EClaireonBlockType::Prose)
		{
			for (const FClaireonStyledLine& Line : Block.Lines)
			{
				if (Line.Text.Contains(TEXT("Some text after")))
				{
					bFoundProse = true;
				}
			}
		}
	}
	UNTEST_ASSERT_TRUE(bFoundTable);
	UNTEST_ASSERT_TRUE(bFoundProse);
	co_return;
}


#endif // WITH_UNTESTED
