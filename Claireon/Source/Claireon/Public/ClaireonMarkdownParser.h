// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

/** A styled segment within a line of text — maps a character range to a style name. */
struct FClaireonTextSegment
{
	FString StyleName;
	int32 StartIndex = 0;
	int32 EndIndex = 0;
};

/** A line of plain text with associated style segments. */
struct FClaireonStyledLine
{
	FString Text;
	TArray<FClaireonTextSegment> Segments;
};

/** Block type in parsed markdown output. */
enum class EClaireonBlockType : uint8
{
	Prose,
	CodeBlock,
	Separator,
	Table
};

/** A parsed markdown block with typed content. */
struct FClaireonMarkdownBlock
{
	EClaireonBlockType Type = EClaireonBlockType::Prose;
	TArray<FClaireonStyledLine> Lines; // For Prose blocks
	FString CodeContent;			// For CodeBlock: raw code text
	FString CodeLanguage;			// For CodeBlock: language label
	TArray<FString> TableHeaders;	// For Table blocks: column headers
	TArray<TArray<FString>> TableRows; // For Table blocks: data rows
};

/**
 * Stateless utility that converts raw markdown text to structured blocks
 * or SRichTextBlock XML markup. Used by the REPL widget to render assistant
 * responses with formatting, code blocks, and clickable asset links.
 */
class FClaireonMarkdownParser
{
public:
	/** Convert raw markdown text to SRichTextBlock-compatible markup. */
	static FString ConvertToRichText(const FString& InMarkdown);

	/**
	 * Parse markdown into structured blocks with styled text segments.
	 * Returns an array of blocks (Prose, CodeBlock, Separator) where each
	 * prose block contains lines with per-character style information.
	 */
	static TArray<FClaireonMarkdownBlock> ParseToBlocks(const FString& InMarkdown);

	/** Detect and wrap asset paths in hyperlink markup. */
	static FString WrapAssetPaths(const FString& InText);

	/**
	 * Validate that the output markup has balanced tags.
	 * Returns true if valid, false if malformed (caller should fall back to plain text).
	 */
	static bool ValidateMarkup(const FString& InMarkup);

	/**
	 * Parse inline markdown into styled segments (no XML markup).
	 * Strips markdown markers, produces plain text + segment array.
	 */
	static FClaireonStyledLine ParseInlineSegments(
		const FString& InLine, const FString& InDefaultStyle);

private:
	/** Escape XML special characters (<, >, &) in content text. */
	static FString EscapeXml(const FString& InText);

	/** Convert inline markdown (bold, italic, code) within a line to XML markup. */
	static FString ConvertInlineMarkdown(const FString& InLine);

	/** Process a fenced code block and return widget decorator markup. */
	static FString ConvertCodeBlock(const FString& InCode, const FString& InLanguage);

	/** Process a markdown table and return monospace-formatted markup. */
	static FString ConvertTable(const TArray<FString>& InTableLines);
};
