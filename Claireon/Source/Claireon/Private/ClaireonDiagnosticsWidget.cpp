// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonDiagnosticsWidget.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "ClaireonLog.h"
#include "ClaireonREPLWidget.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Layout/SSplitter.h"
#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Async/Async.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "ClaireonFeedbackReport.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#define LOCTEXT_NAMESPACE "ClaireonDiagnostics"

const FName SClaireonDiagnosticsWidget::TabId(TEXT("ClaireonDiagnostics"));

void SClaireonDiagnosticsWidget::RegisterTabSpawner()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		TabId,
		FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&) -> TSharedRef<SDockTab>
		{
			return SNew(SDockTab)
				.TabRole(ETabRole::NomadTab)
				[
					SNew(SClaireonDiagnosticsWidget)
				];
		}))
		.SetDisplayName(LOCTEXT("TabTitle", "AI Chat"))
		.SetTooltipText(LOCTEXT("TabTooltip", "AI Chat assistant with MCP diagnostics"))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetDeveloperToolsMiscCategory())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Comment")));
}

void SClaireonDiagnosticsWidget::UnregisterTabSpawner()
{
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
}

void SClaireonDiagnosticsWidget::Construct(const FArguments& InArgs)
{
	// Auto-start MCP server if not already running
	FClaireonModule& Module = FClaireonModule::Get();
	if (!Module.IsServerRunning())
	{
		Module.StartServer();
		if (!Module.IsServerRunning())
		{
			FNotificationInfo Info(LOCTEXT("MCPServerStartFailed", "Failed to start MCP server"));
			Info.ExpireDuration = 4.0f;
			FSlateNotificationManager::Get().AddNotification(Info);
		}
	}

	// Subscribe to entry-added delegate if server is running
	if (Module.IsServerRunning())
	{
		FClaireonServer* Server = Module.GetServer();
		if (Server)
		{
			DiagnosticsEntryDelegateHandle = Server->OnDiagnosticsEntryAdded.AddRaw(
				this, &SClaireonDiagnosticsWidget::OnDiagnosticsEntryAdded);
			RefreshList();
		}
	}

	// Create the REPL widget
	SAssignNew(REPLWidget, SClaireonREPLWidget, Module.GetServer());

	ChildSlot
	[
		SNew(SVerticalBox)

		// Tab bar header
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(4.0f, 2.0f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("REPLTab", "Chat"))
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.OnClicked_Lambda([this]() -> FReply {
						if (TabSwitcher.IsValid()) TabSwitcher->SetActiveWidgetIndex(0);
						return FReply::Handled();
					})
				]

				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("DiagnosticsTab", "Diagnostics"))
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.OnClicked_Lambda([this]() -> FReply {
						if (TabSwitcher.IsValid()) TabSwitcher->SetActiveWidgetIndex(1);
						return FReply::Handled();
					})
				]
			]
		]

		// Tab content switcher
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(TabSwitcher, SWidgetSwitcher)

			// Tab 0: REPL widget (default view)
			+ SWidgetSwitcher::Slot()
			[
				REPLWidget.ToSharedRef()
			]

			// Tab 1: Diagnostics content
			+ SWidgetSwitcher::Slot()
			[
				SNew(SVerticalBox)

				// Status bar
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(4.0f)
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(this, &SClaireonDiagnosticsWidget::GetStatusText)
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f)
					[
						SNew(SButton)
						.Text(this, &SClaireonDiagnosticsWidget::GetToggleButtonText)
						.OnClicked(this, &SClaireonDiagnosticsWidget::OnToggleServerClicked)
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("ClearLog", "Clear Log"))
						.OnClicked(this, &SClaireonDiagnosticsWidget::OnClearLogClicked)
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4.0f, 0.0f)
					[
						SNew(SButton)
						.Text(this, &SClaireonDiagnosticsWidget::GetFeedbackReportButtonText)
						.IsEnabled(this, &SClaireonDiagnosticsWidget::IsFeedbackReportEnabled)
						.OnClicked(this, &SClaireonDiagnosticsWidget::OnGenerateFeedbackReportClicked)
					]
				]

				// Statistics bar
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(4.0f, 0.0f, 4.0f, 4.0f)
				[
					SNew(STextBlock)
					.Text(this, &SClaireonDiagnosticsWidget::GetStatsText)
					.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
				]

				// Request log list (top)
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SNew(SSplitter)
					.Orientation(Orient_Vertical)

					+ SSplitter::Slot()
					.Value(0.6f)
					[
						SAssignNew(ListView, SListView<TSharedPtr<FMCPDiagnosticsEntry>>)
						.ListItemsSource(&ListItems)
						.OnGenerateRow(this, &SClaireonDiagnosticsWidget::OnGenerateRow)
						.SelectionMode(ESelectionMode::Single)
						.OnSelectionChanged(this, &SClaireonDiagnosticsWidget::OnLogEntrySelected)
					]

					// Detail panel (bottom) — visible when an entry is selected
					+ SSplitter::Slot()
					.Value(0.4f)
					[
						SAssignNew(DetailPanel, SBorder)
						.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
						.Padding(4.0f)
						.Visibility(EVisibility::Collapsed)
						[
							SNew(SVerticalBox)

							// Detail tab buttons
							+ SVerticalBox::Slot()
							.AutoHeight()
							.Padding(0.0f, 0.0f, 0.0f, 4.0f)
							[
								SNew(SHorizontalBox)

								+ SHorizontalBox::Slot().AutoWidth()
								[
									SNew(SButton)
									.Text(LOCTEXT("DetailSummary", "Summary"))
									.ButtonStyle(FAppStyle::Get(), "SimpleButton")
									.OnClicked_Lambda([this]() -> FReply {
										if (DetailSwitcher.IsValid()) DetailSwitcher->SetActiveWidgetIndex(0);
										return FReply::Handled();
									})
								]

								+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
								[
									SNew(SButton)
									.Text(LOCTEXT("DetailRawJSON", "Raw JSON"))
									.ButtonStyle(FAppStyle::Get(), "SimpleButton")
									.OnClicked_Lambda([this]() -> FReply {
										if (DetailSwitcher.IsValid()) DetailSwitcher->SetActiveWidgetIndex(1);
										return FReply::Handled();
									})
								]
							]

							// Detail content
							+ SVerticalBox::Slot()
							.FillHeight(1.0f)
							[
								SAssignNew(DetailSwitcher, SWidgetSwitcher)

								// Detail Tab 0: Summary
								+ SWidgetSwitcher::Slot()
								[
									SAssignNew(DetailSummaryBox, SScrollBox)
								]

								// Detail Tab 1: Raw JSON
								+ SWidgetSwitcher::Slot()
								[
									SAssignNew(DetailJsonBox, SScrollBox)
								]
							]
						]
					]
				]
			]
		]
	];
}

