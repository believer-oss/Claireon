// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonMarkdownMarshaller.h"

#include "Framework/Text/SlateTextRun.h"
#include "Framework/Text/SlateHyperlinkRun.h"
#include "Framework/Text/TextLayout.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateTypes.h"

TSharedRef<FClaireonMarkdownMarshaller> FClaireonMarkdownMarshaller::Create(
	const TArray<FClaireonStyledLine>& InStyledLines,
	const FSlateStyleSet& InStyleSet,
	FSlateHyperlinkRun::FOnClick InAssetLinkClickDelegate)
{
	return MakeShareable(new FClaireonMarkdownMarshaller(
		InStyledLines, InStyleSet, MoveTemp(InAssetLinkClickDelegate)));
}

FClaireonMarkdownMarshaller::FClaireonMarkdownMarshaller(
	const TArray<FClaireonStyledLine>& InStyledLines,
	const FSlateStyleSet& InStyleSet,
	FSlateHyperlinkRun::FOnClick InAssetLinkClickDelegate)
	: StyledLines(InStyledLines)
	, StyleSet(InStyleSet)
	, AssetLinkClickDelegate(MoveTemp(InAssetLinkClickDelegate))
{
}

FClaireonMarkdownMarshaller::~FClaireonMarkdownMarshaller()
{
}

void FClaireonMarkdownMarshaller::SetText(
	const FString& SourceString, FTextLayout& TargetTextLayout)
{
	TargetTextLayout.ClearLines();

	const FTextBlockStyle& DefaultStyle =
		StyleSet.GetWidgetStyle<FTextBlockStyle>(TEXT("RichText.Default"));

	TArray<FTextLayout::FNewLineData> LinesToAdd;
	LinesToAdd.Reserve(StyledLines.Num());

	for (const FClaireonStyledLine& StyledLine : StyledLines)
	{
		TSharedRef<FString> LineText = MakeShared<FString>(StyledLine.Text);
		TArray<TSharedRef<IRun>> Runs;

		for (const FClaireonTextSegment& Seg : StyledLine.Segments)
		{
			// Asset links become clickable hyperlink runs
			if (Seg.StyleName == TEXT("RichText.AssetLink") &&
				AssetLinkClickDelegate.IsBound() &&
				StyleSet.HasWidgetStyle<FHyperlinkStyle>(TEXT("RichText.AssetHyperlink")))
			{
				FRunInfo RunInfo;
				RunInfo.Name = TEXT("RichText.AssetLink");
				RunInfo.MetaData.Add(
					TEXT("path"),
					LineText->Mid(Seg.StartIndex, Seg.EndIndex - Seg.StartIndex));

				const FHyperlinkStyle& HyperlinkStyle =
					StyleSet.GetWidgetStyle<FHyperlinkStyle>(
						TEXT("RichText.AssetHyperlink"));

				Runs.Add(FSlateHyperlinkRun::Create(
					RunInfo,
					LineText,
					HyperlinkStyle,
					AssetLinkClickDelegate,
					FSlateHyperlinkRun::FOnGenerateTooltip(),
					FSlateHyperlinkRun::FOnGetTooltipText(),
					FTextRange(Seg.StartIndex, Seg.EndIndex)));
				continue;
			}

			// All other segments use normal text runs
			const FTextBlockStyle* FoundStyle = nullptr;
			const FName StyleFName(*Seg.StyleName);
			if (StyleSet.HasWidgetStyle<FTextBlockStyle>(StyleFName))
			{
				FoundStyle = &StyleSet.GetWidgetStyle<FTextBlockStyle>(StyleFName);
			}
			else
			{
				FoundStyle = &DefaultStyle;
			}

			FRunInfo RunInfo;
			RunInfo.Name = Seg.StyleName;
			Runs.Add(FSlateTextRun::Create(
				RunInfo,
				LineText,
				*FoundStyle,
				FTextRange(Seg.StartIndex, Seg.EndIndex)));
		}

		// If no segments, create a single default run for the whole line
		if (Runs.Num() == 0)
		{
			FRunInfo RunInfo;
			Runs.Add(FSlateTextRun::Create(RunInfo, LineText, DefaultStyle));
		}

		LinesToAdd.Emplace(MoveTemp(LineText), MoveTemp(Runs));
	}

	TargetTextLayout.AddLines(LinesToAdd);
}

void FClaireonMarkdownMarshaller::GetText(
	FString& TargetString, const FTextLayout& SourceTextLayout)
{
	SourceTextLayout.GetAsText(TargetString);
}
