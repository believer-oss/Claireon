// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonDiagnosticsWidget.h"
#include "ClaireonModule.h"
#include "ClaireonProxyClient.h"
#include "ClaireonServer.h"
#include "ClaireonLog.h"
#include "ClaireonSettings.h"
#include "ClaireonREPLWidget.h"
#include "ClaireonFeedbackLog.h"
#include "ClaireonFeedbackReport.h"
#include "ClaireonToolbarStyle.h"
#include "Styling/SlateStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Framework/Docking/TabManager.h"
#include "Styling/AppStyle.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Async/Async.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "ISettingsModule.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"

#include <initializer_list>

#define LOCTEXT_NAMESPACE "ClaireonDiagnostics"

// ---------------------------------------------------------------------------
// Design tokens (matched to styles.css in the Claude Design mock)
// ---------------------------------------------------------------------------
namespace ClaireonColors
{
	static FLinearColor Hex(const TCHAR* HexString, float Alpha = 1.0f)
	{
		FLinearColor Color = FLinearColor::FromSRGBColor(FColor::FromHex(HexString));
		Color.A = Alpha;
		return Color;
	}

	// Backgrounds
	static const FLinearColor BgPanel   = Hex(TEXT("15171A"));
	static const FLinearColor BgRow     = Hex(TEXT("12141A"));
	static const FLinearColor BgRowHover= Hex(TEXT("1D2024"));
	static const FLinearColor BgRowSel  = Hex(TEXT("2A2014"));
	static const FLinearColor Bg2       = Hex(TEXT("1B1D21"));
	static const FLinearColor Bg3       = Hex(TEXT("22252A"));
	static const FLinearColor BgCode    = Hex(TEXT("08090B"));

	// Lines (dividers)
	static const FLinearColor Line1     = Hex(TEXT("25272C"));
	static const FLinearColor Line2     = Hex(TEXT("2E3137"));

	// Foregrounds
	static const FLinearColor Fg1 = Hex(TEXT("ECEDEF"));
	static const FLinearColor Fg2 = Hex(TEXT("A8ACB3"));
	static const FLinearColor Fg3 = Hex(TEXT("6B6F76"));
	static const FLinearColor Fg4 = Hex(TEXT("4A4D54"));

	// Semantic
	static const FLinearColor Accent    = Hex(TEXT("E7873B"));
	static const FLinearColor AccentHi  = Hex(TEXT("F0A063"));
	static const FLinearColor AccentSoft= Hex(TEXT("E7873B"), 0.14f);
	static const FLinearColor AccentLine= Hex(TEXT("E7873B"), 0.35f);
	static const FLinearColor Ok        = Hex(TEXT("4EA780"));
	static const FLinearColor Warn      = Hex(TEXT("D3A24A"));
	static const FLinearColor Err       = Hex(TEXT("D96565"));
	static const FLinearColor Info      = Hex(TEXT("6B8FC7"));
	static const FLinearColor InfoHi    = Hex(TEXT("98B3D9"));

	// Duration threshold colors
	static FLinearColor DurationColor(double Ms)
	{
		if (Ms > 500.0) return Err;
		if (Ms > 150.0) return Warn;
		return Fg3;
	}
}

namespace ClaireonFeedbackPanel
{
	static bool TryGetStringAny(const TSharedPtr<FJsonObject>& Object,
		std::initializer_list<const TCHAR*> FieldNames, FString& OutValue)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		for (const TCHAR* FieldName : FieldNames)
		{
			if (Object->TryGetStringField(FieldName, OutValue))
			{
				return true;
			}
		}
		return false;
	}

	static bool TryGetBoolAny(const TSharedPtr<FJsonObject>& Object,
		std::initializer_list<const TCHAR*> FieldNames, bool& OutValue)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		for (const TCHAR* FieldName : FieldNames)
		{
			if (Object->TryGetBoolField(FieldName, OutValue))
			{
				return true;
			}
		}
		return false;
	}

	static bool FindStringFieldRecursive(const TSharedPtr<FJsonObject>& Object,
		const FString& FieldName, FString& OutValue)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		if (Object->TryGetStringField(FieldName, OutValue))
		{
			return true;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			if (!Pair.Value.IsValid())
			{
				continue;
			}
			if (Pair.Value->Type == EJson::Object)
			{
				if (FindStringFieldRecursive(Pair.Value->AsObject(), FieldName, OutValue))
				{
					return true;
				}
			}
			else if (Pair.Value->Type == EJson::Array)
			{
				for (const TSharedPtr<FJsonValue>& ArrayValue : Pair.Value->AsArray())
				{
					if (ArrayValue.IsValid() && ArrayValue->Type == EJson::Object &&
						FindStringFieldRecursive(ArrayValue->AsObject(), FieldName, OutValue))
					{
						return true;
					}
				}
			}
		}
		return false;
	}

	static FString ExtractFeedbackEntryId(const FString& RawJson)
	{
		TSharedPtr<FJsonObject> RootObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RawJson);
		if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
		{
			return FString();
		}

		FString EntryId;
		FindStringFieldRecursive(RootObject, TEXT("entry_id"), EntryId);
		return EntryId;
	}

	static FString MakePreview(FString Text)
	{
		Text.ReplaceInline(TEXT("\r\n"), TEXT(" "));
		Text.ReplaceInline(TEXT("\n"), TEXT(" "));
		Text.ReplaceInline(TEXT("\r"), TEXT(" "));
		Text.TrimStartAndEndInline();
		return Text.Left(180);
	}

	static void AppendEntriesFromDir(const FString& FeedbackDir,
		const TSet<FString>& CurrentSessionIds,
		TArray<TSharedPtr<FClaireonFeedbackPanelEntry>>& OutEntries)
	{
		const FString IndexPath = FeedbackDir / TEXT("index.json");
		FString IndexJson;
		if (!FFileHelper::LoadFileToString(IndexJson, *IndexPath))
		{
			return;
		}

		TSharedPtr<FJsonObject> IndexObject;
		const TSharedRef<TJsonReader<>> IndexReader = TJsonReaderFactory<>::Create(IndexJson);
		if (!FJsonSerializer::Deserialize(IndexReader, IndexObject) || !IndexObject.IsValid())
		{
			return;
		}

		const TArray<TSharedPtr<FJsonValue>>* EntriesArray = nullptr;
		if (!IndexObject->TryGetArrayField(TEXT("entries"), EntriesArray))
		{
			return;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *EntriesArray)
		{
			const TSharedPtr<FJsonObject>* IndexEntryPtr = nullptr;
			if (!EntryValue.IsValid() || !EntryValue->TryGetObject(IndexEntryPtr) || !(*IndexEntryPtr).IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>& IndexEntry = *IndexEntryPtr;
			TSharedPtr<FClaireonFeedbackPanelEntry> Entry = MakeShared<FClaireonFeedbackPanelEntry>();
			Entry->SourceDir = FeedbackDir;

			IndexEntry->TryGetStringField(TEXT("id"), Entry->Id);
			IndexEntry->TryGetStringField(TEXT("textPreview"), Entry->TextPreview);
			IndexEntry->TryGetBoolField(TEXT("isBug"), Entry->bIsBug);
			IndexEntry->TryGetBoolField(TEXT("isFeedback"), Entry->bIsFeedback);
			IndexEntry->TryGetBoolField(TEXT("isSuggestion"), Entry->bIsSuggestion);

			FString TimestampStr;
			if (IndexEntry->TryGetStringField(TEXT("timestamp"), TimestampStr))
			{
				FDateTime::ParseIso8601(*TimestampStr, Entry->Timestamp);
			}

			if (!Entry->Id.IsEmpty())
			{
				Entry->EntryFilePath = FeedbackDir / TEXT("entries") / (Entry->Id + TEXT(".json"));
				FString EntryJson;
				if (FFileHelper::LoadFileToString(EntryJson, *Entry->EntryFilePath))
				{
					Entry->RawJson = EntryJson;

					TSharedPtr<FJsonObject> EntryObject;
					const TSharedRef<TJsonReader<>> EntryReader = TJsonReaderFactory<>::Create(EntryJson);
					if (FJsonSerializer::Deserialize(EntryReader, EntryObject) && EntryObject.IsValid())
					{
						TryGetStringAny(EntryObject, { TEXT("text"), TEXT("feedback") }, Entry->Text);
						TryGetBoolAny(EntryObject, { TEXT("is_bug"), TEXT("isBug") }, Entry->bIsBug);
						TryGetBoolAny(EntryObject, { TEXT("is_feedback"), TEXT("isFeedback") }, Entry->bIsFeedback);
						TryGetBoolAny(EntryObject, { TEXT("is_suggestion"), TEXT("isSuggestion") }, Entry->bIsSuggestion);
					}
				}
			}

			if (Entry->Text.IsEmpty())
			{
				Entry->Text = Entry->TextPreview;
			}
			if (Entry->TextPreview.IsEmpty())
			{
				Entry->TextPreview = MakePreview(Entry->Text);
			}

			Entry->RequestSummary = TEXT("feedback_submit");
			Entry->bFromCurrentSession = CurrentSessionIds.Contains(Entry->Id);
			OutEntries.Add(Entry);
		}
	}
}

