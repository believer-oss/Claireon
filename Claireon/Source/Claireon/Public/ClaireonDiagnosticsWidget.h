// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "ClaireonTypes.h"
#include "ClaireonREPLWidget.h"

class FClaireonServer;

/**
 * Diagnostics window for the MCP server.
 * Shows server status, request log, and statistics.
 */
class SClaireonDiagnosticsWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SClaireonDiagnosticsWidget)
	{}
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

	/** Register the tab spawner with FGlobalTabmanager */
	static void RegisterTabSpawner();

	/** Unregister the tab spawner */
	static void UnregisterTabSpawner();

private:
	/** Generate a row for the log list view */
	TSharedRef<ITableRow> OnGenerateRow(
		TSharedPtr<FMCPDiagnosticsEntry> Item,
		const TSharedRef<STableViewBase>& OwnerTable);

	/** Called when a new diagnostics entry is added */
	void OnDiagnosticsEntryAdded(const FMCPDiagnosticsEntry& Entry);

	/** Refresh the list from the server's ring buffer */
	void RefreshList();

	/** Get the server status text */
	FText GetStatusText() const;

	/** Get the statistics text */
	FText GetStatsText() const;

	/** Handle Start/Stop button click */
	FReply OnToggleServerClicked();

	/** Handle Clear Log button click */
	FReply OnClearLogClicked();

	/** Get the toggle button text */
	FText GetToggleButtonText() const;

	/** Handle log entry selection for detail view */
	void OnLogEntrySelected(TSharedPtr<FMCPDiagnosticsEntry> SelectedItem, ESelectInfo::Type SelectInfo);

	/** Populate the detail panel with the selected entry */
	void PopulateDetailPanel(const FMCPDiagnosticsEntry& Entry);

	/** Pretty-print a JSON string */
	static FString PrettyPrintJson(const FString& RawJson);

	/** Handle Generate Feedback Report button click */
	FReply OnGenerateFeedbackReportClicked();

	/** Get the feedback report button text (changes while generating) */
	FText GetFeedbackReportButtonText() const;

	/** Whether the feedback report button is enabled */
	bool IsFeedbackReportEnabled() const;

	/** The list of entries for the SListView (shared pointers to copies) */
	TArray<TSharedPtr<FMCPDiagnosticsEntry>> ListItems;

	/** The list view widget */
	TSharedPtr<SListView<TSharedPtr<FMCPDiagnosticsEntry>>> ListView;

	/** Delegate handle for entry-added callback */
	FDelegateHandle DiagnosticsEntryDelegateHandle;

	/** The embedded REPL widget (Claude tab) */
	TSharedPtr<SClaireonREPLWidget> REPLWidget;

	/** Tab content switcher: 0=Chat REPL, 1=Diagnostics */
	TSharedPtr<SWidgetSwitcher> TabSwitcher;

	/** Detail panel for selected log entry */
	TSharedPtr<SBorder> DetailPanel;

	/** Detail view switcher: 0=Summary, 1=Raw JSON */
	TSharedPtr<SWidgetSwitcher> DetailSwitcher;

	/** Detail summary scroll box */
	TSharedPtr<SScrollBox> DetailSummaryBox;

	/** Detail raw JSON scroll box */
	TSharedPtr<SScrollBox> DetailJsonBox;

	/** Whether a feedback report is currently being generated */
	bool bGeneratingFeedbackReport = false;
};