void SClaireonDiagnosticsWidget::HandleGlobalEmergencyStop()
{
	if (REPLWidget.IsValid())
	{
		REPLWidget->TriggerEmergencyStop();
	}
}

FReply SClaireonDiagnosticsWidget::OnKeyDown(const FGeometry& MyGeometry,
	const FKeyEvent& InKeyEvent)
{
	// Ctrl+. = global emergency stop
	if (InKeyEvent.GetKey() == EKeys::Period && InKeyEvent.IsControlDown())
	{
		HandleGlobalEmergencyStop();
		return FReply::Handled();
	}

	// Escape — try REPL first (only during processing), else pass through
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		if (REPLWidget.IsValid() && REPLWidget->HandleEscapeKey())
		{
			return FReply::Handled();
		}
	}

	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

SClaireonDiagnosticsWidget::~SClaireonDiagnosticsWidget()
{
	FClaireonModule* Module = FModuleManager::GetModulePtr<FClaireonModule>(TEXT("Claireon"));
	if (Module && Module->IsServerRunning())
	{
		FClaireonServer* Server = Module->GetServer();
		if (Server)
		{
			Server->OnDiagnosticsEntryAdded.Remove(DiagnosticsEntryDelegateHandle);
		}
	}
}

TSharedRef<ITableRow> SClaireonDiagnosticsWidget::OnGenerateRow(
	TSharedPtr<FMCPDiagnosticsEntry> Item,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	if (!Item.IsValid())
	{
		return SNew(STableRow<TSharedPtr<FMCPDiagnosticsEntry>>, OwnerTable);
	}

	// Format: [HH:MM:SS] method (tool) - duration - status
	FString TimeStr = Item->Timestamp.ToString(TEXT("%H:%M:%S"));
	FString MethodStr = Item->Method;
	if (!Item->ToolName.IsEmpty())
	{
		MethodStr += FString::Printf(TEXT(" (%s)"), *Item->ToolName);
	}
	FString DurationStr = FString::Printf(TEXT("%.1fms"), Item->DurationMs);
	FString StatusStr = Item->bIsError ? TEXT("ERROR") : TEXT("OK");

	FLinearColor RowColor = Item->bIsError
		? FLinearColor(1.0f, 0.3f, 0.3f)
		: FLinearColor::White;

	return SNew(STableRow<TSharedPtr<FMCPDiagnosticsEntry>>, OwnerTable)
	[
		SNew(SHorizontalBox)

		// Timestamp
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4.0f, 2.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(TimeStr))
			.ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))
		]

		// Method + tool name
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(4.0f, 2.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(MethodStr))
			.ColorAndOpacity(FSlateColor(RowColor))
		]

		// Duration
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4.0f, 2.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(DurationStr))
			.ColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.7f, 0.7f)))
		]

		// Status
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(4.0f, 2.0f)
		[
			SNew(SBox)
			.MinDesiredWidth(50.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(StatusStr))
				.ColorAndOpacity(FSlateColor(RowColor))
			]
		]
	];
}

