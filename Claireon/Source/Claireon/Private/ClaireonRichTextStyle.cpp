// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonRichTextStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"

TSharedPtr<FSlateStyleSet> FClaireonRichTextStyle::StyleInstance = nullptr;

void FClaireonRichTextStyle::Initialize()
{
	if (!StyleInstance.IsValid())
	{
		StyleInstance = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
	}
}

void FClaireonRichTextStyle::Shutdown()
{
	if (StyleInstance.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
		ensure(StyleInstance.IsUnique());
		StyleInstance.Reset();
	}
}

FName FClaireonRichTextStyle::GetStyleSetName()
{
	static FName StyleSetName(TEXT("ClaireonRichText"));
	return StyleSetName;
}

TSharedRef<FSlateStyleSet> FClaireonRichTextStyle::Create()
{
	TSharedRef<FSlateStyleSet> StyleRef =
		MakeShareable(new FSlateStyleSet(FClaireonRichTextStyle::GetStyleSetName()));

	FSlateStyleSet& Style = StyleRef.Get();

	const FTextBlockStyle NormalStyle =
		FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>(TEXT("NormalText"));

	auto Hex = [](const TCHAR* HexString) -> FLinearColor
	{
		return FLinearColor::FromSRGBColor(FColor::FromHex(HexString));
	};

	// Color constants matching the Claireon design tokens.
	const FLinearColor RTS_Primary = Hex(TEXT("ECEDEF"));
	const FLinearColor RTS_Secondary = Hex(TEXT("A8ACB3"));
	const FLinearColor RTS_Accent = Hex(TEXT("F0A063"));

	// Default assistant text
	Style.Set(TEXT("RichText.Default"),
		FTextBlockStyle(NormalStyle)
			.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 10))
			.SetColorAndOpacity(FSlateColor(RTS_Primary)));

	// Bold
	Style.Set(TEXT("RichText.Bold"),
		FTextBlockStyle(NormalStyle)
			.SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 10))
			.SetColorAndOpacity(FSlateColor(RTS_Primary)));

	// Italic
	Style.Set(TEXT("RichText.Italic"),
		FTextBlockStyle(NormalStyle)
			.SetFont(FCoreStyle::GetDefaultFontStyle("Italic", 10))
			.SetColorAndOpacity(FSlateColor(RTS_Primary)));

	// Bold Italic
	Style.Set(TEXT("RichText.BoldItalic"),
		FTextBlockStyle(NormalStyle)
			.SetFont(FCoreStyle::GetDefaultFontStyle("BoldItalic", 10))
			.SetColorAndOpacity(FSlateColor(RTS_Primary)));

	// Headers
	Style.Set(TEXT("RichText.Header1"),
		FTextBlockStyle(NormalStyle)
			.SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 14))
			.SetColorAndOpacity(FSlateColor(RTS_Primary)));

	Style.Set(TEXT("RichText.Header2"),
		FTextBlockStyle(NormalStyle)
			.SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 12))
			.SetColorAndOpacity(FSlateColor(RTS_Primary)));

	Style.Set(TEXT("RichText.Header3"),
		FTextBlockStyle(NormalStyle)
			.SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 11))
			.SetColorAndOpacity(FSlateColor(RTS_Secondary)));

	// Inline code (monospace, amber)
	Style.Set(TEXT("RichText.Code"),
		FTextBlockStyle(NormalStyle)
			.SetFont(FCoreStyle::GetDefaultFontStyle("Mono", 9))
			.SetColorAndOpacity(FSlateColor(RTS_Primary)));

	// Asset link (amber, used as fallback FTextBlockStyle for non-hyperlink contexts)
	Style.Set(TEXT("RichText.AssetLink"),
		FTextBlockStyle(NormalStyle)
			.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 10))
			.SetColorAndOpacity(FSlateColor(RTS_Accent)));

	// Asset hyperlink style (FHyperlinkStyle for FSlateHyperlinkRun clickable links)
	{
		FTextBlockStyle LinkTextStyle(NormalStyle);
		LinkTextStyle.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 10));
		LinkTextStyle.SetColorAndOpacity(FSlateColor(RTS_Accent));

		FButtonStyle UnderlineButtonStyle;
		UnderlineButtonStyle.SetNormal(FSlateNoResource());
		UnderlineButtonStyle.SetHovered(FSlateNoResource());
		UnderlineButtonStyle.SetPressed(FSlateNoResource());
		UnderlineButtonStyle.SetNormalPadding(FMargin(0));
		UnderlineButtonStyle.SetPressedPadding(FMargin(0));

		FHyperlinkStyle HyperlinkStyle;
		HyperlinkStyle.SetTextStyle(LinkTextStyle);
		HyperlinkStyle.SetUnderlineStyle(UnderlineButtonStyle);
		HyperlinkStyle.SetPadding(FMargin(0));

		Style.Set(TEXT("RichText.AssetHyperlink"), HyperlinkStyle);
	}

	return StyleRef;
}

const FSlateStyleSet& FClaireonRichTextStyle::Get()
{
	check(StyleInstance.IsValid());
	return *StyleInstance;
}
