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

// --- Color palette ---
// (FLinearColor constants used throughout)
static const FLinearColor Color_Lavender(0.8f, 0.8f, 1.0f);	  // Assistant text
static const FLinearColor Color_Amber(0.9f, 0.7f, 0.3f);	  // Tool cards, CTA
static const FLinearColor Color_Green(0.1f, 0.8f, 0.1f);	  // Active/running
static const FLinearColor Color_Red(1.0f, 0.3f, 0.3f);		  // Error/cancel/stop
static const FLinearColor Color_Gray_Mid(0.5f, 0.5f, 0.5f);	  // Subdued text
static const FLinearColor Color_Gray_Light(0.7f, 0.7f, 0.7f); // Secondary text
static const FLinearColor Color_White(1.0f, 1.0f, 1.0f);	  // User text
static const FLinearColor Color_DarkBg(0.12f, 0.12f, 0.14f);  // Message bg

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
			  [SNew(SBorder)
					  .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
					  .Padding(32.0f)
						  [SNew(SVerticalBox)

							  // Title
							  + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 16)
								  [SNew(STextBlock)
										  .Text(LOCTEXT("OnboardingTitle", "Set up Claude"))
										  .ColorAndOpacity(FSlateColor(Color_Amber))
										  .Font(FCoreStyle::GetDefaultFontStyle("Bold", 16))]

							  // Subtitle
							  + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 24)
								  [SNew(STextBlock)
										  .Text(LOCTEXT("OnboardingSubtitle",
											  "Ask questions, search assets, run scripts — all from here."))
										  .ColorAndOpacity(FSlateColor(Color_Gray_Light))
										  .AutoWrapText(true)]

							  // Step 1
							  + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
								  [SNew(STextBlock)
										  .Text(LOCTEXT("Step1", "1. Get an Anthropic API key"))
										  .ColorAndOpacity(FSlateColor(Color_White))]
							  + SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 16)
								  [SNew(SButton)
										  .Text(LOCTEXT("GetKeyButton", "Open console.anthropic.com ->"))
										  .OnClicked(this, &SClaireonREPLWidget::OnGetApiKeyClicked)
										  .ButtonColorAndOpacity(FLinearColor(Color_Amber.R * 0.3f,
											  Color_Amber.G * 0.3f, Color_Amber.B * 0.1f, 1.0f))]

							  // Step 2
							  + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
								  [SNew(STextBlock)
										  .Text(LOCTEXT("Step2", "2. Paste your API key here"))
										  .ColorAndOpacity(FSlateColor(Color_White))]
							  + SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 16)
								  [SNew(SHorizontalBox)
									  + SHorizontalBox::Slot().FillWidth(1.0f)
										  [SAssignNew(ApiKeyInputBox, SEditableTextBox)
												  .HintText(LOCTEXT("ApiKeyHint", "sk-ant-..."))
												  .IsPassword(true)]]

							  // Step 3 / Verify button
							  + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
								  [SNew(STextBlock)
										  .Text(LOCTEXT("Step3", "3. Verify and save"))
										  .ColorAndOpacity(FSlateColor(Color_White))]
							  + SVerticalBox::Slot().AutoHeight().Padding(16, 0, 0, 0)
								  [SNew(SButton)
										  .Text(LOCTEXT("VerifyButton", "Verify & Save"))
										  .OnClicked(this, &SClaireonREPLWidget::OnVerifyAndSaveClicked)
										  .ButtonColorAndOpacity(Color_Amber)]

							  // Status message
							  + SVerticalBox::Slot().AutoHeight().Padding(0, 16, 0, 0)
								  [SNew(STextBlock)
										  .Text(this, &SClaireonREPLWidget::GetOnboardingStatusText)
										  .AutoWrapText(true)
										  .ColorAndOpacity(FSlateColor(Color_Gray_Light))]]];
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
				Settings->AnthropicApiKey = KeyCapture;
				Settings->SaveConfig();

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
	return SNew(SVerticalBox)

		// Conversation scroll area
		+ SVerticalBox::Slot().FillHeight(1.0f)
			  [SAssignNew(ConversationScroll, SScrollBox)
					  .OnUserScrolled(FOnUserScrolled::CreateLambda([this](float)
	{
		// Auto-scroll is managed by scroll position check
	})) + SScrollBox::Slot()[SAssignNew(ConversationBox, SVerticalBox)]]

		// Input area
		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 4.0f, 4.0f, 0.0f)[SNew(SHorizontalBox)

			+ SHorizontalBox::Slot().FillWidth(1.0f)[SAssignNew(InputBox, SMultiLineEditableTextBox).HintText(LOCTEXT("InputHint", "Ask something...  (Shift+Enter for newline)")).OnTextCommitted(this, &SClaireonREPLWidget::OnInputTextCommitted).ModiferKeyForNewLine(EModifierKey::Shift).AllowMultiLine(true)]

			// New Topic button (always visible, highlighted when fresh context suggested)
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f).VAlign(VAlign_Bottom)[SAssignNew(NewTopicButton, SButton).Text(this, &SClaireonREPLWidget::GetNewTopicButtonText).ButtonColorAndOpacity(this, &SClaireonREPLWidget::GetNewTopicButtonColor).OnClicked(this, &SClaireonREPLWidget::OnNewTopicClicked).ToolTipText(LOCTEXT("NewTopicTooltip", "Start a new conversation (Ctrl+Enter)"))]

			// Send / Stop switcher
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f).VAlign(VAlign_Bottom)[SAssignNew(SendStopSwitcher, SWidgetSwitcher)
				// index 0: Send
				+ SWidgetSwitcher::Slot()[SNew(SButton).Text(LOCTEXT("SendButton", "Send")).OnClicked(this, &SClaireonREPLWidget::OnSendClicked).ButtonColorAndOpacity(Color_Amber).ToolTipText(LOCTEXT("SendTooltip", "Send message (Enter)"))]
				// index 1: Stop
				+ SWidgetSwitcher::Slot()[SNew(SButton).Text(LOCTEXT("StopButton", "Stop")).OnClicked(this, &SClaireonREPLWidget::OnStopClicked).ButtonColorAndOpacity(Color_Red).ToolTipText(LOCTEXT("StopTooltip", "Cancel request (Escape or Ctrl+.)"))]]]

		// Status bar
		+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 2.0f)[SNew(SHorizontalBox)

			// Pulse dot
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 4.0f, 0.0f).VAlign(VAlign_Center)[SNew(SBorder).Padding(4.0f).BorderImage(FAppStyle::GetBrush("NoBrush")).ColorAndOpacity_Lambda([this]() -> FLinearColor
	{
		return GetStatusDotColor().GetSpecifiedColor();
	})[SNew(SBox).WidthOverride(8.0f).HeightOverride(8.0f)]]

			// Status text
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[SAssignNew(StatusText, STextBlock).Text(this, &SClaireonREPLWidget::GetStatusText).ColorAndOpacity(FSlateColor(Color_Gray_Mid))]

			+ SHorizontalBox::Slot().FillWidth(1.0f)[SNew(SSpacer)]

			// Context indicator
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4.0f, 0.0f)[SAssignNew(ContextIndicatorText, STextBlock).Text(this, &SClaireonREPLWidget::GetContextIndicatorText).ColorAndOpacity(this, &SClaireonREPLWidget::GetContextIndicatorColor)]

			// Settings gear
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f)[SNew(SButton).Text(LOCTEXT("SettingsGear", "\u2699")).ButtonStyle(FAppStyle::Get(), "SimpleButton").OnClicked_Lambda([]() -> FReply
	{
		FModuleManager::LoadModuleChecked<ISettingsModule>("Settings")
			.ShowViewer(TEXT("Editor"), TEXT("Plugins"), TEXT("Claireon"));
		return FReply::Handled();
	}).ToolTipText(LOCTEXT("SettingsTooltip", "Open Claireon settings"))]];
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
			SetProcessingState(false);
			UpdateContextIndicator();
			break;

		case EREPLEventType::FreshContextSuggested:
			PendingFreshContextHandoff = Event.FreshContextHandoff;
			UpdateNewTopicButtonState(true, Event.FreshContextHandoff);
			break;

		case EREPLEventType::Retrying:
		{
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

	if (InputBox.IsValid())
	{
		InputBox->SetEnabled(!bProcessing);
	}
	if (SendStopSwitcher.IsValid())
	{
		SendStopSwitcher->SetActiveWidgetIndex(bProcessing ? 1 : 0);
	}

	if (bProcessing)
	{
		StartPulseAnimation();
		// Return keyboard focus to widget (not input box) so Escape works
		FSlateApplication::Get().SetKeyboardFocus(AsShared(),
			EFocusCause::SetDirectly);
	}
	else
	{
		StopPulseAnimation();
		// Return focus to input box
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
	bool bHasKey = Settings && !Settings->AnthropicApiKey.IsEmpty();
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
	if (Client.IsValid() && ContextIndicatorText.IsValid())
	{
		// Widget will re-bind automatically
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
	ConversationItems.Add(Item);
	if (ConversationBox.IsValid())
	{
		ConversationBox->AddSlot()
			.AutoHeight()
			.Padding(4.0f, 2.0f)
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
			.Padding(4.0f, 2.0f)
				[BuildConversationItemWidget(Item)];
	}
}

TSharedRef<SWidget> SClaireonREPLWidget::BuildConversationItemWidget(
	const FREPLConversationItem& Item)
{
	using EItemType = FREPLConversationItem::EItemType;

	if (Item.ItemType == EItemType::ToolCard)
	{
		return BuildToolCardWidget(Item);
	}

	// Determine color and label prefix
	FLinearColor TextColor = Color_White;
	FString Label;

	switch (Item.ItemType)
	{
		case EItemType::UserMessage:
			Label = TEXT("You");
			TextColor = Color_White;
			break;
		case EItemType::AssistantMessage:
			Label = TEXT("Claude");
			TextColor = Color_Lavender;
			break;
		case EItemType::SystemMessage:
			Label.Empty();
			TextColor = Item.bIsError ? Color_Red : Color_Gray_Mid;
			break;
		default:
			break;
	}

	TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);

	if (!Label.IsEmpty())
	{
		if (Item.ItemType == EItemType::AssistantMessage)
		{
			// Assistant messages: label on left, copy button on right
			FString RawMarkdown = Item.Text;
			Content->AddSlot().AutoHeight()
				[SNew(SHorizontalBox)

					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
						[SNew(STextBlock)
								.Text(FText::FromString(Label))
								.ColorAndOpacity(FSlateColor(Color_Gray_Light))
								.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))]

					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[SNew(SButton)
								.Text(LOCTEXT("CopyResponse", "Copy"))
								.ButtonStyle(FAppStyle::Get(), "SimpleButton")
								.TextStyle(FAppStyle::Get(), "SmallButtonText")
								.OnClicked_Lambda([RawMarkdown]() -> FReply
						{
							FPlatformApplicationMisc::ClipboardCopy(*RawMarkdown);
							return FReply::Handled();
						})]];
		}
		else
		{
			Content->AddSlot().AutoHeight()
				[SNew(STextBlock)
						.Text(FText::FromString(Label))
						.ColorAndOpacity(FSlateColor(Color_Gray_Light))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))];
		}
	}

	const bool bRichText = UClaireonSettings::Get()->bEnableRichText;
	if (Item.ItemType == EItemType::AssistantMessage && bRichText)
	{
		Content->AddSlot().AutoHeight()
			[BuildAssistantMessageWidget(Item.Text)];
	}
	else
	{
		// User messages, system messages: keep SMultiLineEditableText
		const FTextBlockStyle MessageTextStyle = ClaireonREPLWidgetInternal::MakeColoredTextStyle(TextColor);
		Content->AddSlot().AutoHeight()
			[SNew(SMultiLineEditableText)
					.Text(FText::FromString(Item.Text))
					.TextStyle(&MessageTextStyle)
					.AutoWrapText(true)
					.IsReadOnly(true)];
	}

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f, 4.0f)
			[Content];
}