void SClaireonDiagnosticsWidget::OnDiagnosticsEntryAdded(const FMCPDiagnosticsEntry& Entry)
{
	// This delegate fires from the HTTP background thread.
	// Marshal Slate updates to the game thread via weak pointer for safety.
	TSharedPtr<FMCPDiagnosticsEntry> EntryCopy = MakeShared<FMCPDiagnosticsEntry>(Entry);
	TWeakPtr<SWidget> WeakSelf = AsShared();

	AsyncTask(ENamedThreads::GameThread, [WeakSelf, EntryCopy]()
	{
		TSharedPtr<SWidget> Pinned = WeakSelf.Pin();
		if (!Pinned.IsValid())
		{
			return;
		}

		SClaireonDiagnosticsWidget& Self = static_cast<SClaireonDiagnosticsWidget&>(*Pinned);
		Self.ListItems.Add(EntryCopy);

		// Trim to match ring buffer size
		if (Self.ListItems.Num() > FClaireonServer::MaxDiagnosticsEntries)
		{
			Self.ListItems.RemoveAt(0);
		}

		if (Self.ListView.IsValid())
		{
			Self.ListView->RequestListRefresh();
			// Auto-scroll to bottom
			if (Self.ListItems.Num() > 0)
			{
				Self.ListView->RequestScrollIntoView(Self.ListItems.Last());
			}
		}
	});
}

void SClaireonDiagnosticsWidget::RefreshList()
{
	ListItems.Empty();

	FClaireonModule& Module = FClaireonModule::Get();
	if (Module.IsServerRunning())
	{
		FClaireonServer* Server = Module.GetServer();
		if (Server)
		{
			for (const FMCPDiagnosticsEntry& Entry : Server->GetDiagnosticsEntries())
			{
				ListItems.Add(MakeShared<FMCPDiagnosticsEntry>(Entry));
			}
		}
	}

	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
	}
}

FText SClaireonDiagnosticsWidget::GetStatusText() const
{
	FClaireonModule& Module = FClaireonModule::Get();
	if (Module.IsServerRunning())
	{
		FClaireonServer* Server = Module.GetServer();
		if (Server)
		{
			FTimespan Uptime = FDateTime::Now() - Server->GetStartTime();
			return FText::FromString(FString::Printf(
				TEXT("Running on port %u (uptime: %s)"),
				Server->GetPort(),
				*Uptime.ToString(TEXT("%h:%m:%s"))));
		}
	}
	return LOCTEXT("StatusStopped", "Server stopped");
}

