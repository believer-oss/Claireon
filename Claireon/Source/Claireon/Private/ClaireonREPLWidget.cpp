// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonREPLWidget.h"
#include "ClaireonServer.h"
#include "ClaireonModule.h"
#include "Modules/ModuleManager.h"
#include "ClaireonSettings.h"
#include "ClaireonREPLLogger.h"
#include "ClaireonAnthropicClient.h"
#include "ClaireonLog.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SBoxPanel.h"
#include "HAL/PlatformProcess.h"
#include "Async/Async.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Animation/CurveSequence.h"
#include "ISettingsModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/SlateTypes.h"
#include "Styling/SlateStyle.h"
#include "ClaireonMarkdownParser.h"
#include "ClaireonMarkdownMarshaller.h"
#include "ClaireonRichTextStyle.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Framework/Text/SlateTextRun.h"
#include "Framework/Text/TextLayout.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Internationalization/Regex.h"

#define LOCTEXT_NAMESPACE "ClaireonREPL"

// ---------------------------------------------------------------------------
// Claireon design tokens — matched to styles.css color variables
// ---------------------------------------------------------------------------
namespace ClaireonTokens
{
	static FLinearColor Hex(const TCHAR* HexString, float Alpha = 1.0f)
	{
		FLinearColor Color = FLinearColor::FromSRGBColor(FColor::FromHex(HexString));
		Color.A = Alpha;
		return Color;
	}

	// Surfaces
	static const FLinearColor BgPanel    = Hex(TEXT("15171A"));
	static const FLinearColor Bg2        = Hex(TEXT("1B1D21"));
	static const FLinearColor Bg3        = Hex(TEXT("22252A"));
	static const FLinearColor BgRowHover = Hex(TEXT("1D2024"));
	static const FLinearColor BgRowSel   = Hex(TEXT("2A2014"));

	// Lines
	static const FLinearColor Line1      = Hex(TEXT("25272C"));
	static const FLinearColor Line2      = Hex(TEXT("2E3137"));

	// Text
	static const FLinearColor Fg1        = Hex(TEXT("ECEDEF"));
	static const FLinearColor Fg2        = Hex(TEXT("A8ACB3"));
	static const FLinearColor Fg3        = Hex(TEXT("6B6F76"));
	static const FLinearColor Fg4        = Hex(TEXT("4A4D54"));

	// Signal
	static const FLinearColor Accent     = Hex(TEXT("E7873B"));
	static const FLinearColor AccentHi   = Hex(TEXT("F0A063"));
	static const FLinearColor AccentLine = Hex(TEXT("E7873B"), 0.35f);
	static const FLinearColor Ok         = Hex(TEXT("4EA780"));
	static const FLinearColor OkSoft     = Hex(TEXT("0D1411"));
	static const FLinearColor Warn       = Hex(TEXT("D3A24A"));
	static const FLinearColor Err        = Hex(TEXT("D96565"));
	static const FLinearColor ErrSoft    = Hex(TEXT("140D0D"));
	static const FLinearColor ErrHi      = Hex(TEXT("EC8585"));

	// Method chip: py (orange accent)
	static const FLinearColor PyChipBg   = Hex(TEXT("352F2C"));
	static const FLinearColor PyChipBdr  = Hex(TEXT("58433A"));
	static const FLinearColor TsChipBg   = Hex(TEXT("292F36"));
	static const FLinearColor TsChipBdr  = Hex(TEXT("363F4B"));
	static const FLinearColor TsText     = Hex(TEXT("98B3D9"));

	// Dark text on orange Send button (#1a1410)
	static const FLinearColor SendBtnText = Hex(TEXT("1A1410"));
}

namespace ClaireonREPLWidgetInternal
{

/**
 * Build an FTextBlockStyle derived from NormalText with the given foreground color.
 * SMultiLineEditableText uses TextStyle rather than ColorAndOpacity.
 * Returns by value; callers should store the result for the widget's lifetime.
 */
FTextBlockStyle MakeColoredTextStyle(const FLinearColor& InColor, const FSlateFontInfo* InFont = nullptr)
{
	FTextBlockStyle Style = FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>(TEXT("NormalText"));
	Style.SetColorAndOpacity(FSlateColor(InColor));
	if (InFont)
	{
		Style.SetFont(*InFont);
	}
	return Style;
}

}  // namespace ClaireonREPLWidgetInternal

void SClaireonREPLWidget::Construct(const FArguments& InArgs, FClaireonServer* InServer)
{
	Server = InServer;

	// Initialize logger
	Logger = MakeShared<FClaireonREPLLogger>();
	Logger->Initialize();

	// Initialize API client
	Client = MakeShared<FClaireonAnthropicClient>(Server, Logger);
	REPLEventHandle = Client->OnREPLEvent.AddSP(this, &SClaireonREPLWidget::OnREPLEvent);

	// Listen for settings changes (to update status bar model display)
	UClaireonSettings* Settings = GetMutableDefault<UClaireonSettings>();
	SettingsChangedHandle = Settings->OnSettingsChanged.AddLambda([this]()
	{
		if (StatusText.IsValid())
		{
			// Status bar will auto-refresh via binding
		}
	});

	// Listen for user-stop state changes
	if (Server)
	{
		UserStopDelegateHandle = Server->OnUserStopChanged.AddLambda([this](bool bActive)
		{
			bUserStopActive = bActive;
		});
	}

	// Initialize pulse animation
	PulseSequence = FCurveSequence(0.0f, 1.5f, ECurveEaseFunction::CubicInOut);
	PulseHandle = PulseSequence.AddCurve(0.0f, 1.5f, ECurveEaseFunction::CubicInOut);

	// Generate first conversation ID
	++ConversationCounter;
	CurrentConversationId = FString::Printf(TEXT("conv_%03d"), ConversationCounter);

	// Log session start
	if (Logger && Server)
	{
		const UClaireonSettings* S = UClaireonSettings::Get();
		Logger->LogSessionStart(S->ModelId, S->GetEffectiveSystemPrompt(),
			Client.IsValid() ? Client->GetAPIToolCount() : Server->GetTools().Num());
	}

	// Build widget
	ChildSlot
		[SAssignNew(RootSwitcher, SWidgetSwitcher)
			+ SWidgetSwitcher::Slot()[BuildOnboardingView()] // index 0
			+ SWidgetSwitcher::Slot()[BuildChatView()]		 // index 1
	];

	// Switch to correct view
	CheckApiKeyAndSwitchView();
}

SClaireonREPLWidget::~SClaireonREPLWidget()
{
	if (Client.IsValid())
	{
		Client->OnREPLEvent.Remove(REPLEventHandle);
		Client->Shutdown();
	}
	if (Server && UserStopDelegateHandle.IsValid())
	{
		Server->OnUserStopChanged.Remove(UserStopDelegateHandle);
	}
	if (UClaireonSettings* Settings = GetMutableDefault<UClaireonSettings>())
	{
		Settings->OnSettingsChanged.Remove(SettingsChangedHandle);
	}
	if (Logger)
	{
		Logger->Shutdown();
	}
}

// ------- Onboarding View -------

