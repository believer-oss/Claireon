// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Animation/CurveSequence.h"
#include "ClaireonAnthropicClient.h"
class FClaireonServer;
class FClaireonREPLLogger;
class FClaireonAnthropicClient;
class SMultiLineEditableTextBox;
class SEditableTextBox;
class SScrollBox;
class SWidgetSwitcher;
class SVerticalBox;
class STextBlock;
class SMultiLineEditableText;
class SButton;
class FMenuBuilder;
struct FClaireonMarkdownBlock;
struct FClaireonStyledLine;

/** A single rendered item in the conversation: message bubble or tool card. */
struct FREPLConversationItem
{
	enum class EItemType
	{
		UserMessage,
		AssistantMessage,
		ToolCard,
		SystemMessage
	};

	EItemType ItemType = EItemType::SystemMessage;
	FString Text;
	FString ToolName;
	FString ToolArgsJson;
	FString ToolUseId;
	double ToolDurationMs = 0.0;
	bool bToolRunning = false;
	bool bToolError = false;
	bool bIsError = false; // For SystemMessage
	FString Timestamp;     // HH:MM:SS recorded at append time
};

/**
 * In-editor Claude REPL widget.
 * Embedded as a tab inside SClaireonDiagnosticsWidget.
 */
class SClaireonREPLWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SClaireonREPLWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, FClaireonServer* InServer);
	virtual ~SClaireonREPLWidget() override;

	/** Handle Escape key for cancel. Called by parent when Escape is pressed and
	 *  REPL has focus. Returns true if consumed. */
	bool HandleEscapeKey();

	/** Trigger emergency stop (Ctrl+.). */
	void TriggerEmergencyStop();

	virtual FReply OnKeyDown(const FGeometry& MyGeometry,
		const FKeyEvent& InKeyEvent) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }

private:
	// --- Construction ---
	TSharedRef<SWidget> BuildOnboardingView();
	TSharedRef<SWidget> BuildChatView();
	TSharedRef<SWidget> BuildThinkingRowWidget();
	TSharedRef<SWidget> BuildComposerWidget();
	TSharedRef<SWidget> BuildAvatarWidget(bool bIsAssistant);
	TSharedRef<SWidget> BuildMessageBodyContent(const FREPLConversationItem& Item);

	// --- Onboarding ---
	FReply OnGetApiKeyClicked();
	FReply OnVerifyAndSaveClicked();
	FText GetOnboardingStatusText() const;

	// --- Chat input ---
	void OnInputTextCommitted(const FText& Text, ETextCommit::Type CommitType);
	FReply OnSendClicked();
	FReply OnStopClicked();
	FReply OnNewTopicClicked();
	void SubmitCurrentInput();

	// --- API client events ---
	void OnREPLEvent(const FREPLEvent& Event);

	// --- Conversation rendering ---
	void AppendConversationItem(FREPLConversationItem&& Item);
	void UpdateToolCard(const FString& ToolUseId, const FREPLConversationItem& Updated);
	void RebuildConversationDisplay();
	TSharedRef<SWidget> BuildConversationItemWidget(const FREPLConversationItem& Item);
	TSharedRef<SWidget> BuildToolCardWidget(const FREPLConversationItem& Item);

	// --- Rich text rendering ---
	TSharedRef<SWidget> BuildAssistantMessageWidget(const FString& InMarkdown);
	TSharedRef<SWidget> BuildCodeBlockWidget(
		const FString& InCode, const FString& InLanguage);
	TSharedRef<SWidget> BuildTableWidget(const FClaireonMarkdownBlock& Block);
	TSharedRef<SWidget> BuildStyledTableCell(
		const FString& InCellText, const FSlateFontInfo& InFont,
		const FLinearColor& InDefaultColor);
	void OnMessageContextMenu(FMenuBuilder& MenuBuilder,
		TSharedRef<SMultiLineEditableText> InTextWidget);
	void NavigateToAssetPath(FString AssetPath);

	// --- State helpers ---
	void SetProcessingState(bool bProcessing);
	void CheckApiKeyAndSwitchView();
	void TriggerNewTopic(bool bWithHandoff);
	void UpdateContextIndicator();
	void UpdateNewTopicButtonState(bool bHighlight, const FString& HandoffText);

	// --- Status / status bar ---
	FText GetStatusText() const;
	FSlateColor GetStatusDotColor() const;
	FText GetContextIndicatorText() const;
	FSlateColor GetContextIndicatorColor() const;
	FText GetNewTopicButtonText() const;
	FSlateColor GetNewTopicButtonColor() const;

	// --- Animation ---
	void StartPulseAnimation();
	void StopPulseAnimation();
	float GetPulseOpacity() const;

	// --- Input history ---
	void PushInputHistory(const FString& Text);
	void NavigateHistory(int32 Delta); // -1 = up (older), +1 = down (newer)

	// --- Data ---
	FClaireonServer* Server = nullptr;
	TSharedPtr<FClaireonREPLLogger> Logger;
	TSharedPtr<FClaireonAnthropicClient> Client;

	// Conversation
	TArray<FREPLConversationItem> ConversationItems;
	FString PendingFreshContextHandoff;
	bool bFreshContextSuggested = false;

	// Input history
	TArray<FString> InputHistory;
	int32 HistoryCursor = -1; // -1 = current draft

	// Processing state
	bool bIsProcessing = false;
	FString CurrentConversationId;
	int32 ConversationCounter = 0;

	// Animation
	FCurveSequence PulseSequence;
	FCurveHandle PulseHandle;

	// Delegate handle
	FDelegateHandle REPLEventHandle;
	FDelegateHandle SettingsChangedHandle;

	// Widgets
	TSharedPtr<SWidgetSwitcher> RootSwitcher; // 0=onboarding, 1=chat
	TSharedPtr<SScrollBox> ConversationScroll;
	TSharedPtr<SVerticalBox> ConversationBox;
	TSharedPtr<SMultiLineEditableText> InputBox;  // composer text (no box wrapper)
	TSharedPtr<SButton> NewTopicButton;
	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<STextBlock> ContextIndicatorText;
	TSharedPtr<STextBlock> ThinkingStepText;       // step label in thinking row
	int32 ContextUsagePct = 3;                     // context window usage 0-100

	// Onboarding
	TSharedPtr<SEditableTextBox> ApiKeyInputBox;
	FString OnboardingStatusMessage;
	bool bOnboardingVerifying = false;

	// User-stop state (shown in status bar)
	bool bUserStopActive = false;
	FDelegateHandle UserStopDelegateHandle;
};