// ---------------------------------------------------------------------------
// Tab ID
// ---------------------------------------------------------------------------
const FName SClaireonDiagnosticsWidget::TabId(TEXT("ClaireonDiagnostics"));

// ---------------------------------------------------------------------------
// Tab spawner registration
// ---------------------------------------------------------------------------
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
		.SetDisplayName(LOCTEXT("TabTitle", "Claireon"))
		.SetTooltipText(LOCTEXT("TabTooltip", "Claireon panel -- activity log, feedback, and agent launch"))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetDeveloperToolsMiscCategory())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Comment")));
}

void SClaireonDiagnosticsWidget::UnregisterTabSpawner()
{
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
}

// ---------------------------------------------------------------------------
// Construct
// ---------------------------------------------------------------------------
void SClaireonDiagnosticsWidget::Construct(const FArguments& InArgs)
{
	// Auto-start server
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

	if (Module.IsServerRunning())
	{
		FClaireonServer* Server = Module.GetServer();
		if (Server)
		{
			DiagnosticsEntryDelegateHandle = Server->OnDiagnosticsEntryAdded.AddRaw(
				this, &SClaireonDiagnosticsWidget::OnDiagnosticsEntryAdded);
			RefreshList();

			// Once-per-session spill-retention warning
			static bool bSpillWarningShown = false;
			if (!bSpillWarningShown)
			{
				const UClaireonSettings* SpillSettings = UClaireonSettings::Get();
				if (SpillSettings && !SpillSettings->bKeepResultSpills
					&& SpillSettings->ResultSpillRetentionDays > 0)
				{
					bSpillWarningShown = true;
					Server->PostSystemMessage(FString::Printf(
						TEXT("Spilled results older than %d day(s) will be deleted "
						     "on next message. Set Retention Days = 0 or enable "
						     "Keep Result Spills in Settings (Plugins > Claireon) to disable."),
						SpillSettings->ResultSpillRetentionDays));
				}
			}
		}
	}

	// REPL widget (Chat tab) - only constructed when bEnableREPLChat is set
	const UClaireonSettings* Settings = UClaireonSettings::Get();
	if (Settings && Settings->bEnableREPLChat)
	{
		SAssignNew(REPLWidget, SClaireonREPLWidget, Module.GetServer());
	}

	// Proxy pulse animation
	ProxyPulseSequence = FCurveSequence(0.0f, 0.8f, ECurveEaseFunction::CubicInOut);
	ProxyPulseHandle = ProxyPulseSequence.AddCurve(0.0f, 0.8f, ECurveEaseFunction::CubicInOut);

	// Drive continuous repaint so the status LED ColorAndOpacity re-evaluates each frame
	// during the blink window -- same approach as the toolbar button.
	RegisterActiveTimer(0.05f, FWidgetActiveTimerDelegate::CreateLambda(
		[](double /*InCurrentTime*/, float /*InDeltaTime*/) -> EActiveTimerReturnType
		{
			return EActiveTimerReturnType::Continue;
		}));

	const bool bShowChat = REPLWidget.IsValid();

	ChildSlot
	[
		SNew(SVerticalBox)

		// --- Tab bar ---
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			BuildTabBar()
		]

		// --- Status strip ---
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			BuildStatusStrip()
		]

		// --- Content area ---
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(TabSwitcher, SWidgetSwitcher)

			// Tab 0: Activity
			+ SWidgetSwitcher::Slot()
			[
				BuildActivityTab()
			]

			// Tab 1: Feedback
			+ SWidgetSwitcher::Slot()
			[
				BuildFeedbackTab()
			]

			// Tab 2: Chat (optional)
			+ SWidgetSwitcher::Slot()
			[
				bShowChat
					? REPLWidget.ToSharedRef()
					: StaticCastSharedRef<SWidget>(SNew(SBox))
			]
		]
	];
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------
SClaireonDiagnosticsWidget::~SClaireonDiagnosticsWidget()
{
	FClaireonModule* Module = FModuleManager::GetModulePtr<FClaireonModule>(TEXT("Claireon"));
	if (Module && Module->IsServerRunning())
	{
		if (FClaireonServer* Server = Module->GetServer())
		{
			Server->OnDiagnosticsEntryAdded.Remove(DiagnosticsEntryDelegateHandle);
		}
	}
}

// ---------------------------------------------------------------------------
// Build helpers
// ---------------------------------------------------------------------------
TSharedRef<SWidget> SClaireonDiagnosticsWidget::BuildTabBar()
{
	const UClaireonSettings* Settings = UClaireonSettings::Get();
	const bool bShowChat = Settings && Settings->bEnableREPLChat;

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(4.0f, 2.0f))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.OnClicked_Lambda([this]() -> FReply { SetActiveTab(0); return FReply::Handled(); })
				[
					SAssignNew(ActivityTabLabel, STextBlock)
					.Text(LOCTEXT("ActivityTab", "Activity"))
					.ColorAndOpacity(FSlateColor(ClaireonColors::Fg1))
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.OnClicked_Lambda([this]() -> FReply { SetActiveTab(1); return FReply::Handled(); })
				[
					SAssignNew(FeedbackTabLabel, STextBlock)
					.Text(LOCTEXT("FeedbackTab", "Feedback"))
					.ColorAndOpacity(FSlateColor(ClaireonColors::Fg2))
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.Visibility(bShowChat ? EVisibility::Visible : EVisibility::Collapsed)
				.OnClicked_Lambda([this]() -> FReply { SetActiveTab(2); return FReply::Handled(); })
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ChatTab", "Chat"))
					.ColorAndOpacity(FSlateColor(ClaireonColors::Fg2))
				]
			]

			// Right-side controls
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)

			// Settings gear
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ToolTipText(LOCTEXT("SettingsTooltip", "Open Claireon settings"))
				.OnClicked_Lambda([]() -> FReply
				{
					if (ISettingsModule* SettingsMod =
						FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
					{
						SettingsMod->ShowViewer(TEXT("Editor"), TEXT("Plugins"), TEXT("Claireon"));
					}
					return FReply::Handled();
				})
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush(TEXT("Icons.Settings")))
					.DesiredSizeOverride(FVector2D(14.0f, 14.0f))
					.ColorAndOpacity(FSlateColor(ClaireonColors::Fg2))
				]
			]

			// Server start/stop
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.Text(this, &SClaireonDiagnosticsWidget::GetToggleButtonText)
				.OnClicked(this, &SClaireonDiagnosticsWidget::OnToggleServerClicked)
			]
		];
}