TSharedRef<SWidget> SClaireonREPLWidget::BuildOnboardingView()
{
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(32.0f)
			[
				SNew(SVerticalBox)

				// Title
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 16)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("OnboardingTitle", "Set up Claude"))
					.ColorAndOpacity(FSlateColor(ClaireonTokens::Accent))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 16))
				]
				// Subtitle
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 24)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("OnboardingSubtitle",
						"Ask questions, search assets, run scripts — all from here."))
					.ColorAndOpacity(FSlateColor(ClaireonTokens::Fg2))
					.AutoWrapText(true)
				]
				// Step 1
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Step1", "1. Get an Anthropic API key"))
					.ColorAndOpacity(FSlateColor(ClaireonTokens::Fg1))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 16)
				[
					SNew(SButton)
					.Text(LOCTEXT("GetKeyButton", "Open console.anthropic.com ->"))
					.OnClicked(this, &SClaireonREPLWidget::OnGetApiKeyClicked)
					.ButtonColorAndOpacity(ClaireonTokens::Bg3)
				]
				// Step 2
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Step2", "2. Paste your API key here"))
					.ColorAndOpacity(FSlateColor(ClaireonTokens::Fg1))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 16)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f)
					[
						SAssignNew(ApiKeyInputBox, SEditableTextBox)
						.HintText(LOCTEXT("ApiKeyHint", "sk-ant-..."))
						.IsPassword(true)
					]
				]
				// Step 3 / Verify
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Step3", "3. Verify and save"))
					.ColorAndOpacity(FSlateColor(ClaireonTokens::Fg1))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("VerifyButton", "Verify & Save"))
					.OnClicked(this, &SClaireonREPLWidget::OnVerifyAndSaveClicked)
					.ButtonColorAndOpacity(ClaireonTokens::Accent)
				]
				// Status message
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 16, 0, 0)
				[
					SNew(STextBlock)
					.Text(this, &SClaireonREPLWidget::GetOnboardingStatusText)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor(ClaireonTokens::Fg2))
				]
			]
		];
}

FReply SClaireonREPLWidget::OnGetApiKeyClicked()
{
	FPlatformProcess::LaunchURL(
		TEXT("https://console.anthropic.com/settings/keys"), nullptr, nullptr);
	return FReply::Handled();
}

FReply SClaireonREPLWidget::OnVerifyAndSaveClicked()
{
	if (!ApiKeyInputBox.IsValid())
		return FReply::Handled();

	FString KeyText = ApiKeyInputBox->GetText().ToString().TrimStartAndEnd();
	if (KeyText.IsEmpty())
	{
		OnboardingStatusMessage = TEXT("Please enter an API key.");
		return FReply::Handled();
	}

	OnboardingStatusMessage = TEXT("Verifying...");
	bOnboardingVerifying = true;

	// Send a minimal test request to verify the key
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> TestRequest =
		FHttpModule::Get().CreateRequest();
	TestRequest->SetURL(UClaireonSettings::Get()->ApiEndpointUrl);
	TestRequest->SetVerb(TEXT("POST"));
	TestRequest->SetHeader(TEXT("content-type"), TEXT("application/json"));
	TestRequest->SetHeader(TEXT("x-api-key"), KeyText);
	TestRequest->SetHeader(TEXT("anthropic-version"),
		UClaireonSettings::Get()->AnthropicVersion);
	TestRequest->SetContentAsString(
		TEXT("{\"model\":\"claude-haiku-4-5-20251001\",\"max_tokens\":1,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}"));

	FString KeyCapture = KeyText;
	TWeakPtr<SClaireonREPLWidget> WeakSelf = StaticCastSharedRef<SClaireonREPLWidget>(AsShared());

	TestRequest->OnProcessRequestComplete().BindLambda(
		[WeakSelf, KeyCapture](FHttpRequestPtr, FHttpResponsePtr Response, bool bSuccess)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, KeyCapture, Response, bSuccess]()
		{
			TSharedPtr<SClaireonREPLWidget> Self = WeakSelf.Pin();
			if (!Self.IsValid())
				return;

			Self->bOnboardingVerifying = false;

			if (!bSuccess || !Response.IsValid())
			{
				Self->OnboardingStatusMessage =
					TEXT("Network error — check your internet connection.");
				return;
			}

			int32 Code = Response->GetResponseCode();
			if (Code == 200 || Code == 400) // 400 = valid key, bad params (expected)
			{
				// Save the key
				UClaireonSettings* Settings = GetMutableDefault<UClaireonSettings>();
				Settings->SetAnthropicApiKey(KeyCapture);

				Self->OnboardingStatusMessage = TEXT("Key saved! Switching to chat...");
				Self->CheckApiKeyAndSwitchView();
			}
			else if (Code == 401)
			{
				Self->OnboardingStatusMessage =
					TEXT("Invalid API key — check the key and try again.");
			}
			else
			{
				Self->OnboardingStatusMessage =
					FString::Printf(TEXT("Unexpected response (%d). Try again."), Code);
			}
		});
	});

	TestRequest->ProcessRequest();
	return FReply::Handled();
}

FText SClaireonREPLWidget::GetOnboardingStatusText() const
{
	return FText::FromString(OnboardingStatusMessage);
}

// ------- Chat View -------

TSharedRef<SWidget> SClaireonREPLWidget::BuildChatView()
{
	// Thinking row lives below ConversationBox in the same scroll container.
	// Visibility is bound to bIsProcessing so it appears/disappears automatically.
	// It is a sibling of ConversationBox so ClearChildren() on ConversationBox
	// does not affect it.
	TSharedRef<SWidget> ThinkingRow = BuildThinkingRowWidget();

	// Wrap everything in an explicit --bg-1 background so the chat area
	// doesn't inherit the editor dock's near-black.
	return SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor(ClaireonTokens::BgPanel)
		[
			SNew(SVerticalBox)

			// ---- Transcript scroll ----
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SAssignNew(ConversationScroll, SScrollBox)
				+ SScrollBox::Slot()
				[
					SNew(SVerticalBox)
					// Messages
					+ SVerticalBox::Slot().AutoHeight()
					[
						SAssignNew(ConversationBox, SVerticalBox)
					]
					// Thinking row (collapsed when idle)
					+ SVerticalBox::Slot().AutoHeight()
					[
						ThinkingRow
					]
				]
			]

			// ---- Composer (hairline top border, then content) ----
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor(ClaireonTokens::Line1)
				.Padding(FMargin(0.0f, 1.0f, 0.0f, 0.0f))  // top hairline divider
				[
					BuildComposerWidget()
				]
			]
		];
}

// Pulsing dot + "Claireon is working . <step>" + Stop link
TSharedRef<SWidget> SClaireonREPLWidget::BuildThinkingRowWidget()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("NoBrush"))
		.Padding(FMargin(50.0f, 12.0f, 24.0f, 14.0f))   // 50px = body-hang matches cm-thinking
		.Visibility_Lambda([this]() -> EVisibility
		{
			return bIsProcessing ? EVisibility::Visible : EVisibility::Collapsed;
		})
		[
			SNew(SHorizontalBox)

			// Pulsing accent dot
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 10.0f, 0.0f))
			[
				SNew(SBox)
				.WidthOverride(7.0f)
				.HeightOverride(7.0f)
				[
					SNew(SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
					.BorderBackgroundColor_Lambda([this]() -> FLinearColor
					{
						float Pulse = (PulseSequence.IsPlaying())
							? FMath::Lerp(0.3f, 1.0f, PulseHandle.GetLerp())
							: 1.0f;
						return FLinearColor(
							ClaireonTokens::Accent.R,
							ClaireonTokens::Accent.G,
							ClaireonTokens::Accent.B,
							Pulse);
					})
				]
			]

			// "Claireon is working"
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ThinkingLabel", "Claireon is working"))
				.ColorAndOpacity(FSlateColor(ClaireonTokens::Fg2))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 12))
			]

			// " . <step>"
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SAssignNew(ThinkingStepText, STextBlock)
				.ColorAndOpacity(FSlateColor(ClaireonTokens::Fg3))
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 11))
			]

			+ SHorizontalBox::Slot().FillWidth(1.0f)[SNew(SSpacer)]

			// Stop link (right-aligned)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.OnClicked(this, &SClaireonREPLWidget::OnStopClicked)
				.ToolTipText(LOCTEXT("StopTooltip", "Cancel request (Escape or Ctrl+.)"))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("StopLink", "Stop"))
					.ColorAndOpacity(FSlateColor(ClaireonTokens::Fg3))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 11))
				]
			]
		];
}