TSharedRef<SWidget> SClaireonREPLWidget::BuildToolCardWidget(
	const FREPLConversationItem& Item)
{
	FLinearColor AccentColor = Item.bToolError ? Color_Red : Color_Amber;
	FLinearColor StatusColor = Item.bToolRunning ? Color_Green
												 : (Item.bToolError ? Color_Red : Color_Green);

	FString HeaderText = Item.bToolRunning
		? FString::Printf(TEXT("> %s"), *Item.ToolName)
		: FString::Printf(TEXT("%s %s (%.0fms)"),
			  Item.bToolError ? TEXT("x") : TEXT("ok"),
			  *Item.ToolName,
			  Item.ToolDurationMs);

	FString ToolStatusText = Item.bToolRunning
		? TEXT("Running\u2026")
		: (Item.bToolError ? TEXT("Error") : Item.Text.Left(200));

	// SMultiLineEditableText uses TextStyle instead of ColorAndOpacity.
	// Store locally; Slate copies the style data during SNew construction.
	const FSlateFontInfo SmallRegularFont = FCoreStyle::GetDefaultFontStyle("Regular", 9);
	const FTextBlockStyle ToolStatusTextStyle = ClaireonREPLWidgetInternal::MakeColoredTextStyle(StatusColor, &SmallRegularFont);

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f, 4.0f)
			[SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
					[SNew(STextBlock)
							.Text(FText::FromString(HeaderText))
							.ColorAndOpacity(FSlateColor(AccentColor))
							.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[SNew(SMultiLineEditableText)
							.Text(FText::FromString(ToolStatusText))
							.TextStyle(&ToolStatusTextStyle)
							.AutoWrapText(true)
							.IsReadOnly(true)]];
}