TSharedRef<SWidget> SClaireonDiagnosticsWidget::BuildStatusStrip()
{
	// Flat segmented status strip matching status-strip-flat CSS design tokens.
	// Each non-first segment gets a 1px left border via a thin outer SBorder.
	// Height: 36px, background: BgPanel, bottom: 1px Line1 divider.

	// Helper: wrap a content widget in a padded segment (no left divider -- only the bottom border remains).
	auto WithLeftDivider = [](TSharedRef<SWidget> Content, float HorizPad) -> TSharedRef<SWidget>
	{
		return SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(ClaireonColors::BgPanel)
			.Padding(FMargin(HorizPad, 0.0f))
			[
				Content
			];
	};

	// Helper: stat label+value pair (UPTIME 04:29:55 style)
	auto MakeStat = [](
		const FText& Label,
		TAttribute<FText> Value,
		TAttribute<FSlateColor> ValueColor) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 7))
				.ColorAndOpacity(FSlateColor(ClaireonColors::Fg3))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			.Padding(FMargin(5.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(STextBlock)
				.Text(Value)
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
				.ColorAndOpacity(ValueColor)
			];
	};

	// Proxy-state dot color helper (used in two places)
	auto ProxyDotColor = [this]() -> FSlateColor
	{
		const FClaireonModule& M = FClaireonModule::Get();
		const FClaireonProxyClient* Proxy = M.GetProxyClient();
		if (!Proxy) return FSlateColor(ClaireonColors::Ok);  // direct = green
		switch (Proxy->GetState())
		{
		case EClaireonProxyState::Registered:    return FSlateColor(ClaireonColors::Ok);
		case EClaireonProxyState::RetryRegister: return FSlateColor(ClaireonColors::Warn);
		case EClaireonProxyState::Failed:        return FSlateColor(ClaireonColors::Err);
		default:                                   return FSlateColor(ClaireonColors::Fg3);
		}
	};

	// Strip outer: bottom 1px divider via the outer SBorder color trick
	return SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor(ClaireonColors::Line1)
		.Padding(FMargin(0.0f, 0.0f, 0.0f, 1.0f))  // 1px bottom = line divider
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(ClaireonColors::BgPanel)
			.Padding(FMargin(0.0f))
			[
				SNew(SBox)
				.HeightOverride(36.0f)
				[
					SNew(SHorizontalBox)

					// -- Seg 1: conn dot + "MCP server running" --
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(FMargin(12.0f, 0.0f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
						[
							SNew(SImage)
							.Image(FClaireonToolbarStyle::Get().GetBrush(TEXT("ClaireonToolbar.StatusDot")))
							.DesiredSizeOverride(FVector2D(8.0f, 8.0f))
							.ColorAndOpacity(this, &SClaireonDiagnosticsWidget::GetConnDotColor)
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(this, &SClaireonDiagnosticsWidget::GetServerStateText)
							.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
							.ColorAndOpacity(FSlateColor(ClaireonColors::Fg1))
						]
					]

					// -- Seg 2: proxy chip (clickable, mode text in green, port in gray) --
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						WithLeftDivider(
							SNew(SButton)
							.ButtonStyle(FAppStyle::Get(), "SimpleButton")
							.Visibility(this, &SClaireonDiagnosticsWidget::GetProxyChipVisibility)
							.OnClicked(this, &SClaireonDiagnosticsWidget::OnProxyChipClicked)
							.ContentPadding(FMargin(0.0f))
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
								[
									SNew(STextBlock)
									.Text(this, &SClaireonDiagnosticsWidget::GetProxyChipModeText)
									.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
									.ColorAndOpacity_Lambda(ProxyDotColor)
								]
								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
								[
									SNew(STextBlock)
									.Text(this, &SClaireonDiagnosticsWidget::GetProxyChipPortText)
									.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
									.ColorAndOpacity(FSlateColor(ClaireonColors::Fg3))
								]
							],
							12.0f)
					]

					// -- Seg 2.5: "Claude Code" launch button in accent orange --
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						WithLeftDivider(
							SNew(SButton)
							.ButtonStyle(FAppStyle::Get(), "SimpleButton")
							.Visibility_Lambda([]() -> EVisibility
							{
								return UClaireonSettings::Get()->bShowClaudeCodeButton
									? EVisibility::Visible
									: EVisibility::Collapsed;
							})
							.OnClicked_Lambda([]() -> FReply
							{
								FClaireonModule::LaunchClaudeCode();
								return FReply::Handled();
							})
							.ToolTipText(LOCTEXT("LaunchClaudeCodeTip",
								"Launch Claude Code at the project root"))
							.ContentPadding(FMargin(0.0f))
							[
								SNew(STextBlock)
								.Text(INVTEXT("Claude Code"))
								.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
								.ColorAndOpacity(FSlateColor(ClaireonColors::Accent))
							],
							12.0f)
					]

					// -- Fill --
					+ SHorizontalBox::Slot().FillWidth(1.0f)

					// -- Seg 3: UPTIME --
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Fill)
					[
						WithLeftDivider(
							MakeStat(
								LOCTEXT("UptimeLbl", "UPTIME"),
								TAttribute<FText>::CreateLambda([this]() -> FText
								{
									const FClaireonServer* S = FClaireonModule::Get().GetServer();
									if (!S || !S->IsRunning())
										return FText::FromString(TEXT("--:--:--"));
									FTimespan Up = FDateTime::Now() - S->GetStartTime();
									return FText::FromString(FString::Printf(TEXT("%02d:%02d:%02d"),
										(int32)Up.GetTotalHours(),
										Up.GetMinutes(), Up.GetSeconds()));
								}),
								FSlateColor(ClaireonColors::Fg1)),
							12.0f)
					]

					// -- Seg 4: REQ --
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Fill)
					[
						WithLeftDivider(
							MakeStat(
								LOCTEXT("ReqLbl", "REQ"),
								TAttribute<FText>::CreateLambda([this]() -> FText
								{
									const FClaireonServer* S = FClaireonModule::Get().GetServer();
									if (!S || !S->IsRunning())
										return FText::FromString(TEXT("0"));
									return FText::FromString(
										FString::FromInt(S->GetTotalRequestCount()));
								}),
								FSlateColor(ClaireonColors::Fg1)),
							12.0f)
					]

					// -- Seg 5: ERR --
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Fill)
					[
						WithLeftDivider(
							MakeStat(
								LOCTEXT("ErrLbl", "ERR"),
								TAttribute<FText>::CreateLambda([this]() -> FText
								{
									const FClaireonServer* S = FClaireonModule::Get().GetServer();
									if (!S || !S->IsRunning())
										return FText::FromString(TEXT("0"));
									return FText::FromString(
										FString::FromInt(S->GetErrorCount()));
								}),
								TAttribute<FSlateColor>::CreateLambda([this]() -> FSlateColor
								{
									const FClaireonServer* S = FClaireonModule::Get().GetServer();
									if (S && S->GetErrorCount() > 0)
										return FSlateColor(ClaireonColors::Err);
									return FSlateColor(ClaireonColors::Fg1);
								})),
							12.0f)
					]

					// -- Seg 6: AVG --
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Fill)
					[
						WithLeftDivider(
							MakeStat(
								LOCTEXT("AvgLbl", "AVG"),
								TAttribute<FText>::CreateLambda([this]() -> FText
								{
									const FClaireonServer* S = FClaireonModule::Get().GetServer();
									if (!S || !S->IsRunning())
										return FText::FromString(TEXT("0ms"));
									return FText::FromString(FString::Printf(
										TEXT("%dms"), S->GetAverageDurationMs()));
								}),
								FSlateColor(ClaireonColors::Fg1)),
							12.0f)
					]
				]
			]
		];
}