// Composer: textarea + New topic/Send buttons + meta row (hint + ctx meter)
TSharedRef<SWidget> SClaireonREPLWidget::BuildComposerWidget()
{
	// Two nested SBorders: outer for the focus-responsive border colour, inner for bg.
	// We drive BorderBackgroundColor via lambda on the outer border so it reacts to
	// keyboard focus on the inner SMultiLineEditableText.
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("NoBrush"))
		.Padding(FMargin(18.0f, 10.0f, 18.0f, 12.0f))
		[
			SNew(SVerticalBox)

			// ---- Composer box (text + action column) ----
			+ SVerticalBox::Slot().AutoHeight()
			[
				// Outer border: provides the 1px focus-responsive stroke
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor_Lambda([this]() -> FLinearColor
				{
					return (InputBox.IsValid() && InputBox->HasKeyboardFocus())
						? ClaireonTokens::AccentLine
						: ClaireonTokens::Line2;
				})
				.Padding(FMargin(1.0f))
				[
					// Inner border: fills with Bg2 / Bg3 on focus
					SNew(SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
					.BorderBackgroundColor_Lambda([this]() -> FLinearColor
					{
						return (InputBox.IsValid() && InputBox->HasKeyboardFocus())
							? ClaireonTokens::Bg3
							: ClaireonTokens::Bg2;
					})
					.Padding(FMargin(0.0f))
					[
						SNew(SHorizontalBox)

						// Text area (fills width, no box wrapper so we own the border)
						+ SHorizontalBox::Slot().FillWidth(1.0f)
						[
							SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush("NoBrush"))
							.Padding(FMargin(12.0f, 9.0f, 12.0f, 10.0f))
							[
								SAssignNew(InputBox, SMultiLineEditableText)
								.HintText(LOCTEXT("ComposerHint",
									"Ask Claireon, or paste an error / asset path\u2026"))
								.OnTextCommitted(this, &SClaireonREPLWidget::OnInputTextCommitted)
								.ModiferKeyForNewLine(EModifierKey::Shift)
								.AutoWrapText(true)
							]
						]

						// Left divider + action buttons
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SBorder)
							.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
							.BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.10f))
							.Padding(FMargin(6.0f))
							[
								SNew(SVerticalBox)

								// New topic (ghost) -- ContentPadding overrides SimpleButton's
								// tall default so the button stays compact and matches the mock.
								+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, 4.0f))
								[
									SAssignNew(NewTopicButton, SButton)
									.ButtonStyle(FAppStyle::Get(), "SimpleButton")
									.ContentPadding(FMargin(6.0f, 3.0f))
									.OnClicked(this, &SClaireonREPLWidget::OnNewTopicClicked)
									.ToolTipText(LOCTEXT("NewTopicTooltip",
										"Clear history and start a new conversation (Ctrl+Enter)"))
									[
										SNew(STextBlock)
										.Text(this, &SClaireonREPLWidget::GetNewTopicButtonText)
										.ColorAndOpacity(FSlateColor(ClaireonTokens::Fg2))
										.Font(FCoreStyle::GetDefaultFontStyle("Regular", 11))
									]
								]

								// Send (primary, accent orange)
								+ SVerticalBox::Slot().AutoHeight()
								[
									SNew(SButton)
									.ButtonStyle(FAppStyle::Get(), "SimpleButton")
									.IsEnabled_Lambda([this]() -> bool
									{
										return !bIsProcessing
											&& InputBox.IsValid()
											&& !InputBox->GetText().IsEmpty();
									})
									.ContentPadding(FMargin(0.0f))
									.OnClicked(this, &SClaireonREPLWidget::OnSendClicked)
									.ToolTipText(LOCTEXT("SendTooltip", "Send message (Enter)"))
									[
										SNew(SBorder)
										.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
										.BorderBackgroundColor_Lambda([this]() -> FLinearColor
										{
											bool bEnabled = !bIsProcessing
												&& InputBox.IsValid()
												&& !InputBox->GetText().IsEmpty();
											FLinearColor C = ClaireonTokens::Accent;
											return bEnabled
												? C
												: FLinearColor(C.R, C.G, C.B, 0.55f);
										})
										.Padding(FMargin(6.0f, 3.0f))
										[
											SNew(STextBlock)
											.Text(LOCTEXT("SendButton", "Send  \u23ce"))
											.ColorAndOpacity(FSlateColor(ClaireonTokens::SendBtnText))
											.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
										]
									]
								]
							]
						]
					]
				]
			]

			// ---- Meta row: Shift+Enter hint ----
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(2.0f, 6.0f, 2.0f, 0.0f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ShiftEnterHint", "Shift+Enter for newline"))
					.ColorAndOpacity(FSlateColor(ClaireonTokens::Fg3))
					.Font(FCoreStyle::GetDefaultFontStyle("Mono", 10))
				]
			]
		];
}

// ------- Event Handling -------

void SClaireonREPLWidget::OnInputTextCommitted(const FText& Text,
	ETextCommit::Type CommitType)
{
	if (CommitType == ETextCommit::OnEnter && !bIsProcessing)
	{
		SubmitCurrentInput();
	}
}

void SClaireonREPLWidget::SubmitCurrentInput()
{
	if (!InputBox.IsValid() || !Client.IsValid())
		return;

	FString Input = InputBox->GetText().ToString().TrimStartAndEnd();
	if (Input.IsEmpty())
		return;

	// Push to history
	PushInputHistory(Input);
	HistoryCursor = -1;

	// Clear input
	InputBox->SetText(FText::GetEmpty());

	// Add user message to display
	FREPLConversationItem UserItem;
	UserItem.ItemType = FREPLConversationItem::EItemType::UserMessage;
	UserItem.Text = Input;
	AppendConversationItem(MoveTemp(UserItem));

	// Enter processing state
	SetProcessingState(true);

	// Send to client
	Client->SendMessage(Input, CurrentConversationId);
}

FReply SClaireonREPLWidget::OnSendClicked()
{
	SubmitCurrentInput();
	return FReply::Handled();
}

FReply SClaireonREPLWidget::OnStopClicked()
{
	if (Client.IsValid())
	{
		Client->CancelActiveRequest();
	}
	return FReply::Handled();
}

FReply SClaireonREPLWidget::OnNewTopicClicked()
{
	TriggerNewTopic(bFreshContextSuggested);
	return FReply::Handled();
}