// ------- Rich Text Rendering (Hybrid Vertical Box) -------

TSharedRef<SWidget> SClaireonREPLWidget::BuildAssistantMessageWidget(
	const FString& InMarkdown)
{
	TArray<FClaireonMarkdownBlock> Blocks = FClaireonMarkdownParser::ParseToBlocks(InMarkdown);

	// Fallback: if parsing produces nothing, render as plain text
	if (Blocks.Num() == 0)
	{
		const FTextBlockStyle PlainStyle = ClaireonREPLWidgetInternal::MakeColoredTextStyle(Color_Lavender);
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
	const FTextBlockStyle CodeTextStyle =
		FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>(TEXT("NormalText"));

	FTextBlockStyle CodeBlockStyle(CodeTextStyle);
	CodeBlockStyle.SetFont(FCoreStyle::GetDefaultFontStyle("Mono", 9));
	CodeBlockStyle.SetColorAndOpacity(FSlateColor(Color_Amber));

	// Capture code for clipboard copy lambda
	FString CodeCapture = InCode;

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
		.Padding(FMargin(8.0f, 4.0f))
			[SNew(SVerticalBox)

				// Header: language label + copy button
				+ SVerticalBox::Slot().AutoHeight()
					[SNew(SHorizontalBox)

						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
							[SNew(STextBlock)
									.Text(FText::FromString(InLanguage))
									.ColorAndOpacity(FSlateColor(Color_Gray_Mid))
									.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
									.Visibility(InLanguage.IsEmpty()
										? EVisibility::Collapsed
										: EVisibility::Visible)]

						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
							[SNew(SButton)
									.Text(LOCTEXT("CopyCode", "Copy"))
									.ButtonStyle(FAppStyle::Get(), "SimpleButton")
									.TextStyle(FAppStyle::Get(), "SmallButtonText")
									.OnClicked_Lambda([CodeCapture]() -> FReply
							{
								FPlatformApplicationMisc::ClipboardCopy(*CodeCapture);
								return FReply::Handled();
							})]]

				// Code content (selectable, copyable)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[SNew(SMultiLineEditableText)
							.Text(FText::FromString(InCode))
							.TextStyle(&CodeBlockStyle)
							.AutoWrapText(false)
							.IsReadOnly(true)]];
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
			SegColor = Color_Amber;
		}
		else if (Seg.StyleName.Contains(TEXT("Bold")))
		{
			SegFont = FCoreStyle::GetDefaultFontStyle("Bold", InFont.Size);
		}
		else if (Seg.StyleName.Contains(TEXT("AssetLink")))
		{
			SegColor = Color_Amber;
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
		[BuildRow(Block.TableHeaders, HeaderBg, HeaderFont, Color_White)];

	// Data rows
	for (int32 RowIdx = 0; RowIdx < Block.TableRows.Num(); ++RowIdx)
	{
		const FLinearColor& RowBg = (RowIdx % 2 == 0) ? EvenRowBg : OddRowBg;
		Table->AddSlot()
			.AutoHeight()
			[BuildRow(Block.TableRows[RowIdx], RowBg, CellFont, Color_Gray_Light)];
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
		TEXT("^/(Game|Script|Engine|Plugins)/[A-Za-z0-9_/.]+$"));
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
	return LOCTEXT("StatusThinking", "Thinking\u2026");
}