TSharedRef<SWidget> SClaireonDiagnosticsWidget::BuildActivityTab()
{
	return SNew(SVerticalBox)

	// Toolbar row (search + clear + live/paused)
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(4.0f, 4.0f, 4.0f, 2.0f))
	[
		SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(SEditableTextBox)
			.HintText(LOCTEXT("SearchHint", "Filter by content..."))
			.OnTextChanged(this, &SClaireonDiagnosticsWidget::OnSearchTextChanged)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
		.VAlign(VAlign_Center)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.Text(this, &SClaireonDiagnosticsWidget::GetLivePausedButtonText)
			.OnClicked(this, &SClaireonDiagnosticsWidget::OnLivePausedToggled)
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
		.VAlign(VAlign_Center)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.Text(LOCTEXT("ClearLog", "Clear"))
			.OnClicked(this, &SClaireonDiagnosticsWidget::OnClearLogClicked)
		]
	]

	// Column header row
	+ SVerticalBox::Slot()
	.AutoHeight()
	.Padding(FMargin(4.0f, 0.0f))
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
		.Padding(FMargin(4.0f, 2.0f))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SNew(SBox).WidthOverride(72.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ColTime", "Time"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
					.ColorAndOpacity(FSlateColor(ClaireonColors::Fg3))
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SNew(SBox).WidthOverride(140.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ColMethod", "Method"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
					.ColorAndOpacity(FSlateColor(ClaireonColors::Fg3))
				]
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ColContent", "Content"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
				.ColorAndOpacity(FSlateColor(ClaireonColors::Fg3))
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SBox).WidthOverride(72.0f)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("ColDuration", "Duration"))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
					.ColorAndOpacity(FSlateColor(ClaireonColors::Fg3))
				]
			]
		]
	]

	// Log list + JSON split
	+ SVerticalBox::Slot()
	.FillHeight(1.0f)
	[
		SNew(SSplitter)
		.Orientation(Orient_Vertical)

		// Activity log
		+ SSplitter::Slot()
		.Value(0.55f)
		[
			SAssignNew(ActivityList, SListView<TSharedPtr<FMCPDiagnosticsEntry>>)
			.ListItemsSource(&FilteredItems)
			.OnGenerateRow(this, &SClaireonDiagnosticsWidget::OnGenerateActivityRow)
			.SelectionMode(ESelectionMode::Single)
			.OnSelectionChanged(this, &SClaireonDiagnosticsWidget::OnActivityEntrySelected)
		]

		// Side-by-side JSON panes
		+ SSplitter::Slot()
		.Value(0.45f)
		[
			SNew(SSplitter)
			.Orientation(Orient_Horizontal)

			// Request pane
			+ SSplitter::Slot()
			.Value(0.5f)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(4.0f, 2.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("RequestLabel", "Request"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
						.ColorAndOpacity(FSlateColor(ClaireonColors::Fg2))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "SimpleButton")
						.Text(LOCTEXT("CopyRequest", "Copy"))
						.OnClicked_Lambda([this]() -> FReply
						{
							FPlatformApplicationMisc::ClipboardCopy(*SelectedRequestJson);
							return FReply::Handled();
						})
					]
				]

				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
					.Padding(4.0f)
					[
						SAssignNew(RequestJsonPane, SScrollBox)
					]
				]
			]

			// Response pane
			+ SSplitter::Slot()
			.Value(0.5f)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(4.0f, 2.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ResponseLabel", "Response"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
						.ColorAndOpacity(FSlateColor(ClaireonColors::Fg2))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "SimpleButton")
						.Text(LOCTEXT("CopyResponse", "Copy"))
						.OnClicked_Lambda([this]() -> FReply
						{
							FPlatformApplicationMisc::ClipboardCopy(*SelectedResponseJson);
							return FReply::Handled();
						})
					]
				]

				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
					.Padding(4.0f)
					[
						SAssignNew(ResponseJsonPane, SScrollBox)
					]
				]
			]
		]
	];
}

TSharedRef<SWidget> SClaireonDiagnosticsWidget::BuildFeedbackTab()
{
	return SNew(SVerticalBox)

	// Toolbar: persisted count + scope toggle + export + report
	+ SVerticalBox::Slot()
	.AutoHeight()
	[
		SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor(ClaireonColors::BgPanel)
		.Padding(FMargin(18.0f, 10.0f))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([this]() -> FText
				{
					const FString ScopeText = bFeedbackAllWorktrees
						? TEXT("all worktrees")
						: FClaireonFeedbackLog::Get().GetFeedbackDir();
					return FText::FromString(FString::Printf(
						TEXT("%d persisted entries  |  %s"),
						FeedbackItems.Num(),
						*ScopeText));
				})
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
				.ColorAndOpacity(FSlateColor(ClaireonColors::Fg2))
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.Text(this, &SClaireonDiagnosticsWidget::GetFeedbackScopeButtonText)
				.OnClicked(this, &SClaireonDiagnosticsWidget::OnFeedbackScopeToggled)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(8.0f, 0.0f, 0.0f, 0.0f))
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.Text(LOCTEXT("ExportJsonl", "Export .jsonl"))
				.OnClicked(this, &SClaireonDiagnosticsWidget::OnExportFeedbackJsonlClicked)
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(8.0f, 0.0f, 0.0f, 0.0f))
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonColorAndOpacity(ClaireonColors::Accent)
				.Text(this, &SClaireonDiagnosticsWidget::GetFeedbackReportButtonText)
				.IsEnabled(this, &SClaireonDiagnosticsWidget::IsFeedbackReportEnabled)
				.OnClicked(this, &SClaireonDiagnosticsWidget::OnGenerateFeedbackReportClicked)
			]
		]
	]

	// Two-column split
	+ SVerticalBox::Slot()
	.FillHeight(1.0f)
	[
		SNew(SSplitter)
		.Orientation(Orient_Horizontal)

		// Left: feedback entry list
		+ SSplitter::Slot()
		.Value(0.35f)
		[
			SAssignNew(FeedbackList, SListView<TSharedPtr<FClaireonFeedbackPanelEntry>>)
			.ListItemsSource(&FeedbackItems)
			.OnGenerateRow(this, &SClaireonDiagnosticsWidget::OnGenerateFeedbackRow)
			.SelectionMode(ESelectionMode::Single)
			.OnSelectionChanged(this, &SClaireonDiagnosticsWidget::OnFeedbackEntrySelected)
		]

		// Right: detail view
		+ SSplitter::Slot()
		.Value(0.65f)
		[
			SAssignNew(FeedbackDetailBorder, SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(ClaireonColors::Bg3)
			.Padding(0.0f)
			[
				SAssignNew(FeedbackDetailPane, SScrollBox)
			]
		]
	];
}

// ---------------------------------------------------------------------------
// Tab switching
// ---------------------------------------------------------------------------
void SClaireonDiagnosticsWidget::SetActiveTab(int32 Index)
{
	ActiveTabIndex = Index;
	if (TabSwitcher.IsValid())
	{
		TabSwitcher->SetActiveWidgetIndex(Index);
	}
}

// ---------------------------------------------------------------------------
// Status strip
// ---------------------------------------------------------------------------
FSlateColor SClaireonDiagnosticsWidget::GetConnDotColor() const
{
	// Mirror the toolbar button's StatusDotColorLambda: blink cyan/blue while processing,
	// green when idle/running, red when stopped.
	const FClaireonModule& Module = FClaireonModule::Get();
	const FClaireonServer* Server = Module.GetServer();
	if (!Server || !Server->IsRunning())
	{
		return FSlateColor(FClaireonToolbarStyle::GetStoppedColor());
	}

	const double Now = FPlatformTime::Seconds();
	const double LastRequest = Server->GetLastRequestTime();
	const double MinDuration = FClaireonToolbarStyle::GetProcessingMinDurationSeconds();
	if (LastRequest > 0.0 && (Now - LastRequest) < MinDuration)
	{
		const double HalfPeriod = FClaireonToolbarStyle::GetProcessingBlinkHalfPeriodSeconds();
		const int32 Phase = static_cast<int32>(Now / HalfPeriod) & 1;
		return FSlateColor(Phase == 0
			? FClaireonToolbarStyle::GetProcessingColorA()
			: FClaireonToolbarStyle::GetProcessingColorB());
	}

	return FSlateColor(FClaireonToolbarStyle::GetRunningColor());
}

FText SClaireonDiagnosticsWidget::GetServerStateText() const
{
	const FClaireonModule& Module = FClaireonModule::Get();
	if (!Module.IsServerRunning())
	{
		return LOCTEXT("ServerStopped", "Server stopped");
	}
	return LOCTEXT("ServerRunning", "MCP server running");
}

FText SClaireonDiagnosticsWidget::GetProxyChipText() const
{
	const FClaireonModule& Module = FClaireonModule::Get();
	const FClaireonProxyClient* Proxy = Module.GetProxyClient();

	if (!Proxy)
	{
		// Direct connect mode
		const FClaireonServer* Server = Module.GetServer();
		uint32 Port = Server ? Server->GetPort() : 0;
		return FText::FromString(FString::Printf(TEXT("direct :%u"), Port));
	}

	const FClaireonServer* Server = Module.GetServer();
	uint32 Port = Server ? Server->GetPort() : 0;

	switch (Proxy->GetState())
	{
	case EClaireonProxyState::Registered:
		return FText::FromString(FString::Printf(TEXT("proxy :%u registered"), Port));
	case EClaireonProxyState::RetryRegister:
		return FText::FromString(FString::Printf(TEXT("proxy :%u verifying..."), Port));
	case EClaireonProxyState::Failed:
		return FText::FromString(FString::Printf(TEXT("proxy :%u disconnected -- click to retry"), Port));
	default:
		return FText::FromString(FString::Printf(TEXT("proxy :%u starting..."), Port));
	}
}