FText SClaireonDiagnosticsWidget::GetStatsText() const
{
	FClaireonModule& Module = FClaireonModule::Get();
	if (Module.IsServerRunning())
	{
		FClaireonServer* Server = Module.GetServer();
		if (Server)
		{
			return FText::FromString(FString::Printf(
				TEXT("Requests: %d | Errors: %d"),
				Server->GetTotalRequestCount(),
				Server->GetErrorCount()));
		}
	}
	return FText::GetEmpty();
}

FReply SClaireonDiagnosticsWidget::OnToggleServerClicked()
{
	FClaireonModule& Module = FClaireonModule::Get();

	if (Module.IsServerRunning())
	{
		// Unsubscribe from old server before stopping
		FClaireonServer* Server = Module.GetServer();
		if (Server)
		{
			Server->OnDiagnosticsEntryAdded.Remove(DiagnosticsEntryDelegateHandle);
			DiagnosticsEntryDelegateHandle.Reset();
		}
		Module.StopServer();
	}
	else
	{
		Module.StartServer();

		// Subscribe to new server
		FClaireonServer* Server = Module.GetServer();
		if (Server)
		{
			DiagnosticsEntryDelegateHandle = Server->OnDiagnosticsEntryAdded.AddRaw(
				this, &SClaireonDiagnosticsWidget::OnDiagnosticsEntryAdded);
		}
	}

	RefreshList();
	return FReply::Handled();
}

FReply SClaireonDiagnosticsWidget::OnClearLogClicked()
{
	ListItems.Empty();

	FClaireonModule& Module = FClaireonModule::Get();
	if (Module.IsServerRunning())
	{
		FClaireonServer* Server = Module.GetServer();
		if (Server)
		{
			Server->ClearDiagnostics();
		}
	}

	if (ListView.IsValid())
	{
		ListView->RequestListRefresh();
	}

	return FReply::Handled();
}

FText SClaireonDiagnosticsWidget::GetToggleButtonText() const
{
	return FClaireonModule::Get().IsServerRunning()
		? LOCTEXT("StopServer", "Stop Server")
		: LOCTEXT("StartServer", "Start Server");
}

void SClaireonDiagnosticsWidget::OnLogEntrySelected(
	TSharedPtr<FMCPDiagnosticsEntry> SelectedItem, ESelectInfo::Type SelectInfo)
{
	if (!SelectedItem.IsValid())
	{
		if (DetailPanel.IsValid())
		{
			DetailPanel->SetVisibility(EVisibility::Collapsed);
		}
		return;
	}

	PopulateDetailPanel(*SelectedItem);

	if (DetailPanel.IsValid())
	{
		DetailPanel->SetVisibility(EVisibility::Visible);
	}
}