FReply SClaireonREPLWidget::OnKeyDown(const FGeometry& MyGeometry,
	const FKeyEvent& InKeyEvent)
{
	// Escape = cancel (only during processing)
	if (InKeyEvent.GetKey() == EKeys::Escape && bIsProcessing)
	{
		if (Client.IsValid())
			Client->CancelActiveRequest();
		return FReply::Handled();
	}

	// Ctrl+Enter = New Topic
	if (InKeyEvent.GetKey() == EKeys::Enter && InKeyEvent.IsControlDown() && !bIsProcessing)
	{
		TriggerNewTopic(bFreshContextSuggested);
		return FReply::Handled();
	}

	// Up arrow = history navigate (only when input has focus and not processing)
	if (InKeyEvent.GetKey() == EKeys::Up && !bIsProcessing)
	{
		NavigateHistory(-1);
		return FReply::Handled();
	}
	if (InKeyEvent.GetKey() == EKeys::Down && !bIsProcessing)
	{
		NavigateHistory(1);
		return FReply::Handled();
	}

	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

bool SClaireonREPLWidget::HandleEscapeKey()
{
	if (bIsProcessing && Client.IsValid())
	{
		Client->CancelActiveRequest();
		return true;
	}
	return false;
}

void SClaireonREPLWidget::TriggerEmergencyStop()
{
	// Cancel REPL request
	if (Client.IsValid() && bIsProcessing)
	{
		Client->CancelActiveRequest();
	}
	// Activate server-level user stop
	if (Server)
	{
		Server->ActivateUserStop();
		if (Logger)
			Logger->LogEmergencyStop(CurrentConversationId);
	}
}

// ------- REPL Event Handler -------

void SClaireonREPLWidget::OnREPLEvent(const FREPLEvent& Event)
{
	switch (Event.Type)
	{
		case EREPLEventType::AssistantText:
		{
			FREPLConversationItem Item;
			Item.ItemType = FREPLConversationItem::EItemType::AssistantMessage;
			Item.Text = Event.Text;
			AppendConversationItem(MoveTemp(Item));
			break;
		}
		case EREPLEventType::ToolCallStarted:
		{
			// Update thinking-row step label with the tool being invoked
			if (ThinkingStepText.IsValid())
			{
				ThinkingStepText->SetText(FText::FromString(
					FString::Printf(TEXT("· %s"), *Event.ToolName)));
			}
			FREPLConversationItem Item;
			Item.ItemType = FREPLConversationItem::EItemType::ToolCard;
			Item.ToolName = Event.ToolName;
			Item.ToolUseId = Event.ToolUseId;
			Item.ToolArgsJson = Event.ToolArgsJson;
			Item.bToolRunning = true;
			AppendConversationItem(MoveTemp(Item));
			break;
		}
		case EREPLEventType::ToolCallCompleted:
		{
			FREPLConversationItem Updated;
			Updated.ItemType = FREPLConversationItem::EItemType::ToolCard;
			Updated.ToolName = Event.ToolName;
			Updated.ToolUseId = Event.ToolUseId;
			Updated.Text = Event.Text;
			Updated.ToolDurationMs = Event.DurationMs;
			Updated.bToolRunning = false;
			Updated.bToolError = Event.bIsError;
			UpdateToolCard(Event.ToolUseId, Updated);
			break;
		}
		case EREPLEventType::ToolCallCancelled:
		{
			FREPLConversationItem Updated;
			Updated.ItemType = FREPLConversationItem::EItemType::ToolCard;
			Updated.ToolName = Event.ToolName;
			Updated.ToolUseId = Event.ToolUseId;
			Updated.Text = TEXT("Cancelled");
			Updated.bToolRunning = false;
			Updated.bToolError = true;
			UpdateToolCard(Event.ToolUseId, Updated);
			break;
		}
		case EREPLEventType::Cancelled:
		{
			FREPLConversationItem Item;
			Item.ItemType = FREPLConversationItem::EItemType::SystemMessage;
			Item.Text = TEXT("[stopped]");
			Item.bIsError = true;
			AppendConversationItem(MoveTemp(Item));
			SetProcessingState(false);
			break;
		}
		case EREPLEventType::Error:
		{
			FREPLConversationItem Item;
			Item.ItemType = FREPLConversationItem::EItemType::SystemMessage;
			Item.Text = Event.Text;
			Item.bIsError = true;
			AppendConversationItem(MoveTemp(Item));
			SetProcessingState(false);
			break;
		}
		case EREPLEventType::Finished:
			if (ThinkingStepText.IsValid())
			{
				ThinkingStepText->SetText(FText::GetEmpty());
			}
			SetProcessingState(false);
			UpdateContextIndicator();
			break;

		case EREPLEventType::FreshContextSuggested:
			PendingFreshContextHandoff = Event.FreshContextHandoff;
			UpdateNewTopicButtonState(true, Event.FreshContextHandoff);
			break;

		case EREPLEventType::Retrying:
		{
			// Update thinking-row step label
			if (ThinkingStepText.IsValid())
			{
				ThinkingStepText->SetText(FText::FromString(
					FString::Printf(TEXT("· %s"), *Event.Text)));
			}
			FREPLConversationItem Item;
			Item.ItemType = FREPLConversationItem::EItemType::SystemMessage;
			Item.Text = Event.Text;
			Item.bIsError = false;
			AppendConversationItem(MoveTemp(Item));
			// Do NOT call SetProcessingState(false) — keep processing state active
			break;
		}
	}

	// Auto-scroll to bottom
	if (ConversationScroll.IsValid())
	{
		ConversationScroll->ScrollToEnd();
	}
}

// ------- State Helpers -------

void SClaireonREPLWidget::SetProcessingState(bool bProcessing)
{
	bIsProcessing = bProcessing;

	// The Send button's IsEnabled_Lambda reads bIsProcessing, so no explicit
	// widget update is needed.  The composer text field is kept editable so the
	// user can draft the next message while the response streams.

	if (bProcessing)
	{
		StartPulseAnimation();
		// Hold focus on the compound widget so Escape / Ctrl+. are captured
		FSlateApplication::Get().SetKeyboardFocus(AsShared(),
			EFocusCause::SetDirectly);
	}
	else
	{
		StopPulseAnimation();
		// Return focus to the composer text field
		if (InputBox.IsValid())
		{
			FSlateApplication::Get().SetKeyboardFocus(InputBox,
				EFocusCause::SetDirectly);
		}
	}
}

void SClaireonREPLWidget::CheckApiKeyAndSwitchView()
{
	const UClaireonSettings* Settings = UClaireonSettings::Get();
	bool bHasKey = Settings && Settings->HasAnthropicApiKey();
	if (RootSwitcher.IsValid())
	{
		RootSwitcher->SetActiveWidgetIndex(bHasKey ? 1 : 0);
	}

	// Add welcome message on first switch to chat
	if (bHasKey && ConversationItems.Num() == 0)
	{
		// Use the live module server pointer — not the stale constructor pointer,
		// which may be null if the tab was opened before the server was started.
		FClaireonServer* LiveServer =
			FClaireonModule::Get().GetServer();
		FREPLConversationItem WelcomeItem;
		WelcomeItem.ItemType = FREPLConversationItem::EItemType::SystemMessage;
		WelcomeItem.Text = (LiveServer && Client.IsValid())
			? FString::Printf(TEXT("Connected \u00b7 %d API tools (%d via bridge)"),
				  Client->GetAPIToolCount(), LiveServer->GetTools().Num())
			: TEXT("Connected \u00b7 Start the MCP server to enable tool use");
		AppendConversationItem(MoveTemp(WelcomeItem));
	}
}

void SClaireonREPLWidget::TriggerNewTopic(bool bWithHandoff)
{
	FString OldConversationId = CurrentConversationId;
	++ConversationCounter;
	CurrentConversationId = FString::Printf(TEXT("conv_%03d"), ConversationCounter);

	FString Handoff = bWithHandoff ? PendingFreshContextHandoff : FString();

	if (Logger)
	{
		Logger->LogFreshContext(OldConversationId, CurrentConversationId,
			bWithHandoff ? TEXT("user_claude_suggested") : TEXT("user_manual"), Handoff);
		Logger->ForceFlush();
	}

	if (Client.IsValid())
	{
		Client->ResetConversation();
	}

	// Clear conversation display
	ConversationItems.Empty();
	if (ConversationBox.IsValid())
	{
		ConversationBox->ClearChildren();
	}

	bFreshContextSuggested = false;
	PendingFreshContextHandoff.Empty();
	UpdateNewTopicButtonState(false, TEXT(""));

	// If handoff, inject it as first system message then send
	if (bWithHandoff && !Handoff.IsEmpty() && Client.IsValid())
	{
		FREPLConversationItem SysItem;
		SysItem.ItemType = FREPLConversationItem::EItemType::SystemMessage;
		SysItem.Text = TEXT("Starting fresh context with summary from previous conversation.");
		AppendConversationItem(MoveTemp(SysItem));

		// Send handoff as first user message
		Client->SendMessage(
			FString::Printf(TEXT("[System context from previous conversation]\n%s"), *Handoff),
			CurrentConversationId);
		SetProcessingState(true);
	}
	else
	{
		// Clean welcome
		if (Server)
		{
			FREPLConversationItem WelcomeItem;
			WelcomeItem.ItemType = FREPLConversationItem::EItemType::SystemMessage;
			WelcomeItem.Text = Client.IsValid()
				? FString::Printf(TEXT("New conversation \u00b7 %d API tools (%d via bridge)"),
					  Client->GetAPIToolCount(), Server->GetTools().Num())
				: FString::Printf(TEXT("New conversation \u00b7 %d tools available"),
					  Server->GetTools().Num());
			AppendConversationItem(MoveTemp(WelcomeItem));
		}
	}
}

void SClaireonREPLWidget::UpdateContextIndicator()
{
	if (!Client.IsValid())
		return;

	constexpr int32 ContextWindowTokens = 200000;
	const int32 Used = Client->GetApproximateTokenCount();
	const int32 NewPct = FMath::Clamp((Used * 100) / ContextWindowTokens, 0, 100);

	// Only mark dirty when the integer percentage changes to keep paint cost flat
	if (NewPct != ContextUsagePct)
	{
		ContextUsagePct = NewPct;
	}
}

void SClaireonREPLWidget::UpdateNewTopicButtonState(bool bHighlight,
	const FString& HandoffText)
{
	bFreshContextSuggested = bHighlight;
	if (NewTopicButton.IsValid())
	{
		NewTopicButton->SetToolTipText(bHighlight
				? FText::FromString(FString::Printf(
					  TEXT("Claude suggests starting fresh:\n%s\n(Ctrl+Enter)"), *HandoffText))
				: LOCTEXT("NewTopicTooltip", "Start a new conversation (Ctrl+Enter)"));
	}
}

// ------- Conversation Rendering -------

void SClaireonREPLWidget::AppendConversationItem(FREPLConversationItem&& Item)
{
	// Stamp creation time so it survives rebuilds
	if (Item.Timestamp.IsEmpty())
	{
		Item.Timestamp = FDateTime::Now().ToString(TEXT("%H:%M:%S"));
	}
	ConversationItems.Add(Item);
	if (ConversationBox.IsValid())
	{
		ConversationBox->AddSlot()
			.AutoHeight()
			.Padding(FMargin(0.0f))
				[BuildConversationItemWidget(ConversationItems.Last())];
	}
}

void SClaireonREPLWidget::UpdateToolCard(const FString& ToolUseId,
	const FREPLConversationItem& Updated)
{
	// Find and update the item in the data array
	for (FREPLConversationItem& Item : ConversationItems)
	{
		if (Item.ToolUseId == ToolUseId)
		{
			Item = Updated;
			break;
		}
	}
	// Rebuild display (simple approach — tool cards are few)
	RebuildConversationDisplay();
}

void SClaireonREPLWidget::RebuildConversationDisplay()
{
	if (!ConversationBox.IsValid())
		return;
	ConversationBox->ClearChildren();
	for (const FREPLConversationItem& Item : ConversationItems)
	{
		ConversationBox->AddSlot()
			.AutoHeight()
			.Padding(FMargin(0.0f))
				[BuildConversationItemWidget(Item)];
	}
}

// Small avatar chip: 16x16, rounded, "cl" (assistant) or "me" (user)
TSharedRef<SWidget> SClaireonREPLWidget::BuildAvatarWidget(bool bIsAssistant)
{
	const FLinearColor AvatarBg = bIsAssistant
		? ClaireonTokens::Hex(TEXT("E7873B"), 0.14f)
		: ClaireonTokens::Bg3;
	const FLinearColor AvatarBdr = bIsAssistant
		? ClaireonTokens::AccentLine
		: ClaireonTokens::Line2;
	const FLinearColor AvatarFg = bIsAssistant
		? ClaireonTokens::AccentHi
		: ClaireonTokens::Fg2;
	const FString Label = bIsAssistant ? TEXT("cl") : TEXT("me");

	return SNew(SBox)
		.WidthOverride(16.0f)
		.HeightOverride(16.0f)
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(AvatarBdr)
			.Padding(FMargin(1.0f))
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor(AvatarBg)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Label))
					.ColorAndOpacity(FSlateColor(AvatarFg))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 7))
				]
			]
		];
}