FText SClaireonDiagnosticsWidget::GetProxyChipModeText() const
{
	const FClaireonModule& Module = FClaireonModule::Get();
	const FClaireonProxyClient* Proxy = Module.GetProxyClient();
	if (!Proxy) return FText::FromString(TEXT("direct"));
	switch (Proxy->GetState())
	{
	case EClaireonProxyState::Registered:    return FText::FromString(TEXT("proxy registered"));
	case EClaireonProxyState::RetryRegister: return FText::FromString(TEXT("proxy verifying..."));
	case EClaireonProxyState::Failed:        return FText::FromString(TEXT("proxy disconnected -- click to retry"));
	default:                                   return FText::FromString(TEXT("proxy starting..."));
	}
}

FText SClaireonDiagnosticsWidget::GetProxyChipPortText() const
{
	const FClaireonModule& Module = FClaireonModule::Get();
	const FClaireonServer* Server = Module.GetServer();
	uint32 Port = Server ? Server->GetPort() : 0;
	return FText::FromString(FString::Printf(TEXT(" :%u"), Port));
}

EVisibility SClaireonDiagnosticsWidget::GetProxyChipVisibility() const
{
	return FClaireonModule::Get().IsServerRunning()
		? EVisibility::Visible
		: EVisibility::Collapsed;
}

FReply SClaireonDiagnosticsWidget::OnProxyChipClicked()
{
	FClaireonModule& Module = FClaireonModule::Get();
	if (FClaireonProxyClient* Proxy = Module.GetProxyClient())
	{
		Proxy->RequestReconnect();
		StartProxyPulse();
	}
	return FReply::Handled();
}

float SClaireonDiagnosticsWidget::GetProxyPulseOpacity() const
{
	if (!ProxyPulseSequence.IsPlaying())
	{
		return 1.0f;
	}
	float T = ProxyPulseHandle.GetLerp();
	return 0.4f + 0.6f * T;
}

FText SClaireonDiagnosticsWidget::GetStatsSegmentsText() const
{
	const FClaireonModule& Module = FClaireonModule::Get();
	const FClaireonServer* Server = Module.GetServer();
	if (!Server || !Server->IsRunning())
	{
		return FText::GetEmpty();
	}

	FTimespan Uptime = FDateTime::Now() - Server->GetStartTime();
	int32 H = (int32)Uptime.GetTotalHours();
	int32 M = Uptime.GetMinutes();
	int32 S = Uptime.GetSeconds();

	return FText::FromString(FString::Printf(
		TEXT("%02d:%02d:%02d  |  %d requests  |  %d errors  |  avg %dms"),
		H, M, S,
		Server->GetTotalRequestCount(),
		Server->GetErrorCount(),
		Server->GetAverageDurationMs()));
}

// ---------------------------------------------------------------------------
// Proxy pulse animation
// ---------------------------------------------------------------------------
void SClaireonDiagnosticsWidget::StartProxyPulse()
{
	ProxyPulseSequence.Play(AsShared(), true);
}

void SClaireonDiagnosticsWidget::StopProxyPulse()
{
	ProxyPulseSequence.Pause();
}

// ---------------------------------------------------------------------------
// Activity tab - log list rows
// ---------------------------------------------------------------------------
TSharedRef<ITableRow> SClaireonDiagnosticsWidget::OnGenerateActivityRow(
	TSharedPtr<FMCPDiagnosticsEntry> Item,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	if (!Item.IsValid())
	{
		return SNew(STableRow<TSharedPtr<FMCPDiagnosticsEntry>>, OwnerTable);
	}

	const FString TimeStr = Item->Timestamp.ToString(TEXT("%H:%M:%S"));

	// System messages get a distinct compact layout: timestamp + [system] badge + full-width text
	if (Item->bIsSystemMessage)
	{
		TWeakPtr<FMCPDiagnosticsEntry> WeakSysItem = Item;
		return SNew(STableRow<TSharedPtr<FMCPDiagnosticsEntry>>, OwnerTable)
		[
			SNew(SHorizontalBox)
			// Left stripe (suppresses UE selection blue; system rows aren't user-selectable but keep consistent)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox).WidthOverride(2.0f)
				[ SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor::Transparent) ]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor(ClaireonColors::BgRow)
				.Padding(FMargin(0.0f))
				[
					SNew(SHorizontalBox)

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(FMargin(4.0f, 2.0f))
					[
						SNew(SBox).WidthOverride(72.0f)
						[
							SNew(STextBlock)
							.Text(FText::FromString(TimeStr))
							.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
							.ColorAndOpacity(FSlateColor(ClaireonColors::Fg3))
						]
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(FMargin(0.0f, 2.0f, 8.0f, 2.0f))
					[
						SNew(SBox).WidthOverride(140.0f)
						[
							SNew(SBorder)
							.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
							.Padding(FMargin(4.0f, 1.0f))
							[
								SNew(STextBlock)
								.Text(LOCTEXT("SystemBadge", "system"))
								.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
								.ColorAndOpacity(FSlateColor(ClaireonColors::Fg3))
							]
						]
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.Padding(FMargin(0.0f, 2.0f, 4.0f, 2.0f))
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Item->ContentPreview))
						.Font(FCoreStyle::GetDefaultFontStyle("Italic", 9))
						.ColorAndOpacity(FSlateColor(ClaireonColors::Warn))
						.AutoWrapText(true)
					]
				]
			]
		];
	}

	const FString DurStr  = FString::Printf(TEXT("%.0fms"), Item->DurationMs);

	// Method display: ToolName if available, else Method
	FString MethodDisplay = Item->ToolName.IsEmpty()
		? Item->Method
		: Item->ToolName;
	// Strip "claireon_" prefix for brevity
	MethodDisplay.RemoveFromStart(TEXT("claireon_"));

	const FLinearColor MethodColor = Item->bIsFeedbackCall
		? ClaireonColors::Accent
		: (Item->bIsError ? ClaireonColors::Err : ClaireonColors::Info);

	const FLinearColor DurColor = ClaireonColors::DurationColor(Item->DurationMs);

	// Content preview
	const FString Content = Item->ContentPreview.IsEmpty()
		? Item->Method
		: Item->ContentPreview;

	// Capture weak-ptr to item for selection lambda (pointer stable until RefreshList)
	TWeakPtr<FMCPDiagnosticsEntry> WeakItem = Item;

	return SNew(STableRow<TSharedPtr<FMCPDiagnosticsEntry>>, OwnerTable)
	[
		// Outer horizontal box: [2px left stripe] [main content with bg]
		SNew(SHorizontalBox)

		// Left selection stripe (2px, accent orange when selected)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SBox)
			.WidthOverride(2.0f)
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor_Lambda([this, WeakItem]() -> FLinearColor
				{
					TSharedPtr<FMCPDiagnosticsEntry> Pinned = WeakItem.Pin();
					return (Pinned.IsValid() && Pinned == SelectedActivityItem)
						? ClaireonColors::Accent
						: FLinearColor::Transparent;
				})
			]
		]

		// Main row content with selection-aware background
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this, WeakItem]() -> FLinearColor
			{
				TSharedPtr<FMCPDiagnosticsEntry> Pinned = WeakItem.Pin();
				return (Pinned.IsValid() && Pinned == SelectedActivityItem)
					? ClaireonColors::BgRowSel
					: ClaireonColors::BgRow;
			})
			.Padding(FMargin(0.0f))
			[
				SNew(SHorizontalBox)

				// Time
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(FMargin(4.0f, 2.0f))
				[
					SNew(SBox).WidthOverride(72.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TimeStr))
						.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
						.ColorAndOpacity(FSlateColor(ClaireonColors::Fg3))
					]
				]

				// Method badge
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(FMargin(0.0f, 2.0f, 8.0f, 2.0f))
				[
					SNew(SBox).WidthOverride(140.0f)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
						.Padding(FMargin(4.0f, 1.0f))
						[
							SNew(STextBlock)
							.Text(FText::FromString(MethodDisplay))
							.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
							.ColorAndOpacity(FSlateColor(MethodColor))
						]
					]
				]

				// Content (hero column, fills)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(FMargin(0.0f, 2.0f, 8.0f, 2.0f))
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Content))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
					.ColorAndOpacity(FSlateColor(Item->bIsError ? ClaireonColors::Err : ClaireonColors::Fg1))
					.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				]

				// Duration
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(FMargin(0.0f, 2.0f, 4.0f, 2.0f))
				.VAlign(VAlign_Center)
				[
					SNew(SBox).WidthOverride(72.0f)
					[
						SNew(STextBlock)
						.Text(FText::FromString(DurStr))
						.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
						.ColorAndOpacity(FSlateColor(DurColor))
						.Justification(ETextJustify::Right)
					]
				]
			]
		]
	];
}

