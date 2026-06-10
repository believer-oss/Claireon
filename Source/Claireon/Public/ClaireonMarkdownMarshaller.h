// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Framework/Text/BaseTextLayoutMarshaller.h"
#include "Framework/Text/SlateHyperlinkRun.h"
#include "ClaireonMarkdownParser.h"

class FSlateStyleSet;

/**
 * Custom text layout marshaller that creates styled FSlateTextRun instances
 * from pre-parsed markdown blocks. Used with SMultiLineEditableText to
 * render rich text that supports text selection and Ctrl+C copy.
 *
 * Asset link segments are rendered as FSlateHyperlinkRun instances,
 * providing native clickable link behavior with hover cursor changes.
 *
 * Pattern follows FOutputLogTextLayoutMarshaller from the engine Output Log.
 */
class FClaireonMarkdownMarshaller : public FBaseTextLayoutMarshaller
{
public:
	static TSharedRef<FClaireonMarkdownMarshaller> Create(
		const TArray<FClaireonStyledLine>& InStyledLines,
		const FSlateStyleSet& InStyleSet,
		FSlateHyperlinkRun::FOnClick InAssetLinkClickDelegate = FSlateHyperlinkRun::FOnClick());

	virtual ~FClaireonMarkdownMarshaller() override;

	// ITextLayoutMarshaller interface
	virtual void SetText(const FString& SourceString, FTextLayout& TargetTextLayout) override;
	virtual void GetText(FString& TargetString, const FTextLayout& SourceTextLayout) override;

private:
	FClaireonMarkdownMarshaller(
		const TArray<FClaireonStyledLine>& InStyledLines,
		const FSlateStyleSet& InStyleSet,
		FSlateHyperlinkRun::FOnClick InAssetLinkClickDelegate);

	TArray<FClaireonStyledLine> StyledLines;
	const FSlateStyleSet& StyleSet;
	FSlateHyperlinkRun::FOnClick AssetLinkClickDelegate;
};