// Body content for a message: markdown for assistant, plain text for user/system
TSharedRef<SWidget> SClaireonREPLWidget::BuildMessageBodyContent(
	const FREPLConversationItem& Item)
{
	using EItemType = FREPLConversationItem::EItemType;

	if (Item.ItemType == EItemType::AssistantMessage
		&& UClaireonSettings::Get()->bEnableRichText)
	{
		return BuildAssistantMessageWidget(Item.Text);
	}

	const FLinearColor TextColor = (Item.ItemType == EItemType::UserMessage)
		? ClaireonTokens::Fg2
		: (Item.bIsError ? ClaireonTokens::Err : ClaireonTokens::Fg3);

	const FTextBlockStyle BodyStyle =
		ClaireonREPLWidgetInternal::MakeColoredTextStyle(TextColor);

	return SNew(SMultiLineEditableText)
		.Text(FText::FromString(Item.Text))
		.TextStyle(&BodyStyle)
		.AutoWrapText(true)
		.IsReadOnly(true);
}

TSharedRef<SWidget> SClaireonREPLWidget::BuildConversationItemWidget(
	const FREPLConversationItem& Item)
{
	using EItemType = FREPLConversationItem::EItemType;

	// Tool cards: render as an inline cm-tool block with a thin assistant header row
	if (Item.ItemType == EItemType::ToolCard)
	{
		return SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("NoBrush"))
			.Padding(FMargin(24.0f, 4.0f, 24.0f, 4.0f))
			[
				BuildToolCardWidget(Item)
			];
	}

	// System messages: minimal mono annotation (no avatar chrome)
	if (Item.ItemType == EItemType::SystemMessage)
	{
		return SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("NoBrush"))
			.Padding(FMargin(50.0f, 5.0f, 24.0f, 5.0f))
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item.Text))
				.ColorAndOpacity(FSlateColor(Item.bIsError ? ClaireonTokens::Err : ClaireonTokens::Fg3))
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 10))
				.AutoWrapText(true)
			];
	}

	const bool bIsAssistant = (Item.ItemType == EItemType::AssistantMessage);
	const FString AuthorText = bIsAssistant ? TEXT("Claireon") : TEXT("You");
	const FString TimestampText = Item.Timestamp;

	// We need the outer border ptr for hover-reveal of the tool buttons.
	// Use a shared holder so the lambda can reference it after SAssignNew runs.
	auto WeakHolder = MakeShared<TWeakPtr<SBorder>>();
	FString TextCapture = Item.Text;

	TSharedRef<SWidget> HoverTools =
		SNew(SBox)
		.Visibility_Lambda([WeakHolder]() -> EVisibility
		{
			TSharedPtr<SBorder> B = WeakHolder->Pin();
			return (B.IsValid() && B->IsHovered())
				? EVisibility::Visible
				: EVisibility::Hidden;
		})
		[
			SNew(SHorizontalBox)

			// Copy
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(LOCTEXT("CopyTooltip", "Copy message"))
				.OnClicked_Lambda([TextCapture]() -> FReply
				{
					FPlatformApplicationMisc::ClipboardCopy(*TextCapture);
					return FReply::Handled();
				})
				[
					SNew(STextBlock)
					.Text(LOCTEXT("CopyTool", "Copy"))
					.ColorAndOpacity(FSlateColor(ClaireonTokens::Fg3))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 11))
				]
			]

			// As feedback (assistant only)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Visibility(bIsAssistant ? EVisibility::Visible : EVisibility::Collapsed)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(LOCTEXT("AsFeedbackTooltip", "Open as feedback()"))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("AsFeedbackTool", "As feedback"))
					.ColorAndOpacity(FSlateColor(ClaireonTokens::Fg3))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 11))
				]
			]
		];

	TSharedRef<SWidget> Body = BuildMessageBodyContent(Item);

	// Full message: cm-head (avatar + author + ts + hover tools) + cm-body
	TSharedRef<SWidget> MessageContent =
		SNew(SVerticalBox)

		// cm-head
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, 6.0f))
		[
			SNew(SHorizontalBox)
			// Avatar
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 7.0f, 0.0f))
			[
				BuildAvatarWidget(bIsAssistant)
			]
			// Author name
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(AuthorText))
				.ColorAndOpacity(FSlateColor(ClaireonTokens::Fg1))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
			]
			// Timestamp
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			.Padding(FMargin(10.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(STextBlock)
				.Text(FText::FromString(TimestampText))
				.ColorAndOpacity(FSlateColor(ClaireonTokens::Fg3))
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 11))
			]
			// Spacer pushes hover tools right
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			// Hover-revealed tools
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				HoverTools
			]
		]

		// cm-body (indented under avatar rail: 23px = 16px avatar + 7px gap)
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(23.0f, 0.0f, 0.0f, 0.0f))
		[
			Body
		];

	// Outer hover-tinted border; fill pointer AFTER construction so the
	// WeakHolder lambda can resolve it at paint time.
	TSharedPtr<SBorder> OuterBorder;
	SAssignNew(OuterBorder, SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor_Lambda([WeakHolder]() -> FLinearColor
		{
			TSharedPtr<SBorder> B = WeakHolder->Pin();
			return (B.IsValid() && B->IsHovered())
				? ClaireonTokens::BgRowHover
				: FLinearColor::Transparent;
		})
		.Padding(FMargin(24.0f, 14.0f))
		[
			MessageContent
		];

	*WeakHolder = OuterBorder;
	return OuterBorder.ToSharedRef();
}