FSlateColor SClaireonREPLWidget::GetStatusDotColor() const
{
	if (bUserStopActive)
		return FSlateColor(Color_Red);
	if (!bIsProcessing)
		return FSlateColor(Color_Gray_Mid);
	float Pulse = PulseSequence.IsPlaying()
		? FMath::Lerp(0.3f, 1.0f, PulseHandle.GetLerp())
		: 1.0f;
	return FSlateColor(FLinearColor(Color_Amber.R, Color_Amber.G, Color_Amber.B, Pulse));
}

FText SClaireonREPLWidget::GetContextIndicatorText() const
{
	if (!Client.IsValid())
		return FText::GetEmpty();
	// Haiku context window ~200K tokens; use rough estimate
	constexpr int32 ContextWindowTokens = 200000;
	int32 Used = Client->GetApproximateTokenCount();
	int32 Pct = FMath::Clamp((Used * 100) / ContextWindowTokens, 0, 100);
	return FText::FromString(FString::Printf(TEXT("ctx: %d%%"), Pct));
}

FSlateColor SClaireonREPLWidget::GetContextIndicatorColor() const
{
	if (!Client.IsValid())
		return FSlateColor(Color_Gray_Mid);
	constexpr int32 ContextWindowTokens = 200000;
	int32 Used = Client->GetApproximateTokenCount();
	int32 Pct = (Used * 100) / ContextWindowTokens;
	if (Pct >= 85)
		return FSlateColor(Color_Amber);
	return FSlateColor(Color_Gray_Mid);
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
		return FSlateColor(FLinearColor(Color_Amber.R, Color_Amber.G,
			Color_Amber.B, Pulse));
	}
	return FSlateColor(FLinearColor(0.25f, 0.25f, 0.25f, 1.0f));
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