void SClaireonDiagnosticsWidget::OnActivityEntrySelected(
	TSharedPtr<FMCPDiagnosticsEntry> Item, ESelectInfo::Type /*SelectInfo*/)
{
	SelectedActivityItem = Item;
	if (Item.IsValid())
	{
		PopulateJsonPanes(*Item);
	}
}

void SClaireonDiagnosticsWidget::PopulateJsonPanes(const FMCPDiagnosticsEntry& Entry)
{
	SelectedRequestJson  = PrettyPrintJson(Entry.RequestBody);
	SelectedResponseJson = PrettyPrintJson(Entry.ResponseBody);

	auto FillPane = [](TSharedPtr<SScrollBox> Pane, const FString& PrettyJson)
	{
		if (!Pane.IsValid()) return;
		Pane->ClearChildren();
		Pane->AddSlot()
		[
			SNew(STextBlock)
			.Text(FText::FromString(PrettyJson))
			.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
			.ColorAndOpacity(FSlateColor(ClaireonColors::Fg2))
			.AutoWrapText(true)
		];
	};

	FillPane(RequestJsonPane,  SelectedRequestJson);
	FillPane(ResponseJsonPane, SelectedResponseJson);
}

// ---------------------------------------------------------------------------
// Activity tab - callbacks
// ---------------------------------------------------------------------------
void SClaireonDiagnosticsWidget::OnDiagnosticsEntryAdded(const FMCPDiagnosticsEntry& Entry)
{
	TSharedPtr<FMCPDiagnosticsEntry> Copy = MakeShared<FMCPDiagnosticsEntry>(Entry);
	TWeakPtr<SWidget> WeakSelf = AsShared();

	AsyncTask(ENamedThreads::GameThread, [WeakSelf, Copy]()
	{
		TSharedPtr<SWidget> Pinned = WeakSelf.Pin();
		if (!Pinned.IsValid()) return;

		SClaireonDiagnosticsWidget& Self =
			static_cast<SClaireonDiagnosticsWidget&>(*Pinned);

		Self.ListItems.Add(Copy);
		Self.RememberCurrentSessionFeedbackId(*Copy);
		if (Self.ListItems.Num() > FClaireonServer::MaxDiagnosticsEntries)
		{
			Self.ListItems.RemoveAt(0);
		}

		Self.RebuildFilteredItems();
		Self.RebuildFeedbackItems();

		// Update tab labels
		if (Self.ActivityTabLabel.IsValid())
		{
			Self.ActivityTabLabel->SetText(FText::FromString(
				FString::Printf(TEXT("Activity (%d)"), Self.FilteredItems.Num())));
		}
		if (Self.FeedbackTabLabel.IsValid() && Self.FeedbackItems.Num() > 0)
		{
			Self.FeedbackTabLabel->SetText(FText::FromString(
				FString::Printf(TEXT("Feedback (%d)"), Self.FeedbackItems.Num())));
		}

		if (Self.bIsLive && Self.ActivityList.IsValid())
		{
			Self.ActivityList->RequestListRefresh();
			if (Self.FilteredItems.Num() > 0)
			{
				Self.ActivityList->RequestScrollIntoView(Self.FilteredItems.Last());
			}
		}

		if (Self.FeedbackList.IsValid())
		{
			Self.FeedbackList->RequestListRefresh();
		}
	});
}

void SClaireonDiagnosticsWidget::RefreshList()
{
	ListItems.Empty();

	FClaireonModule& Module = FClaireonModule::Get();
	if (Module.IsServerRunning())
	{
		if (FClaireonServer* Server = Module.GetServer())
		{
			for (const FMCPDiagnosticsEntry& E : Server->GetDiagnosticsEntries())
			{
				ListItems.Add(MakeShared<FMCPDiagnosticsEntry>(E));
				RememberCurrentSessionFeedbackId(E);
			}
		}
	}

	RebuildFilteredItems();
	RebuildFeedbackItems();

	if (ActivityList.IsValid()) ActivityList->RequestListRefresh();
	if (FeedbackList.IsValid()) FeedbackList->RequestListRefresh();
	if (ActivityTabLabel.IsValid())
	{
		ActivityTabLabel->SetText(FText::FromString(
			FString::Printf(TEXT("Activity (%d)"), FilteredItems.Num())));
	}
	if (FeedbackTabLabel.IsValid())
	{
		FeedbackTabLabel->SetText(FText::FromString(
			FString::Printf(TEXT("Feedback (%d)"), FeedbackItems.Num())));
	}
}

void SClaireonDiagnosticsWidget::RebuildFilteredItems()
{
	FilteredItems.Empty();
	for (const TSharedPtr<FMCPDiagnosticsEntry>& Item : ListItems)
	{
		if (SearchFilter.IsEmpty() ||
			Item->ContentPreview.Contains(SearchFilter, ESearchCase::IgnoreCase) ||
			Item->ToolName.Contains(SearchFilter, ESearchCase::IgnoreCase) ||
			Item->Method.Contains(SearchFilter, ESearchCase::IgnoreCase))
		{
			FilteredItems.Add(Item);
		}
	}
}

void SClaireonDiagnosticsWidget::RebuildFeedbackItems()
{
	FeedbackItems.Empty();

	TArray<FString> FeedbackDirs;
	if (bFeedbackAllWorktrees)
	{
		FeedbackDirs = FClaireonFeedbackLog::FindAllWorktreeFeedbackDirs();
	}
	else
	{
		FeedbackDirs.Add(FClaireonFeedbackLog::Get().GetFeedbackDir());
	}

	for (const FString& FeedbackDir : FeedbackDirs)
	{
		ClaireonFeedbackPanel::AppendEntriesFromDir(FeedbackDir, CurrentSessionFeedbackIds, FeedbackItems);
	}

	FeedbackItems.Sort([](const TSharedPtr<FClaireonFeedbackPanelEntry>& A,
		const TSharedPtr<FClaireonFeedbackPanelEntry>& B)
	{
		if (!A.IsValid() || !B.IsValid())
		{
			return A.IsValid();
		}
		if (A->bFromCurrentSession != B->bFromCurrentSession)
		{
			return A->bFromCurrentSession;
		}
		return A->Timestamp > B->Timestamp;
	});
}

void SClaireonDiagnosticsWidget::RememberCurrentSessionFeedbackId(const FMCPDiagnosticsEntry& Entry)
{
	if (!Entry.bIsFeedbackCall)
	{
		return;
	}

	FString EntryId = ClaireonFeedbackPanel::ExtractFeedbackEntryId(Entry.ResponseBody);
	if (EntryId.IsEmpty())
	{
		EntryId = ClaireonFeedbackPanel::ExtractFeedbackEntryId(Entry.RequestBody);
	}
	if (!EntryId.IsEmpty())
	{
		CurrentSessionFeedbackIds.Add(EntryId);
	}
}

void SClaireonDiagnosticsWidget::OnSearchTextChanged(const FText& NewText)
{
	SearchFilter = NewText.ToString();
	RebuildFilteredItems();
	if (ActivityList.IsValid())
	{
		ActivityList->RequestListRefresh();
	}
}

FReply SClaireonDiagnosticsWidget::OnLivePausedToggled()
{
	bIsLive = !bIsLive;
	return FReply::Handled();
}

FText SClaireonDiagnosticsWidget::GetLivePausedButtonText() const
{
	return bIsLive ? LOCTEXT("PauseBtn", "Pause") : LOCTEXT("LiveBtn", "Live");
}