TSharedRef<SWidget> SClaireonREPLWidget::BuildToolCardWidget(
	const FREPLConversationItem& Item)
{
	// Status dot colour
	const FLinearColor DotColor = Item.bToolRunning  ? ClaireonTokens::Warn
		                        : Item.bToolError    ? ClaireonTokens::Err
		                                             : ClaireonTokens::Ok;

	// Method chip: python_execute -> mb.py (orange), anything else -> mb.ts (blue)
	const bool bIsPy = Item.ToolName.Contains(TEXT("python"));
	const FLinearColor ChipBg  = bIsPy ? ClaireonTokens::PyChipBg  : ClaireonTokens::TsChipBg;
	const FLinearColor ChipBdr = bIsPy ? ClaireonTokens::PyChipBdr : ClaireonTokens::TsChipBdr;
	const FLinearColor ChipFg  = bIsPy ? ClaireonTokens::AccentHi  : ClaireonTokens::TsText;
	const FString ChipLabel    = bIsPy ? TEXT("py_exec") : TEXT("search");

	// Status text and duration
	const FString StatusLabel = Item.bToolRunning ? TEXT("running...")
		: Item.bToolError ? TEXT("error") : TEXT("completed");
	const FString DurationLabel = Item.bToolRunning
		? FString()
		: FString::Printf(TEXT("%.0fms"), Item.ToolDurationMs);

	// Args and output wells (read-only, selectable)
	const FSlateFontInfo MonoFont11 = FCoreStyle::GetDefaultFontStyle("Mono", 10);
	const FTextBlockStyle ArgsStyle =
		ClaireonREPLWidgetInternal::MakeColoredTextStyle(ClaireonTokens::Fg2, &MonoFont11);
	const FTextBlockStyle OutStyle =
		ClaireonREPLWidgetInternal::MakeColoredTextStyle(
			Item.bToolError ? ClaireonTokens::ErrHi : ClaireonTokens::Ok, &MonoFont11);

	TSharedRef<SVerticalBox> CardBody = SNew(SVerticalBox);

	// --- Header row ---
	CardBody->AddSlot().AutoHeight()
	[
		SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor(ClaireonTokens::Bg2)
		.Padding(FMargin(10.0f, 6.0f))
		[
			SNew(SHorizontalBox)

			// Status dot (6x6)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SNew(SBox).WidthOverride(6.0f).HeightOverride(6.0f)
				[
					SNew(SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
					.BorderBackgroundColor(DotColor)
				]
			]

			// Method chip (mb.py / mb.ts)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor(ChipBdr)
				.Padding(FMargin(1.0f))
				[
					SNew(SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
					.BorderBackgroundColor(ChipBg)
					.Padding(FMargin(5.0f, 2.0f))
					[
						SNew(STextBlock)
						.Text(FText::FromString(ChipLabel))
						.ColorAndOpacity(FSlateColor(ChipFg))
						.Font(FCoreStyle::GetDefaultFontStyle("Mono", 10))
					]
				]
			]

			// Status text
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(StatusLabel))
				.ColorAndOpacity(FSlateColor(ClaireonTokens::Fg3))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 11))
			]

			// Right-align duration
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(DurationLabel))
				.ColorAndOpacity(FSlateColor(ClaireonTokens::Fg3))
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 11))
			]
		]
	];

	// --- Args well (optional) ---
	if (!Item.ToolArgsJson.IsEmpty())
	{
		CardBody->AddSlot().AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(ClaireonTokens::Bg3)
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 1.0f))  // bottom hairline
			[
				// Cap height at ~90px via SBox max
				SNew(SBox).MaxDesiredHeight(90.0f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("NoBrush"))
						.Padding(FMargin(12.0f, 8.0f))
						[
							SNew(SMultiLineEditableText)
							.Text(FText::FromString(Item.ToolArgsJson))
							.TextStyle(&ArgsStyle)
							.AutoWrapText(false)
							.IsReadOnly(true)
						]
					]
				]
			]
		];
	}

	// --- Output well (optional) ---
	if (!Item.Text.IsEmpty() && !Item.bToolRunning)
	{
		const FLinearColor OutBg = Item.bToolError ? ClaireonTokens::ErrSoft : ClaireonTokens::OkSoft;
		CardBody->AddSlot().AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(OutBg)
			.Padding(FMargin(12.0f, 8.0f))
			[
				SNew(SMultiLineEditableText)
				.Text(FText::FromString(Item.Text))
				.TextStyle(&OutStyle)
				.AutoWrapText(true)
				.IsReadOnly(true)
			]
		];
	}

	// Outer card border: bg-3, line-1 stroke
	return SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor(ClaireonTokens::Line1)
		.Padding(FMargin(1.0f))
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(ClaireonTokens::Bg3)
			.Padding(FMargin(0.0f))
			[
				CardBody
			]
		];
}

// ------- Rich Text Rendering (Hybrid Vertical Box) -------

