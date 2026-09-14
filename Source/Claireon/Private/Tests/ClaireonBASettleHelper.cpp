// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tests/ClaireonBASettleHelper.h"

#if WITH_CLAIREON_TESTS

#include "ClaireonLog.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GraphEditor.h"
#include "HAL/PlatformTime.h"
#include "TimerManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

#if WITH_BLUEPRINT_ASSIST
#include "BlueprintAssistGraphHandler.h"
#include "BlueprintAssistSettings.h"
#include "BlueprintAssistTabHandler.h"
#include "BlueprintAssistUtils.h"
#include "ClaireonBlueprintAssistCompat.h"
#endif

namespace ClaireonBASettleHelperInternal
{
	// Named, not anonymous: a non-unity build partitions these Tests translation units
	// differently and two anonymous namespaces in the same batch collide.

	static int32 BASettle_TransactionQueueLength()
	{
		if (!IsValid(GEditor))
		{
			return INDEX_NONE;
		}
		const UTransBuffer* TransBuffer = Cast<UTransBuffer>(GEditor->Trans);
		if (!IsValid(TransBuffer))
		{
			return INDEX_NONE;
		}
		return TransBuffer->GetQueueLength();
	}

#if WITH_BLUEPRINT_ASSIST
	// Compile this helper only with its BlueprintAssist caller.
	static void BASettle_SnapshotPositions(const UEdGraph* Graph, TMap<FGuid, FIntPoint>& Out)
	{
		Out.Reset();
		if (!IsValid(Graph))
		{
			return;
		}
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (IsValid(Node))
			{
				Out.Add(Node->NodeGuid, FIntPoint(Node->NodePosX, Node->NodePosY));
			}
		}
	}

	/** Collect top-level windows and native children recursively, including background editors. */
	static void BASettle_CollectWindows(TArray<TSharedRef<SWindow>>& Out)
	{
		TFunction<void(const TSharedRef<SWindow>&)> Descend;
		Descend = [&Out, &Descend](const TSharedRef<SWindow>& Window)
		{
			Out.Add(Window);
			for (const TSharedRef<SWindow>& Child : Window->GetChildWindows())
			{
				Descend(Child);
			}
		};

		for (const TSharedRef<SWindow>& Window : FSlateApplication::Get().GetTopLevelWindows())
		{
			Descend(Window);
		}
	}

	/** Activate the graph's dock tab using BlueprintAssist's supported widget types. */
	static bool BASettle_TryActivateTargetTab(const UEdGraph* Graph, FString& OutDiagnostics)
	{
		TSharedPtr<SDockTab> TargetTab;
		int32 WindowsVisited = 0;
		int32 DockTabsSeen = 0;
		int32 GraphEditorsSeen = 0;
		TArray<FString> OtherGraphs;

		TFunction<void(TSharedPtr<SWidget>)> Visit;
		Visit = [&](TSharedPtr<SWidget> Widget)
		{
			if (TargetTab.IsValid() || !Widget.IsValid())
			{
				return;
			}

			if (TSharedPtr<SDockTab> AsTab = CAST_SLATE_WIDGET(Widget, SDockTab))
			{
				++DockTabsSeen;
				TSharedPtr<SGraphEditor> TabGraphEditor = FBAUtils::GetChildWidgetByTypesCasted<SGraphEditor>(
					AsTab->GetContent(), UBASettings::Get().SupportedGraphEditors);
				if (TabGraphEditor.IsValid())
				{
					++GraphEditorsSeen;
					UEdGraph* Current = TabGraphEditor->GetCurrentGraph();
					if (Current == Graph)
					{
						TargetTab = AsTab;
						return;
					}
					OtherGraphs.AddUnique(IsValid(Current) ? Current->GetName() : TEXT("<null>"));
				}
			}

			if (FChildren* Children = Widget->GetChildren())
			{
				for (int32 I = 0; I < Children->Num() && !TargetTab.IsValid(); ++I)
				{
					Visit(Children->GetChildAt(I));
				}
			}
		};

		TArray<TSharedRef<SWindow>> AllWindows;
		BASettle_CollectWindows(AllWindows);
		for (const TSharedRef<SWindow>& Window : AllWindows)
		{
			++WindowsVisited;
			Visit(Window);
			if (TargetTab.IsValid())
			{
				break;
			}
		}

		if (!TargetTab.IsValid())
		{
			// Distinguish missing UI from a handler focused on another graph.
			OutDiagnostics = FString::Printf(
				TEXT("target tab NOT found (%d window(s), %d dock tab(s), %d graph editor(s); graphs seen: %s)"),
				WindowsVisited, DockTabsSeen, GraphEditorsSeen,
				OtherGraphs.Num() > 0 ? *FString::Join(OtherGraphs, TEXT(", ")) : TEXT("none"));
			return false;
		}

		// DrawAttention foregrounds the tab without requiring OS window focus.
		FGlobalTabmanager::Get()->DrawAttention(TargetTab.ToSharedRef());
		// SetActiveTab drives BA's OnActiveTabChanged subscription, which is what queues
		// ProcessTab. It sets a pointer and broadcasts -- no OS focus involved.
		FGlobalTabmanager::Get()->SetActiveTab(TargetTab);
		// Queue the tab for handler creation on the next engine frame.
		FBATabHandler::Get().ProcessTab(TargetTab);

		OutDiagnostics = FString::Printf(
			TEXT("target tab activated (%d window(s), %d dock tab(s), %d graph editor(s)); foreground=%s"),
			WindowsVisited, DockTabsSeen, GraphEditorsSeen,
			TargetTab->IsForeground() ? TEXT("true") : TEXT("false"));
		return true;
	}