void SClaireonDiagnosticsWidget::PopulateDetailPanel(const FMCPDiagnosticsEntry& Entry)
{
	// --- Summary tab ---
	if (DetailSummaryBox.IsValid())
	{
		DetailSummaryBox->ClearChildren();

		FLinearColor StatusColor = Entry.bIsError
			? FLinearColor(1.0f, 0.3f, 0.3f)
			: FLinearColor(0.1f, 0.8f, 0.1f);
		FString StatusStr = Entry.bIsError ? TEXT("ERROR") : TEXT("OK");

		// Timestamp
		DetailSummaryBox->AddSlot().Padding(2.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(FString::Printf(TEXT("Timestamp: %s"),
				*Entry.Timestamp.ToString(TEXT("%Y-%m-%d %H:%M:%S")))))
		];

		// Method
		DetailSummaryBox->AddSlot().Padding(2.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(FString::Printf(TEXT("Method: %s"), *Entry.Method)))
		];

		// Tool Name
		if (!Entry.ToolName.IsEmpty())
		{
			DetailSummaryBox->AddSlot().Padding(2.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("Tool: %s"), *Entry.ToolName)))
			];
		}

		// Duration
		DetailSummaryBox->AddSlot().Padding(2.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(FString::Printf(TEXT("Duration: %.1fms"), Entry.DurationMs)))
		];

		// Status
		DetailSummaryBox->AddSlot().Padding(2.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(FString::Printf(TEXT("Status: %s"), *StatusStr)))
			.ColorAndOpacity(FSlateColor(StatusColor))
		];

		// Error message extract (if error, try to parse from response body)
		if (Entry.bIsError && !Entry.ResponseBody.IsEmpty())
		{
			FString ErrorMsg;
			TSharedPtr<FJsonObject> JsonObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Entry.ResponseBody);
			if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
			{
				if (const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
					JsonObj->TryGetObjectField(TEXT("error"), ErrorObj))
				{
					(*ErrorObj)->TryGetStringField(TEXT("message"), ErrorMsg);
				}
			}

			if (!ErrorMsg.IsEmpty())
			{
				DetailSummaryBox->AddSlot().Padding(2.0f, 8.0f, 2.0f, 2.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("Error: %s"), *ErrorMsg)))
					.ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.3f, 0.3f)))
					.AutoWrapText(true)
				];
			}
		}
	}

	// --- Raw JSON tab ---
	if (DetailJsonBox.IsValid())
	{
		DetailJsonBox->ClearChildren();

		auto AddJsonSection = [this](const FString& Label, const FString& RawJson)
		{
			DetailJsonBox->AddSlot().Padding(2.0f, 4.0f)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Label))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
					]

					+ SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("CopyToClipboard", "Copy"))
						.ButtonStyle(FAppStyle::Get(), "SimpleButton")
						.OnClicked_Lambda([RawJson]() -> FReply {
							FPlatformApplicationMisc::ClipboardCopy(*RawJson);
							return FReply::Handled();
						})
					]
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
					.Padding(4.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(PrettyPrintJson(RawJson)))
						.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
						.AutoWrapText(true)
					]
				]
			];
		};

		AddJsonSection(TEXT("Request"), Entry.RequestBody);
		AddJsonSection(TEXT("Response"), Entry.ResponseBody);
	}

	// Default to Summary tab
	if (DetailSwitcher.IsValid())
	{
		DetailSwitcher->SetActiveWidgetIndex(0);
	}
}

FReply SClaireonDiagnosticsWidget::OnGenerateFeedbackReportClicked()
{
	bGeneratingFeedbackReport = true;

	TWeakPtr<SWidget> WeakSelf = AsShared();

	FClaireonFeedbackReport::Generate(
		FOnFeedbackReportComplete::CreateLambda([WeakSelf](bool bSuccess, const FString& Message)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, bSuccess, Message]()
		{
			TSharedPtr<SWidget> Pinned = WeakSelf.Pin();
			if (!Pinned.IsValid())
			{
				return;
			}

			SClaireonDiagnosticsWidget& Self = static_cast<SClaireonDiagnosticsWidget&>(*Pinned);
			Self.bGeneratingFeedbackReport = false;

			// Show notification
			UE_LOG(LogClaireon, Display, TEXT("[MCP] Feedback report: %s"), *Message);
		});
	}));

	return FReply::Handled();
}

FText SClaireonDiagnosticsWidget::GetFeedbackReportButtonText() const
{
	return bGeneratingFeedbackReport
		? LOCTEXT("GeneratingReport", "Generating Report...")
		: LOCTEXT("GenerateReport", "Generate Feedback Report");
}

bool SClaireonDiagnosticsWidget::IsFeedbackReportEnabled() const
{
	return !bGeneratingFeedbackReport;
}

FString SClaireonDiagnosticsWidget::PrettyPrintJson(const FString& RawJson)
{
	if (RawJson.IsEmpty())
	{
		return TEXT("(empty)");
	}

	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RawJson);
	if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
	{
		FString Output;
		TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Output);
		FJsonSerializer::Serialize(JsonObj.ToSharedRef(), Writer);
		return Output;
	}

	// If not valid JSON, return as-is
	return RawJson;
}

#undef LOCTEXT_NAMESPACE