FReply SClaireonDiagnosticsWidget::OnClearLogClicked()
{
	ListItems.Empty();
	FilteredItems.Empty();
	CurrentSessionFeedbackIds.Empty();
	SelectedActivityItem.Reset();

	FClaireonModule& Module = FClaireonModule::Get();
	if (Module.IsServerRunning())
	{
		if (FClaireonServer* Server = Module.GetServer())
		{
			Server->ClearDiagnostics();
		}
	}

	if (ActivityList.IsValid()) ActivityList->RequestListRefresh();
	RebuildFeedbackItems();
	if (FeedbackList.IsValid()) FeedbackList->RequestListRefresh();
	if (ActivityTabLabel.IsValid()) ActivityTabLabel->SetText(LOCTEXT("ActivityTab", "Activity"));
	if (FeedbackTabLabel.IsValid())
	{
		FeedbackTabLabel->SetText(FText::FromString(
			FString::Printf(TEXT("Feedback (%d)"), FeedbackItems.Num())));
	}

	return FReply::Handled();
}

// ---------------------------------------------------------------------------
// Feedback tab
// ---------------------------------------------------------------------------
TSharedRef<ITableRow> SClaireonDiagnosticsWidget::OnGenerateFeedbackRow(
	TSharedPtr<FClaireonFeedbackPanelEntry> Item,
	const TSharedRef<STableViewBase>& OwnerTable)
{
	if (!Item.IsValid())
	{
		return SNew(STableRow<TSharedPtr<FClaireonFeedbackPanelEntry>>, OwnerTable);
	}

	const FString Snippet = Item->TextPreview.IsEmpty()
		? ClaireonFeedbackPanel::MakePreview(Item->Text)
		: Item->TextPreview;
	const FString TypeLabel = Item->bIsBug
		? TEXT("bug")
		: (Item->bIsSuggestion ? TEXT("suggestion") : TEXT("feedback"));
	const FString EntryIdDisplay = Item->Id.IsEmpty() ? TypeLabel : Item->Id;

	// Capture item ID so the selection lambda stays valid across list rebuilds
	FString ItemId = Item->Id;
	const bool bCurrentSession = Item->bFromCurrentSession;

	return SNew(STableRow<TSharedPtr<FClaireonFeedbackPanelEntry>>, OwnerTable)
	[
		// [2px left stripe] [main content bg]
		SNew(SHorizontalBox)

		// Left selection stripe — accent orange when selected, transparent otherwise
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SBox)
			.WidthOverride(2.0f)
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor_Lambda([this, ItemId]() -> FLinearColor
				{
					return (SelectedFeedbackItemId == ItemId)
						? ClaireonColors::Accent
						: FLinearColor::Transparent;
				})
			]
		]

		// Main content with selection-aware background
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor_Lambda([this, ItemId, bCurrentSession]() -> FLinearColor
			{
				// Selected takes priority; otherwise current-session entries use BgRowSel
				// (same warm tint, so the session highlight persists when not clicked)
				if (SelectedFeedbackItemId == ItemId)
					return ClaireonColors::BgRowSel;
				return bCurrentSession ? ClaireonColors::BgRowSel : ClaireonColors::BgRow;
			})
			.Padding(FMargin(14.0f, 10.0f, 14.0f, 12.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 0.0f, 6.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(EntryIdDisplay))
						.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
						.ColorAndOpacity(FSlateColor(ClaireonColors::Fg2))
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					.Padding(FMargin(8.0f, 0.0f, 0.0f, 0.0f))
					[
						SNew(STextBlock)
						.Visibility(bCurrentSession ? EVisibility::Visible : EVisibility::Collapsed)
						.Text(LOCTEXT("CurrentSessionFeedbackTag", "current"))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
						.ColorAndOpacity(FSlateColor(ClaireonColors::AccentHi))
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "SimpleButton")
						.ToolTipText(LOCTEXT("CopyFeedbackTip", "Copy feedback text"))
						.OnClicked_Lambda([TextToCopy = Item->Text]() -> FReply
						{
							FPlatformApplicationMisc::ClipboardCopy(*TextToCopy);
							return FReply::Handled();
						})
						[
							SNew(STextBlock)
							.Text(LOCTEXT("CopyFeedback", "Copy"))
							.ColorAndOpacity(FSlateColor(ClaireonColors::Fg3))
							.Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Snippet))
					.Font(FCoreStyle::GetDefaultFontStyle(bCurrentSession ? "Bold" : "Regular", 10))
					.ColorAndOpacity(FSlateColor(ClaireonColors::Fg1))
					.AutoWrapText(true)
				]
			]
		]
	];
}

void SClaireonDiagnosticsWidget::OnFeedbackEntrySelected(
	TSharedPtr<FClaireonFeedbackPanelEntry> Item, ESelectInfo::Type /*SelectInfo*/)
{
	SelectedFeedbackItemId = Item.IsValid() ? Item->Id : TEXT("");
	if (Item.IsValid())
	{
		PopulateFeedbackDetail(*Item);
	}
}

void SClaireonDiagnosticsWidget::PopulateFeedbackDetail(const FClaireonFeedbackPanelEntry& Entry)
{
	if (!FeedbackDetailPane.IsValid()) return;
	FeedbackDetailPane->ClearChildren();

	FeedbackDetailPane->AddSlot().Padding(FMargin(18.0f, 14.0f, 18.0f, 10.0f))
	[
		SNew(STextBlock)
		.Text(FText::FromString(FString::Printf(TEXT("ENTRY %s  %s"),
			Entry.Id.IsEmpty() ? TEXT("--") : *Entry.Id,
			*Entry.Timestamp.ToString(TEXT("%H:%M:%S")))))
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))
		.ColorAndOpacity(FSlateColor(ClaireonColors::Fg3))
	];

	// Megaphone quote block
	FeedbackDetailPane->AddSlot().Padding(FMargin(18.0f, 8.0f, 18.0f, 18.0f))
	[
		SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor(ClaireonColors::AccentSoft)
		.Padding(FMargin(14.0f))
		[
			SNew(STextBlock)
			.Text(FText::FromString(Entry.Text))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 12))
			.ColorAndOpacity(FSlateColor(ClaireonColors::Fg1))
			.AutoWrapText(true)
		]
	];

	// Metadata k/v grid
	auto AddKV = [this](const FString& Key, const FString& Val)
	{
		FeedbackDetailPane->AddSlot().Padding(FMargin(18.0f, 2.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox).WidthOverride(100.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(Key))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
					.ColorAndOpacity(FSlateColor(ClaireonColors::Fg3))
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Val))
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
				.ColorAndOpacity(FSlateColor(ClaireonColors::Fg2))
			]
		];
	};

	AddKV(TEXT("recorded"),    Entry.Timestamp.ToString(TEXT("%Y-%m-%d %H:%M:%S")));
	AddKV(TEXT("request"),     Entry.RequestSummary);
	AddKV(TEXT("entry id"),    Entry.Id);
	AddKV(TEXT("source"),      Entry.bFromCurrentSession ? TEXT("current session") : TEXT("persisted"));
	AddKV(TEXT("type"),        Entry.bIsBug ? TEXT("bug") : (Entry.bIsSuggestion ? TEXT("suggestion") : TEXT("feedback")));
	AddKV(TEXT("duration"),    FString::Printf(TEXT("%.1fms"), Entry.DurationMs));
	AddKV(TEXT("persisted to"),Entry.EntryFilePath.IsEmpty() ? Entry.SourceDir : Entry.EntryFilePath);

	// Raw request block
	FeedbackDetailPane->AddSlot().Padding(FMargin(18.0f, 18.0f, 18.0f, 0.0f))
	[
		SNew(STextBlock)
		.Text(LOCTEXT("RawFeedbackLabel", "Raw entry"))
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
		.ColorAndOpacity(FSlateColor(ClaireonColors::Fg3))
	];

	FeedbackDetailPane->AddSlot().Padding(FMargin(18.0f, 6.0f, 18.0f, 18.0f))
	[
		SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
		.BorderBackgroundColor(ClaireonColors::BgCode)
		.Padding(12.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Entry.RawJson.IsEmpty() ? Entry.Text : PrettyPrintJson(Entry.RawJson)))
			.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
			.ColorAndOpacity(FSlateColor(ClaireonColors::Fg2))
			.AutoWrapText(true)
		]
	];
}