#endif // WITH_BLUEPRINT_ASSIST
} // namespace ClaireonBASettleHelperInternal

namespace ClaireonBASettleHelper
{
	bool IsAvailable()
	{
#if WITH_BLUEPRINT_ASSIST
		return FSlateApplication::IsInitialized();
#else
		return false;
#endif
	}

	void PumpOnce(float DeltaSeconds)
	{
		// Do not tick the core ticker recursively from an MCP call.
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().Tick();
		}

		// The editor timer manager ticks once per frame; it cannot create a BA handler while this tool holds the game thread.
		// Prepare in one call and settle an existing handler in a later call.
		if (IsValid(GEditor))
		{
			GEditor->GetTimerManager()->Tick(DeltaSeconds);
		}

#if WITH_BLUEPRINT_ASSIST
		FBATabHandler::Get().Tick(DeltaSeconds);
#endif
	}

	FSettleReport Settle(UEdGraph* Graph, const FSettleOptions& Options)
	{
		using namespace ClaireonBASettleHelperInternal;

		FSettleReport Report;
#if WITH_BLUEPRINT_ASSIST
		Report.bBlueprintAssistCompiledIn = true;
#endif
		Report.bSlateAvailable = FSlateApplication::IsInitialized();
		Report.TransactionQueueBefore = BASettle_TransactionQueueLength();

		if (!IsValid(Graph))
		{
			Report.Diagnostics = TEXT("no graph supplied");
			return Report;
		}
		Report.NodesBefore = Graph->Nodes.Num();
		Report.NodesAfter = Report.NodesBefore;
		Report.TransactionQueueAfter = Report.TransactionQueueBefore;

#if !WITH_BLUEPRINT_ASSIST
		Report.Diagnostics = TEXT(
			"BlueprintAssist is not compiled into this build (WITH_BLUEPRINT_ASSIST=0), so there is "
			"nothing to settle. A test asserting on BA behavior must FAIL here rather than pass "
			"vacuously -- absence must never become success.");
		return Report;
#else
		if (!Report.bSlateAvailable)
		{
			Report.Diagnostics = TEXT(
				"FSlateApplication is not initialized, so BlueprintAssist cannot run at all. This "
				"suite is EditorContext-only precisely so it is never discovered in a commandlet; "
				"reaching this means it was invoked outside an interactive editor.");
			return Report;
		}

		const float DeltaSeconds = 1.0f / 60.0f;

		TMap<FGuid, FIntPoint> PositionsBefore;
		BASettle_SnapshotPositions(Graph, PositionsBefore);

		FString TabDiagnostics = TEXT("target tab: not searched");
		Report.bTabActivated = BASettle_TryActivateTargetTab(Graph, TabDiagnostics);

		// Retry tab discovery as Slate builds the UI.
		TSharedPtr<FBAGraphHandler> GraphHandler;
		const double HandlerDeadline = FPlatformTime::Seconds() + Options.HandlerTimeoutSeconds;
		while (FPlatformTime::Seconds() < HandlerDeadline)
		{
			PumpOnce(DeltaSeconds);
			++Report.PumpIterations;

			if (!Report.bTabActivated)
			{
				Report.bTabActivated = BASettle_TryActivateTargetTab(Graph, TabDiagnostics);
			}

			GraphHandler = FBATabHandler::Get().GetActiveGraphHandler();
			if (GraphHandler.IsValid() && GraphHandler->GetFocusedEdGraph() == Graph)
			{
				break;
			}
		}

		Report.bHandlerAcquired = GraphHandler.IsValid();
		Report.bIntendedGraphConfirmed =
			GraphHandler.IsValid() && GraphHandler->GetFocusedEdGraph() == Graph;

		if (!Report.bIntendedGraphConfirmed)
		{
			// Only the active graph handler ticks; reject a handler focused elsewhere.
			const UEdGraph* Focused =
				GraphHandler.IsValid() ? GraphHandler->GetFocusedEdGraph() : nullptr;
			Report.Diagnostics = FString::Printf(
				TEXT("no BlueprintAssist handler focused on '%s' within %.1fs. %s. Active handler: %s. ")
				TEXT("Handlers alive: %d. THE FIX IS A CALL BOUNDARY, not a longer wait: BA builds its ")
				TEXT("handler from a timer scheduled with SetTimerForNextTick, and FTimerManager::Tick ")
				TEXT("early-returns for the rest of a frame once UEditorEngine::Tick has ticked it -- ")
				TEXT("which it always has by the time MCP dispatch runs, and GFrameCounter cannot ")
				TEXT("advance while this call holds the game thread. The tab has been activated and ")
				TEXT("enqueued, so the next real engine frame builds the handler; issue another call ")
				TEXT("and settle again."),
				*Graph->GetName(), Options.HandlerTimeoutSeconds, *TabDiagnostics,
				IsValid(Focused) ? *Focused->GetName()
					: (GraphHandler.IsValid() ? TEXT("<handler with no focused graph>") : TEXT("<none>")),
				FBATabHandler::Get().GetAllGraphHandlers().Num());
			Report.NodesAfter = Graph->Nodes.Num();
			Report.TransactionQueueAfter = BASettle_TransactionQueueLength();
			return Report;
		}

		// Wait for BA's node-size readiness signal before measuring layout.
		const double SizeDeadline = FPlatformTime::Seconds() + Options.NodeSizeTimeoutSeconds;
		while (ClaireonBA::IsCalculatingNodeSize(*GraphHandler) && FPlatformTime::Seconds() < SizeDeadline)
		{
			PumpOnce(DeltaSeconds);
			++Report.PumpIterations;
		}
		Report.bNodeSizesSettled = !ClaireonBA::IsCalculatingNodeSize(*GraphHandler);

		if (!Report.bNodeSizesSettled)
		{
			Report.Diagnostics = FString::Printf(
				TEXT("%s; BlueprintAssist was still measuring node sizes after %.1fs"),
				*TabDiagnostics, Options.NodeSizeTimeoutSeconds);
			Report.NodesAfter = Graph->Nodes.Num();
			Report.TransactionQueueAfter = BASettle_TransactionQueueLength();
			return Report;
		}

		if (Options.bFormatAllEvents)
		{
			// FormatAllEvents, never SmartFormatAll -- see the header.
			ClaireonBA::FormatAllEvents(*GraphHandler);
			Report.bFormatRequested = true;
		}

		// PendingFormatting is private; observe stable positions while driving delayed graph detection.
		int32 StableRounds = 0;
		TMap<FGuid, FIntPoint> LastSeen = PositionsBefore;
		const double QuiesceDeadline = FPlatformTime::Seconds() + Options.QuiesceTimeoutSeconds;

		while (FPlatformTime::Seconds() < QuiesceDeadline)
		{
			PumpOnce(DeltaSeconds);
			++Report.PumpIterations;

			if (ClaireonBA::IsCalculatingNodeSize(*GraphHandler))
			{
				// A newly measured node can still move things; not settled yet.
				StableRounds = 0;
				continue;
			}

			bool bChanged = false;
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (!IsValid(Node))
				{
					continue;
				}
				const FIntPoint Current(Node->NodePosX, Node->NodePosY);
				const FIntPoint* Previous = LastSeen.Find(Node->NodeGuid);
				if (!Previous || *Previous != Current)
				{
					bChanged = true;
					LastSeen.Add(Node->NodeGuid, Current);
				}
			}

			StableRounds = bChanged ? 0 : StableRounds + 1;
			if (StableRounds >= Options.RequiredStableRounds)
			{
				break;
			}
		}

		Report.bPositionsSettled = StableRounds >= Options.RequiredStableRounds;

		Report.NodesAfter = Graph->Nodes.Num();
		Report.TransactionQueueAfter = BASettle_TransactionQueueLength();
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node))
			{
				continue;
			}
			if (const FIntPoint* Before = PositionsBefore.Find(Node->NodeGuid))
			{
				if (*Before != FIntPoint(Node->NodePosX, Node->NodePosY))
				{
					++Report.NodesMoved;
				}
			}
		}

		Report.Diagnostics = FString::Printf(
			TEXT("%s; handler focused on '%s'; pumps=%d; sizes_settled=%s; positions_settled=%s; ")
			TEXT("nodes %d -> %d; moved=%d; transactions %d -> %d"),
			*TabDiagnostics, *Graph->GetName(), Report.PumpIterations,
			Report.bNodeSizesSettled ? TEXT("true") : TEXT("false"),
			Report.bPositionsSettled ? TEXT("true") : TEXT("false"),
			Report.NodesBefore, Report.NodesAfter, Report.NodesMoved,
			Report.TransactionQueueBefore, Report.TransactionQueueAfter);

		UE_LOG(LogClaireon, Display, TEXT("[BASettle] %s"), *Report.Diagnostics);
		return Report;
#endif // WITH_BLUEPRINT_ASSIST
	}
} // namespace ClaireonBASettleHelper

#endif // WITH_CLAIREON_TESTS
