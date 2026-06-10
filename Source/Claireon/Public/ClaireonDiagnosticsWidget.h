// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Layout/Visibility.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Animation/CurveSequence.h"
#include "ClaireonTypes.h"
#include "ClaireonREPLWidget.h"

class FClaireonServer;

struct FClaireonFeedbackPanelEntry
{
	FString Id;
	FDateTime Timestamp;
	FString Text;
	FString TextPreview;
	FString RequestSummary;
	FString SourceDir;
	FString EntryFilePath;
	FString RawJson;
	double DurationMs = 0.0;
	bool bIsBug = false;
	bool bIsFeedback = true;
	bool bIsSuggestion = false;
	bool bFromCurrentSession = false;
};

/**
 * Claireon panel.
 *
 * Layout:
 *   [Tab bar: Activity (N) | Feedback (M) | Chat*]   (* only when bEnableREPLChat)
 *   [Status strip: conn-dot + state | proxy chip | stats segments]
 *   [Content area: switcher driven by active tab]
 *
 * Activity tab: log list (Time | Method | Content | Duration)
 *               + horizontal JSON split (Request | Response)
 * Feedback tab: left feedback list + right detail view
 * Chat tab:     in-editor REPL (SClaireonREPLWidget), hidden by default
 */
class SClaireonDiagnosticsWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SClaireonDiagnosticsWidget) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SClaireonDiagnosticsWidget() override;

	/** Nomad tab ID for registration */
	static const FName TabId;

	/** Handle Ctrl+. emergency stop from the parent window. */
	void HandleGlobalEmergencyStop();

	virtual FReply OnKeyDown(const FGeometry& MyGeometry,
		const FKeyEvent& InKeyEvent) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }

	/** Register / unregister the tab spawner with FGlobalTabmanager */
	static void RegisterTabSpawner();
	static void UnregisterTabSpawner();