TSharedRef<SWidget> SClaireonREPLWidget::BuildAssistantMessageWidget(
	const FString& InMarkdown)
{
	TArray<FClaireonMarkdownBlock> Blocks = FClaireonMarkdownParser::ParseToBlocks(InMarkdown);

	// Fallback: if parsing produces nothing, render as plain text
	if (Blocks.Num() == 0)
	{
		const FTextBlockStyle PlainStyle = ClaireonREPLWidgetInternal::MakeColoredTextStyle(ClaireonTokens::Fg1);
		return SNew(SMultiLineEditableText)
			.Text(FText::FromString(InMarkdown))
			.TextStyle(&PlainStyle)
			.AutoWrapText(true)
			.IsReadOnly(true);
	}

	TSharedRef<SVerticalBox> MessageContent = SNew(SVerticalBox);

	for (const FClaireonMarkdownBlock& Block : Blocks)
	{
		switch (Block.Type)
		{
			case EClaireonBlockType::Prose:
			{
				TSharedRef<FClaireonMarkdownMarshaller> Marshaller =
					FClaireonMarkdownMarshaller::Create(
						Block.Lines, FClaireonRichTextStyle::Get());

				const FTextBlockStyle& DefaultTextStyle =
					FClaireonRichTextStyle::Get().GetWidgetStyle<FTextBlockStyle>(
						TEXT("RichText.Default"));

				// Holder allows the context menu lambda to safely access the widget
				// after construction (SAssignNew sets the pointer after the builder chain)
				auto WidgetHolder = MakeShared<TWeakPtr<SMultiLineEditableText>>();
				TSharedPtr<SMultiLineEditableText> ProseTextWidget;

				TWeakPtr<SClaireonREPLWidget> WeakSelf =
					StaticCastSharedRef<SClaireonREPLWidget>(AsShared());

				MessageContent->AddSlot()
					.AutoHeight()
					.Padding(0.0f, 2.0f)
					[SAssignNew(ProseTextWidget, SMultiLineEditableText)
							.Marshaller(Marshaller)
							.TextStyle(&DefaultTextStyle)
							.IsReadOnly(true)
							.AutoWrapText(true)
							.ContextMenuExtender_Lambda(
								[WeakSelf, WidgetHolder](FMenuBuilder& MenuBuilder)
				{
					TSharedPtr<SClaireonREPLWidget> Self = WeakSelf.Pin();
					TSharedPtr<SMultiLineEditableText> Widget = WidgetHolder->Pin();
					if (!Self.IsValid() || !Widget.IsValid())
					{
						return;
					}
					Self->OnMessageContextMenu(MenuBuilder, Widget.ToSharedRef());
				})];

				*WidgetHolder = ProseTextWidget;
				break;
			}
			case EClaireonBlockType::CodeBlock:
			{
				MessageContent->AddSlot()
					.AutoHeight()
					.Padding(0.0f, 6.0f)
						[BuildCodeBlockWidget(Block.CodeContent, Block.CodeLanguage)];
				break;
			}
			case EClaireonBlockType::Separator:
			{
				MessageContent->AddSlot()
					.AutoHeight()
					.Padding(0.0f, 6.0f)
						[SNew(SSeparator)
								.Orientation(Orient_Horizontal)
								.SeparatorImage(FAppStyle::GetBrush("ThinLine.Horizontal"))
								.Thickness(1.0f)];
				break;
			}
			case EClaireonBlockType::Table:
			{
				MessageContent->AddSlot()
					.AutoHeight()
					.Padding(0.0f, 6.0f)
						[BuildTableWidget(Block)];
				break;
			}
		}
	}

	return MessageContent;
}

TSharedRef<SWidget> SClaireonREPLWidget::BuildCodeBlockWidget(
	const FString& InCode, const FString& InLanguage)
{
	// cm-code style: mono, fg-1, bg-3 background, line-1 border
	const FSlateFontInfo MonoFont = FCoreStyle::GetDefaultFontStyle("Mono", 10);
	FTextBlockStyle CodeBlockStyle =
		ClaireonREPLWidgetInternal::MakeColoredTextStyle(ClaireonTokens::Fg1, &MonoFont);

	FString CodeCapture = InCode;

	return SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor(ClaireonTokens::Line1)
		.Padding(FMargin(1.0f))
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(ClaireonTokens::Bg3)
			.Padding(FMargin(0.0f))
			[
				SNew(SVerticalBox)

				// Header: optional language label + copy button
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
					.BorderBackgroundColor(ClaireonTokens::Bg2)
					.Padding(FMargin(12.0f, 5.0f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(FText::FromString(InLanguage))
							.ColorAndOpacity(FSlateColor(ClaireonTokens::Fg3))
							.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
							.Visibility(InLanguage.IsEmpty()
								? EVisibility::Collapsed : EVisibility::Visible)
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(SButton)
							.ButtonStyle(FAppStyle::Get(), "SimpleButton")
							.ToolTipText(LOCTEXT("CopyCodeTooltip", "Copy code"))
							.OnClicked_Lambda([CodeCapture]() -> FReply
							{
								FPlatformApplicationMisc::ClipboardCopy(*CodeCapture);
								return FReply::Handled();
							})
							[
								SNew(STextBlock)
								.Text(LOCTEXT("CopyCode", "Copy"))
								.ColorAndOpacity(FSlateColor(ClaireonTokens::Fg3))
								.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
							]
						]
					]
				]

				// Code content (read-only, selectable for copy-paste)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("NoBrush"))
					.Padding(FMargin(12.0f, 10.0f))
					[
						SNew(SMultiLineEditableText)
						.Text(FText::FromString(InCode))
						.TextStyle(&CodeBlockStyle)
						.AutoWrapText(false)
						.IsReadOnly(true)
					]
				]
			]
		];
}

TSharedRef<SWidget> SClaireonREPLWidget::BuildStyledTableCell(
	const FString& InCellText, const FSlateFontInfo& InFont,
	const FLinearColor& InDefaultColor)
{
	FClaireonStyledLine Styled =
		FClaireonMarkdownParser::ParseInlineSegments(InCellText, TEXT("RichText.Default"));

	if (Styled.Segments.Num() == 0)
	{
		return SNew(STextBlock)
			.Text(FText::FromString(Styled.Text))
			.ColorAndOpacity(FSlateColor(InDefaultColor))
			.Font(InFont)
			.AutoWrapText(true);
	}

	// Build styled runs — handles both single and multi-segment cells
	TSharedRef<SHorizontalBox> HBox = SNew(SHorizontalBox);
	for (const FClaireonTextSegment& Seg : Styled.Segments)
	{
		const FString SegText = Styled.Text.Mid(
			Seg.StartIndex, Seg.EndIndex - Seg.StartIndex);

		FSlateFontInfo SegFont = InFont;
		FLinearColor SegColor = InDefaultColor;

		if (Seg.StyleName.Contains(TEXT("Code")))
		{
			SegFont = FCoreStyle::GetDefaultFontStyle("Mono", 9);
			SegColor = ClaireonTokens::AccentHi;
		}
		else if (Seg.StyleName.Contains(TEXT("Bold")))
		{
			SegFont = FCoreStyle::GetDefaultFontStyle("Bold", InFont.Size);
		}
		else if (Seg.StyleName.Contains(TEXT("AssetLink")))
		{
			SegColor = ClaireonTokens::AccentHi;
		}

		HBox->AddSlot().AutoWidth()
			[SNew(STextBlock)
					.Text(FText::FromString(SegText))
					.ColorAndOpacity(FSlateColor(SegColor))
					.Font(SegFont)];
	}
	return HBox;
}