FReply SClaireonDiagnosticsWidget::OnExportFeedbackJsonlClicked()
{
	// Export persistent feedback log entries to Saved/Claireon/Feedback/YYYY-MM-DD.jsonl.
	// Scope respects the current All Worktrees / This Worktree toggle.
	FString Today = FDateTime::Now().ToString(TEXT("%Y-%m-%d"));
	FString OutPath = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("Claireon"), TEXT("Feedback"),
		Today + TEXT(".jsonl"));

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutPath), true);

	// Collect feedback dirs based on scope
	TArray<FString> FeedbackDirs;
	if (bFeedbackAllWorktrees)
	{
		FeedbackDirs = FClaireonFeedbackLog::FindAllWorktreeFeedbackDirs();
	}
	else
	{
		FeedbackDirs.Add(FClaireonFeedbackLog::Get().GetFeedbackDir());
	}

	FString Lines;
	int32 EntryCount = 0;

	for (const FString& FeedbackDir : FeedbackDirs)
	{
		const FString IndexPath = FeedbackDir / TEXT("index.json");
		FString IndexJson;
		if (!FFileHelper::LoadFileToString(IndexJson, *IndexPath))
		{
			continue;
		}

		TSharedPtr<FJsonObject> IndexObj;
		TSharedRef<TJsonReader<>> IdxReader = TJsonReaderFactory<>::Create(IndexJson);
		if (!FJsonSerializer::Deserialize(IdxReader, IndexObj) || !IndexObj.IsValid())
		{
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>* EntriesArray = nullptr;
		if (!IndexObj->TryGetArrayField(TEXT("entries"), EntriesArray))
		{
			continue;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *EntriesArray)
		{
			const TSharedPtr<FJsonObject>* EntryObjPtr = nullptr;
			if (!EntryValue->TryGetObject(EntryObjPtr) || !(*EntryObjPtr).IsValid())
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& EntryObj = *EntryObjPtr;

			// Try to load full text from the per-entry file
			FString Id;
			EntryObj->TryGetStringField(TEXT("id"), Id);
			FString FullText;
			if (!Id.IsEmpty())
			{
				FString EntryJson;
				const FString EntryPath = FeedbackDir / TEXT("entries") / (Id + TEXT(".json"));
				if (FFileHelper::LoadFileToString(EntryJson, *EntryPath))
				{
					TSharedPtr<FJsonObject> FullObj;
					TSharedRef<TJsonReader<>> EntryReader = TJsonReaderFactory<>::Create(EntryJson);
					if (FJsonSerializer::Deserialize(EntryReader, FullObj) && FullObj.IsValid())
					{
						FullObj->TryGetStringField(TEXT("text"), FullText);
					}
				}
			}
			if (FullText.IsEmpty())
			{
				EntryObj->TryGetStringField(TEXT("textPreview"), FullText);
			}

			// Build output JSONL record: copy index fields + add full text + source dir
			TSharedPtr<FJsonObject> OutObj = MakeShared<FJsonObject>();
			for (const auto& Pair : EntryObj->Values)
			{
				OutObj->SetField(Pair.Key, Pair.Value);
			}
			OutObj->SetStringField(TEXT("text"), FullText);
			OutObj->SetStringField(TEXT("worktreeDir"), FeedbackDir);

			FString Line;
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> W =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Line);
			FJsonSerializer::Serialize(OutObj.ToSharedRef(), W);
			Lines += Line + TEXT("\n");
			++EntryCount;
		}
	}

	if (FFileHelper::SaveStringToFile(Lines, *OutPath))
	{
		// Open Explorer with the exported file selected
		FPlatformProcess::ExploreFolder(*OutPath);

		FNotificationInfo Info(FText::FromString(
			FString::Printf(TEXT("Exported %d entries"), EntryCount)));
		Info.ExpireDuration = 3.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}

	return FReply::Handled();
}

FReply SClaireonDiagnosticsWidget::OnFeedbackScopeToggled()
{
	bFeedbackAllWorktrees = !bFeedbackAllWorktrees;
	RebuildFeedbackItems();
	if (FeedbackList.IsValid())
	{
		FeedbackList->RequestListRefresh();
	}
	if (FeedbackDetailPane.IsValid())
	{
		FeedbackDetailPane->ClearChildren();
	}
	if (FeedbackTabLabel.IsValid())
	{
		FeedbackTabLabel->SetText(FText::FromString(
			FString::Printf(TEXT("Feedback (%d)"), FeedbackItems.Num())));
	}
	return FReply::Handled();
}

FText SClaireonDiagnosticsWidget::GetFeedbackScopeButtonText() const
{
	return bFeedbackAllWorktrees
		? LOCTEXT("ScopeAllWorktrees", "All Worktrees")
		: LOCTEXT("ScopeThisWorktree", "This Worktree");
}

FReply SClaireonDiagnosticsWidget::OnGenerateFeedbackReportClicked()
{
	bGeneratingFeedbackReport = true;
	TWeakPtr<SWidget> WeakSelf = AsShared();

	FClaireonFeedbackReport::Generate(
		bFeedbackAllWorktrees,
		FOnFeedbackReportComplete::CreateLambda([WeakSelf](bool bSuccess, const FString& Message)
	{
		AsyncTask(ENamedThreads::GameThread, [WeakSelf, bSuccess, Message]()
		{
			TSharedPtr<SWidget> Pinned = WeakSelf.Pin();
			if (!Pinned.IsValid()) return;
			SClaireonDiagnosticsWidget& Self =
				static_cast<SClaireonDiagnosticsWidget&>(*Pinned);
			Self.bGeneratingFeedbackReport = false;
			UE_LOG(LogClaireon, Display, TEXT("[MCP] Feedback report: %s"), *Message);
		});
	}));

	return FReply::Handled();
}

FText SClaireonDiagnosticsWidget::GetFeedbackReportButtonText() const
{
	return bGeneratingFeedbackReport
		? LOCTEXT("GeneratingReport", "Generating...")
		: LOCTEXT("GenerateReport", "Feedback Report");
}

bool SClaireonDiagnosticsWidget::IsFeedbackReportEnabled() const
{
	return !bGeneratingFeedbackReport;
}

// ---------------------------------------------------------------------------
// Server toggle
// ---------------------------------------------------------------------------
FReply SClaireonDiagnosticsWidget::OnToggleServerClicked()
{
	FClaireonModule& Module = FClaireonModule::Get();
	if (Module.IsServerRunning())
	{
		if (FClaireonServer* Server = Module.GetServer())
		{
			Server->OnDiagnosticsEntryAdded.Remove(DiagnosticsEntryDelegateHandle);
			DiagnosticsEntryDelegateHandle.Reset();
		}
		Module.StopServer();
	}
	else
	{
		Module.StartServer();
		if (FClaireonServer* Server = Module.GetServer())
		{
			DiagnosticsEntryDelegateHandle = Server->OnDiagnosticsEntryAdded.AddRaw(
				this, &SClaireonDiagnosticsWidget::OnDiagnosticsEntryAdded);
		}
	}
	RefreshList();
	return FReply::Handled();
}

FText SClaireonDiagnosticsWidget::GetToggleButtonText() const
{
	return FClaireonModule::Get().IsServerRunning()
		? LOCTEXT("StopServer", "Stop")
		: LOCTEXT("StartServer", "Start");
}

// ---------------------------------------------------------------------------
// Key handling
// ---------------------------------------------------------------------------
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
	if (InKeyEvent.GetKey() == EKeys::Period && InKeyEvent.IsControlDown())
	{
		HandleGlobalEmergencyStop();
		return FReply::Handled();
	}
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		if (REPLWidget.IsValid() && REPLWidget->HandleEscapeKey())
		{
			return FReply::Handled();
		}
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
FString SClaireonDiagnosticsWidget::PrettyPrintJson(const FString& RawJson)
{
	if (RawJson.IsEmpty()) return TEXT("(empty)");

	TSharedPtr<FJsonObject> Obj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RawJson);
	if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid())
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> W =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
		return Out;
	}
	return RawJson;
}

#undef LOCTEXT_NAMESPACE