private:
	// ---------------------------------------------------------------------------
	// Build helpers
	// ---------------------------------------------------------------------------
	TSharedRef<SWidget> BuildTabBar();
	TSharedRef<SWidget> BuildStatusStrip();
	TSharedRef<SWidget> BuildActivityTab();
	TSharedRef<SWidget> BuildFeedbackTab();

	// ---------------------------------------------------------------------------
	// Tab switching
	// ---------------------------------------------------------------------------
	void SetActiveTab(int32 Index);

	// ---------------------------------------------------------------------------
	// Status strip
	// ---------------------------------------------------------------------------
	FSlateColor GetConnDotColor() const;
	FText GetServerStateText() const;
	FText GetProxyChipText() const;
	FText GetProxyChipModeText() const;
	FText GetProxyChipPortText() const;
	EVisibility GetProxyChipVisibility() const;
	FReply OnProxyChipClicked();
	float GetProxyPulseOpacity() const;
	FText GetStatsSegmentsText() const;

	// ---------------------------------------------------------------------------
	// Activity tab - log list
	// ---------------------------------------------------------------------------
	TSharedRef<ITableRow> OnGenerateActivityRow(
		TSharedPtr<FMCPDiagnosticsEntry> Item,
		const TSharedRef<STableViewBase>& OwnerTable);
	void OnActivityEntrySelected(
		TSharedPtr<FMCPDiagnosticsEntry> Item, ESelectInfo::Type SelectInfo);
	void PopulateJsonPanes(const FMCPDiagnosticsEntry& Entry);

	/** Called when a new diagnostics entry is added (may fire from background thread) */
	void OnDiagnosticsEntryAdded(const FMCPDiagnosticsEntry& Entry);

	/** Refresh the list from the server's ring buffer */
	void RefreshList();

	/** Text filter for the Content column */
	void OnSearchTextChanged(const FText& NewText);

	/** Live/Paused toggle */
	FReply OnLivePausedToggled();
	FText GetLivePausedButtonText() const;

	// ---------------------------------------------------------------------------
	// Activity tab - toolbar actions
	// ---------------------------------------------------------------------------
	FReply OnClearLogClicked();

	// ---------------------------------------------------------------------------
	// Feedback tab
	// ---------------------------------------------------------------------------
	TSharedRef<ITableRow> OnGenerateFeedbackRow(
		TSharedPtr<FClaireonFeedbackPanelEntry> Item,
		const TSharedRef<STableViewBase>& OwnerTable);
	void OnFeedbackEntrySelected(
		TSharedPtr<FClaireonFeedbackPanelEntry> Item, ESelectInfo::Type SelectInfo);
	void PopulateFeedbackDetail(const FClaireonFeedbackPanelEntry& Entry);
	FReply OnExportFeedbackJsonlClicked();
	FReply OnGenerateFeedbackReportClicked();
	FText GetFeedbackReportButtonText() const;
	bool IsFeedbackReportEnabled() const;
	FReply OnFeedbackScopeToggled();
	FText GetFeedbackScopeButtonText() const;

	// ---------------------------------------------------------------------------
	// Server start/stop (kept for status strip)
	// ---------------------------------------------------------------------------
	FReply OnToggleServerClicked();
	FText GetToggleButtonText() const;

	// ---------------------------------------------------------------------------
	// Helpers
	// ---------------------------------------------------------------------------
	static FString PrettyPrintJson(const FString& RawJson);

	/** Build the filtered+sorted view of ListItems for the activity list */
	void RebuildFilteredItems();
	/** Same but for feedback-only entries */
	void RebuildFeedbackItems();
	void RememberCurrentSessionFeedbackId(const FMCPDiagnosticsEntry& Entry);

	// ---------------------------------------------------------------------------
	// Animation (proxy verifying pulse)
	// ---------------------------------------------------------------------------
	void StartProxyPulse();
	void StopProxyPulse();

	// ---------------------------------------------------------------------------
	// Data
	// ---------------------------------------------------------------------------
	/** All entries, newest last (mirrors server ring buffer) */
	TArray<TSharedPtr<FMCPDiagnosticsEntry>> ListItems;

	/** Filtered subset shown in the activity list (rebuilt on filter change) */
	TArray<TSharedPtr<FMCPDiagnosticsEntry>> FilteredItems;

	/** Persisted feedback entries loaded from Saved/Claireon/Feedback. */
	TArray<TSharedPtr<FClaireonFeedbackPanelEntry>> FeedbackItems;

	/** Feedback entry IDs observed through this panel's live diagnostics stream. */
	TSet<FString> CurrentSessionFeedbackIds;

	/** Active text filter (matches against ContentPreview) */
	FString SearchFilter;

	/** Whether new entries should auto-scroll (true = live, false = paused) */
	bool bIsLive = true;

	/** Delegate handle for entry-added callback */
	FDelegateHandle DiagnosticsEntryDelegateHandle;

	/** Whether a feedback report is currently being generated */
	bool bGeneratingFeedbackReport = false;

	/** When true, the report generator reads from all git worktrees of this repo */
	bool bFeedbackAllWorktrees = false;

	// ---------------------------------------------------------------------------
	// Widgets
	// ---------------------------------------------------------------------------
	/** Main tab content switcher: 0=Activity, 1=Feedback, 2=Chat(optional) */
	TSharedPtr<SWidgetSwitcher> TabSwitcher;
	int32 ActiveTabIndex = 0;

	/** Activity log list view */
	TSharedPtr<SListView<TSharedPtr<FMCPDiagnosticsEntry>>> ActivityList;

	/** Activity tab "Activity (N)" button label -- updated when entries change */
	TSharedPtr<STextBlock> ActivityTabLabel;
	TSharedPtr<STextBlock> FeedbackTabLabel;

	/** Status strip: proxy pulse animation */
	FCurveSequence ProxyPulseSequence;
	FCurveHandle ProxyPulseHandle;

	/** JSON panes (request / response) in the Activity split */
	TSharedPtr<SScrollBox> RequestJsonPane;
	TSharedPtr<SScrollBox> ResponseJsonPane;

	/** Cached pretty-printed JSON strings for clipboard copy */
	FString SelectedRequestJson;
	FString SelectedResponseJson;

	/** Feedback tab list and detail panes */
	TSharedPtr<SListView<TSharedPtr<FClaireonFeedbackPanelEntry>>> FeedbackList;
	TSharedPtr<SScrollBox> FeedbackDetailPane;
	TSharedPtr<SBorder> FeedbackDetailBorder;

	/** Tracks the currently selected feedback item (by ID for rebuild safety) */
	FString SelectedFeedbackItemId;

	/** Tracks the currently selected activity item (pointer valid until RefreshList) */
	TSharedPtr<FMCPDiagnosticsEntry> SelectedActivityItem;

	/** The embedded REPL widget (Chat tab) */
	TSharedPtr<SClaireonREPLWidget> REPLWidget;
};