TSharedRef<SWidget> SClaireonREPLWidget::BuildTableWidget(
	const FClaireonMarkdownBlock& Block)
{
	const int32 NumCols = Block.TableHeaders.Num();
	if (NumCols == 0)
	{
		return SNullWidget::NullWidget;
	}

	// Neutral dark grays matching the Unreal editor's Data Table style
	const FLinearColor HeaderBg(0.14f, 0.14f, 0.14f);
	const FLinearColor EvenRowBg(0.04f, 0.04f, 0.04f);
	const FLinearColor OddRowBg(0.07f, 0.07f, 0.07f);
	const FLinearColor BorderColor(0.16f, 0.16f, 0.16f);
	const FSlateFontInfo HeaderFont = FCoreStyle::GetDefaultFontStyle("Bold", 9);
	const FSlateFontInfo CellFont = FCoreStyle::GetDefaultFontStyle("Regular", 9);

	// Compute fixed pixel width per column to guarantee content fits.
	// Bold 9pt ~7.5px/char, Regular 9pt ~6.5px/char, plus cell padding.
	constexpr float BoldCharWidth = 7.5f;
	constexpr float RegularCharWidth = 6.5f;
	constexpr float CellPadH = 10.0f + 16.0f; // left + right padding

	TArray<float> ColPixelWidths;
	ColPixelWidths.SetNumZeroed(NumCols);
	for (int32 Col = 0; Col < NumCols; ++Col)
	{
		const int32 HeaderLen = Block.TableHeaders.IsValidIndex(Col)
			? Block.TableHeaders[Col].Len() : 0;
		const float HeaderPixels = HeaderLen * BoldCharWidth + CellPadH;

		float MaxDataPixels = 0.0f;
		for (const TArray<FString>& Row : Block.TableRows)
		{
			if (Row.IsValidIndex(Col))
			{
				MaxDataPixels = FMath::Max(MaxDataPixels,
					Row[Col].Len() * RegularCharWidth + CellPadH);
			}
		}

		// Column must fit both the header and the widest data cell
		ColPixelWidths[Col] = FMath::Max(HeaderPixels, MaxDataPixels);
	}

	auto BuildRow = [&](const TArray<FString>& CellTexts,
		const FLinearColor& RowBg, const FSlateFontInfo& Font,
		const FLinearColor& TextColor) -> TSharedRef<SWidget>
	{
		TSharedRef<SHorizontalBox> RowBox = SNew(SHorizontalBox);
		for (int32 Col = 0; Col < NumCols; ++Col)
		{
			const FString& CellText =
				CellTexts.IsValidIndex(Col) ? CellTexts[Col] : FString();

			RowBox->AddSlot()
				.AutoWidth()
				[SNew(SBox)
						.WidthOverride(ColPixelWidths[Col])
					[SNew(SBorder)
							.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
							.BorderBackgroundColor(RowBg)
							.Padding(FMargin(10.0f, 4.0f, 16.0f, 4.0f))
							.Clipping(EWidgetClipping::ClipToBounds)
								[BuildStyledTableCell(CellText, Font, TextColor)]]];
		}
		return RowBox;
	};

	TSharedRef<SVerticalBox> Table = SNew(SVerticalBox);

	// Outer border to frame the table
	TSharedRef<SBorder> TableBorder = SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor(BorderColor)
		.Padding(FMargin(1.0f))
			[Table];

	// Header row
	Table->AddSlot()
		.AutoHeight()
		[BuildRow(Block.TableHeaders, HeaderBg, HeaderFont, ClaireonTokens::Fg1)];

	// Data rows
	for (int32 RowIdx = 0; RowIdx < Block.TableRows.Num(); ++RowIdx)
	{
		const FLinearColor& RowBg = (RowIdx % 2 == 0) ? EvenRowBg : OddRowBg;
		Table->AddSlot()
			.AutoHeight()
			[BuildRow(Block.TableRows[RowIdx], RowBg, CellFont, ClaireonTokens::Fg2)];
	}

	// Wrap in horizontal scrollbox for wide tables
	return SNew(SScrollBox)
		.Orientation(Orient_Horizontal)
		+ SScrollBox::Slot()
			[TableBorder];
}

void SClaireonREPLWidget::OnMessageContextMenu(
	FMenuBuilder& MenuBuilder, TSharedRef<SMultiLineEditableText> InTextWidget)
{
	const FString SelectedText = InTextWidget->GetSelectedText().ToString().TrimStartAndEnd();
	if (SelectedText.IsEmpty())
	{
		return;
	}

	// Check if the selected text looks like an asset path
	const FRegexPattern AssetPathPattern(
		TEXT("^/(Game|Script|Engine|FS[A-Za-z]+|Plugins)/[A-Za-z0-9_/.]+$"));
	FRegexMatcher Matcher(AssetPathPattern, SelectedText);

	if (Matcher.FindNext())
	{
		MenuBuilder.AddMenuEntry(
			FText::Format(
				LOCTEXT("OpenInContentBrowser", "Open in Content Browser: {0}"),
				FText::FromString(FPackageName::GetShortName(SelectedText))),
			FText::FromString(SelectedText),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSP(
				this, &SClaireonREPLWidget::NavigateToAssetPath, SelectedText)));
	}
}

void SClaireonREPLWidget::NavigateToAssetPath(FString AssetPath)
{
	IAssetRegistry& AssetRegistry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
	if (!AssetData.IsValid())
	{
		// Try with PackageName.ShortName format
		const FString FullObjectPath =
			AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(FullObjectPath));
	}

	if (AssetData.IsValid())
	{
		FContentBrowserModule& ContentBrowserModule =
			FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		TArray<FAssetData> Assets;
		Assets.Add(AssetData);
		ContentBrowserModule.Get().SyncBrowserToAssets(Assets);
	}
	else
	{
		// Fallback: try syncing to the folder containing the path
		FContentBrowserModule& ContentBrowserModule =
			FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
		TArray<FString> Folders;
		Folders.Add(FPackageName::GetLongPackagePath(AssetPath));
		ContentBrowserModule.Get().SyncBrowserToFolders(Folders);
	}
}

// ------- Status Bar Bindings -------

FText SClaireonREPLWidget::GetStatusText() const
{
	if (bUserStopActive)
		return LOCTEXT("StatusStopped", "All operations stopped");
	if (!bIsProcessing)
		return FText::FromString(
			FString::Printf(TEXT("Idle \u00b7 %s"),
				*UClaireonSettings::Get()->ModelId));
	return LOCTEXT("StatusThinking", "Working\u2026");
}

FSlateColor SClaireonREPLWidget::GetStatusDotColor() const
{
	if (bUserStopActive)
		return FSlateColor(ClaireonTokens::Err);
	if (!bIsProcessing)
		return FSlateColor(ClaireonTokens::Fg3);
	float Pulse = PulseSequence.IsPlaying()
		? FMath::Lerp(0.3f, 1.0f, PulseHandle.GetLerp())
		: 1.0f;
	return FSlateColor(FLinearColor(
		ClaireonTokens::Accent.R,
		ClaireonTokens::Accent.G,
		ClaireonTokens::Accent.B,
		Pulse));
}

FText SClaireonREPLWidget::GetContextIndicatorText() const
{
	return FText::FromString(FString::Printf(TEXT("ctx %d%%"), ContextUsagePct));
}

FSlateColor SClaireonREPLWidget::GetContextIndicatorColor() const
{
	if (ContextUsagePct >= 90) return FSlateColor(ClaireonTokens::Err);
	if (ContextUsagePct >= 70) return FSlateColor(ClaireonTokens::Warn);
	return FSlateColor(ClaireonTokens::Fg3);
}

FText SClaireonREPLWidget::GetNewTopicButtonText() const
{
	return bFreshContextSuggested
		? LOCTEXT("NewTopicHighlighted", "* New Topic")
		: LOCTEXT("NewTopicNormal", "New Topic");
}

FSlateColor SClaireonREPLWidget::GetNewTopicButtonColor() const
{
	if (bFreshContextSuggested)
	{
		float Pulse = PulseSequence.IsPlaying()
			? FMath::Lerp(0.4f, 1.0f, PulseHandle.GetLerp())
			: 1.0f;
		return FSlateColor(FLinearColor(
			ClaireonTokens::Accent.R,
			ClaireonTokens::Accent.G,
			ClaireonTokens::Accent.B,
			Pulse));
	}
	return FSlateColor(ClaireonTokens::Fg2);
}

// ------- Animation -------

void SClaireonREPLWidget::StartPulseAnimation()
{
	if (!PulseSequence.IsPlaying())
	{
		PulseSequence.Play(AsShared(), true); // loop
	}
}

void SClaireonREPLWidget::StopPulseAnimation()
{
	if (PulseSequence.IsPlaying())
	{
		PulseSequence.JumpToEnd();
	}
}

float SClaireonREPLWidget::GetPulseOpacity() const
{
	if (!PulseSequence.IsPlaying())
		return 1.0f;
	return FMath::Lerp(0.3f, 1.0f, PulseHandle.GetLerp());
}

// ------- Input History -------

void SClaireonREPLWidget::PushInputHistory(const FString& Text)
{
	if (!Text.IsEmpty())
	{
		InputHistory.Insert(Text, 0); // newest first
		if (InputHistory.Num() > 50)
			InputHistory.SetNum(50);
	}
}

void SClaireonREPLWidget::NavigateHistory(int32 Delta)
{
	if (InputHistory.Num() == 0)
		return;

	HistoryCursor = FMath::Clamp(HistoryCursor + Delta, -1, InputHistory.Num() - 1);

	if (!InputBox.IsValid())
		return;
	if (HistoryCursor < 0)
	{
		InputBox->SetText(FText::GetEmpty());
	}
	else
	{
		InputBox->SetText(FText::FromString(InputHistory[HistoryCursor]));
	}
}

#undef LOCTEXT_NAMESPACE
