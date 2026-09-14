// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Compare grouped and ungrouped extraction against transaction baselines.
// Observe only the operation-local queue suffix; a shortened queue invalidates that window.
// The shared BA fixture needs engine frames for cold handler creation. Each test reshapes
// it before measuring and reports an unready handler as failure.

#include "Misc/AutomationTest.h"

#if WITH_CLAIREON_TESTS && WITH_EDITOR

#include "Tests/ClaireonBASettleHelper.h"
#include "Tests/ClaireonBPEditorFixtures.h"
#include "Tests/ClaireonBPEditorTransactionBaselines.h"
#include "Tools/ClaireonBlueprintGraphTool_Extract.h"
#include "Tools/ClaireonTool_TransactionBeginGroup.h"
#include "Tools/ClaireonTool_TransactionEndGroup.h"
#include "Tools/ClaireonTool_TransactionUndo.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "Interfaces/IPluginManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_CustomEvent.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/IToolkit.h"
#include "Toolkits/IToolkitHost.h"

namespace ClaireonBPEditorTxnInternal
{
	// Prefix helpers to avoid unity-build collisions.

	/** Use the persistent shared BA fixture and its open editor. */
	static FString TXN_FixturePath()
	{
		return ClaireonBPEditorFixtures::SharedBlueprintAssistFixturePath();
	}

	static UBlueprint* TXN_ResolveFixture()
	{
		return Cast<UBlueprint>(ClaireonBPEditorFixtures::Resolve(TXN_FixturePath()));
	}


	struct FTxnWindow
	{
		int32 StartLength = INDEX_NONE;
		int32 StartUndoCount = INDEX_NONE;

		/** Record existing transaction IDs so an unpurged undone tail cannot be attributed to this operation. */
		TSet<FGuid> PreExisting;
	};

	static UTransBuffer* TXN_Buffer()
	{
		return IsValid(GEditor) ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
	}

	static bool TXN_OpenWindow(FTxnWindow& Out, FString& OutError)
	{
		UTransBuffer* Buffer = TXN_Buffer();
		if (!IsValid(Buffer))
		{
			OutError = TEXT("GEditor->Trans is not a UTransBuffer, so no transaction can be observed. "
				"This suite is EditorContext-only precisely so that cannot happen.");
			return false;
		}
		Out.StartLength = Buffer->GetQueueLength();
		Out.StartUndoCount = Buffer->GetUndoCount();
		Out.PreExisting.Reset();
		Out.PreExisting.Reserve(Out.StartLength);
		for (int32 Index = 0; Index < Out.StartLength; ++Index)
		{
			if (const FTransaction* Transaction = Buffer->GetTransaction(Index))
			{
				Out.PreExisting.Add(Transaction->GetId());
			}
		}
		return true;
	}

	/** Begin purges the undone tail; use queue length minus undo count as the window start. */
	static int32 TXN_EffectiveStart(const FTxnWindow& Window)
	{
		return FMath::Max(0, Window.StartLength - FMath::Max(0, Window.StartUndoCount));
	}

	/** The operation-local suffix: every entry appended since the window opened. */
	static bool TXN_ReadWindow(const FTxnWindow& Window, TArray<FString>& OutTitles, FString& OutError)
	{
		OutTitles.Reset();
		UTransBuffer* Buffer = TXN_Buffer();
		if (!IsValid(Buffer))
		{
			OutError = TEXT("the transaction buffer went away mid-operation.");
			return false;
		}
		const int32 Length = Buffer->GetQueueLength();
		const int32 Start = TXN_EffectiveStart(Window);
		if (Length < Start)
		{
			// Reject a window shortened beyond undo purging, which indicates trimming or reset.
			OutError = FString::Printf(
				TEXT("the transaction queue is %d entries long but the operation-local suffix was ")
				TEXT("expected to start at %d (queue was %d with %d undone when the window opened). ")
				TEXT("The undo buffer was trimmed or reset; this is a measurement failure, not a ")
				TEXT("characterization."),
				Length, Start, Window.StartLength, Window.StartUndoCount);
			return false;
		}
		for (int32 Index = Start; Index < Length; ++Index)
		{
			const FTransaction* Transaction = Buffer->GetTransaction(Index);
			// Exclude pre-existing IDs even if the operation opened no transaction and left the undone tail intact.
			if (Transaction != nullptr && Window.PreExisting.Contains(Transaction->GetId()))
			{
				continue;
			}
			OutTitles.Add(Transaction ? Transaction->GetTitle().ToString() : TEXT("<null transaction>"));
		}
		return true;
	}

	static FString TXN_FormatSequence(const TArray<FString>& Titles)
	{
		if (Titles.Num() == 0)
		{
			return TEXT("(empty)");
		}
		TArray<FString> Quoted;
		for (const FString& Title : Titles)
		{
			Quoted.Add(FString::Printf(TEXT("TEXT(\"%s\"),"), *Title));
		}
		return FString::Join(Quoted, TEXT(" "));
	}


	struct FCanonicalNodes
	{
		UK2Node_CustomEvent* Entry = nullptr;
		UK2Node_CallFunction* Call = nullptr;
	};

	/** Reset to an event calling PrintString; RemoveNode also removes a composite gateway's BoundGraph. */
	static bool TXN_ResetToCanonical(UBlueprint* Blueprint, UEdGraph* Graph,
		FCanonicalNodes& Out, FString& OutError)
	{
		if (!IsValid(Blueprint) || !IsValid(Graph))
		{
			OutError = TEXT("no fixture Blueprint or graph to reset.");
			return false;
		}

		TArray<UEdGraphNode*> Existing = Graph->Nodes;
		for (UEdGraphNode* Node : Existing)
		{
			if (!IsValid(Node))
			{
				continue;
			}
			// Undo can restore graph membership without restoring Outer. Remove such nodes directly from this graph.
			if (Node->GetOuter() == Graph)
			{
				FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile=*/true);
			}
			else
			{
				Graph->RemoveNode(Node);
			}
		}
		if (Graph->Nodes.Num() != 0)
		{
			OutError = FString::Printf(TEXT("the reset left %d node(s) in '%s'."),
				Graph->Nodes.Num(), *Graph->GetName());
			return false;
		}

		UFunction* PrintString = UKismetSystemLibrary::StaticClass()->FindFunctionByName(
			TEXT("PrintString"));
		if (PrintString == nullptr)
		{
			OutError = TEXT("UKismetSystemLibrary::PrintString could not be resolved, so the canonical "
				"graph cannot be built. This is an engine-side change, not a fixture problem.");
			return false;
		}

		// Set the function reference before allocating call pins.
		UK2Node_CustomEvent* Entry = NewObject<UK2Node_CustomEvent>(Graph);
		Graph->AddNode(Entry, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Entry->CreateNewGuid();
		Entry->CustomFunctionName = TEXT("CanonicalEntry");
		Entry->NodePosX = 0;
		Entry->NodePosY = 0;
		Entry->AllocateDefaultPins();

		UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
		Graph->AddNode(Call, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Call->CreateNewGuid();
		Call->SetFromFunction(PrintString);
		Call->NodePosX = 400;
		Call->NodePosY = 0;
		Call->AllocateDefaultPins();

		UEdGraphPin* Then = Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		UEdGraphPin* Exec = Call->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		if (Then == nullptr || Exec == nullptr)
		{
			OutError = FString::Printf(
				TEXT("the canonical graph could not be wired: %s pin missing."),
				Then == nullptr ? TEXT("event Then") : TEXT("call Execute"));
			return false;
		}
		Then->MakeLinkTo(Exec);

		Graph->NotifyGraphChanged();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Out.Entry = Entry;
		Out.Call = Call;
		return true;
	}

	/**
	 * Rebuild editor widgets after node reparenting before pumping Slate.
	 * NotifyGraphChanged alone does not broadcast the structural change needed by the editor.
	 */
	static void TXN_RefreshOpenEditor(UBlueprint* Blueprint)
	{
		if (IsValid(Blueprint))
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
	}

	/** Count gateways because extraction replaces nodes without necessarily changing node count. */
	static int32 TXN_CountComposites(const UEdGraph* Graph)
	{
		int32 Count = 0;
		if (IsValid(Graph))
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (Cast<const UK2Node_Composite>(Node) != nullptr)
				{
					++Count;
				}
			}
		}
		return Count;
	}

	/** Count nodes listed in a graph with a different Outer; painting this inconsistent state can fault. */
	static int32 TXN_CountForeignOuterNodes(const UEdGraph* Graph)
	{
		int32 Count = 0;
		if (IsValid(Graph))
		{
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (IsValid(Node) && Node->GetOuter() != Graph)
				{
					++Count;
				}
			}
		}
		return Count;
	}

	/** One node added with no transaction of its own, for the negative half of test 1. */
	static UEdGraphNode* TXN_AddProbeNode(UEdGraph* Graph)
	{
		if (!IsValid(Graph))
		{
			return nullptr;
		}
		UFunction* PrintString = UKismetSystemLibrary::StaticClass()->FindFunctionByName(
			TEXT("PrintString"));
		if (PrintString == nullptr)
		{
			return nullptr;
		}
		UK2Node_CallFunction* Probe = NewObject<UK2Node_CallFunction>(Graph);
		Graph->AddNode(Probe, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Probe->CreateNewGuid();
		Probe->SetFromFunction(PrintString);
		Probe->NodePosX = 400;
		Probe->NodePosY = 400;
		Probe->AllocateDefaultPins();
		// Notify BA of the added node without recompiling inside the measurement window.
		Graph->NotifyGraphChanged();
		return Probe;
	}


	static bool TXN_CheckProvenance(FAutomationTestBase& Test)
	{
		const TSharedPtr<IPlugin> BAPlugin = IPluginManager::Get().FindPlugin(TEXT("BlueprintAssist"));
		if (!BAPlugin.IsValid())
		{
			Test.AddError(TEXT("BlueprintAssist is not a discoverable plugin, so these baselines cannot "
				"be attributed to a BA build. A test asserting on BA behaviour must fail here rather "
				"than pass vacuously."));
			return false;
		}

		const FString Observed = BAPlugin->GetDescriptor().VersionName;
		FString Expected;
		bool bCharacterized = false;
		for (const TCHAR* Version : ClaireonBPEditorBaselines::BlueprintAssistVersionNames)
		{
			bCharacterized |= Observed.Equals(Version, ESearchCase::CaseSensitive);
			Expected += Expected.IsEmpty() ? Version : FString(TEXT(", ")) + Version;
		}
		if (!bCharacterized)
		{
			Test.AddError(FString::Printf(
				TEXT("BlueprintAssist version is '%s' but these baselines were characterized against ")
				TEXT("'%s'. A BA upgrade can change delayed-transaction timing, node-size settlement or ")
				TEXT("public API availability with NO change to Claireon source, so it must fail ")
				TEXT("visibly rather than be absorbed. Re-characterize deliberately: run this test, ")
				TEXT("read the observed sequence out of its failure message, and edit BOTH the sequence ")
				TEXT("AND BlueprintAssistVersionNames in ClaireonBPEditorTransactionBaselines.h in one ")
				TEXT("reviewed commit (append the version only if the sequence is unchanged)."),
				*Observed, *Expected));
			return false;
		}

		Test.AddInfo(FString::Printf(
			TEXT("provenance: BlueprintAssist %s; engine build now '%s', baseline recorded '%s'; %s"),
			*Observed, FApp::GetBuildVersion(), ClaireonBPEditorBaselines::EngineBuildVersion,
			ClaireonBPEditorBaselines::CharacterizedBy));
		if (!FString(FApp::GetBuildVersion()).Equals(ClaireonBPEditorBaselines::EngineBuildVersion))
		{
			Test.AddWarning(FString::Printf(
				TEXT("engine build is '%s' but the baseline was recorded on '%s'. Not a failure: the "
					 "sequence itself is the claim. If the sequence also changed, treat the engine bump "
					 "as the likely cause and re-characterize deliberately."),
				FApp::GetBuildVersion(), ClaireonBPEditorBaselines::EngineBuildVersion));
		}
		return true;
	}

	/** Report baseline mismatches without automatically updating them. */
	static void TXN_CompareToBaseline(FAutomationTestBase& Test, const TCHAR* Label,
		const TCHAR* const* Expected, int32 ExpectedNum, const TArray<FString>& Observed)
	{
		const bool bUncharacterized = (ExpectedNum == 1)
			&& FString(Expected[0]).Equals(ClaireonBPEditorBaselines::Uncharacterized);
		if (bUncharacterized)
		{
			Test.AddError(FString::Printf(
				TEXT("%s has NO committed baseline yet (it still carries the %s sentinel). Observed ")
				TEXT("sequence, %d entr(y/ies): %s -- paste it into ")
				TEXT("ClaireonBPEditorTransactionBaselines.h. A placeholder baseline must never read as ")
				TEXT("a pass."),
				Label, ClaireonBPEditorBaselines::Uncharacterized, Observed.Num(),
				*TXN_FormatSequence(Observed)));
			return;
		}

		bool bMatches = (Observed.Num() == ExpectedNum);
		for (int32 Index = 0; bMatches && Index < ExpectedNum; ++Index)
		{
			bMatches = Observed[Index].Equals(Expected[Index], ESearchCase::CaseSensitive);
		}

		if (bMatches)
		{
			Test.AddInfo(FString::Printf(TEXT("%s matches its committed baseline (%d entr(y/ies): %s)"),
				Label, Observed.Num(), *TXN_FormatSequence(Observed)));
			return;
		}

		TArray<FString> ExpectedList;
		for (int32 Index = 0; Index < ExpectedNum; ++Index)
		{
			ExpectedList.Add(Expected[Index]);
		}
		Test.AddError(FString::Printf(
			TEXT("%s DIFFERS from its committed baseline.\n  EXPECTED (%d): %s\n  OBSERVED (%d): %s\n")
			TEXT("This is a behavioural change, not a broken test. Do NOT regenerate it as a matter of ")
			TEXT("routine: decide whether the new sequence is correct, then edit ")
			TEXT("ClaireonBPEditorTransactionBaselines.h deliberately so the diff shows the old and new ")
			TEXT("sequences plus the BlueprintAssist version."),
			Label, ExpectedNum, *TXN_FormatSequence(ExpectedList),
			Observed.Num(), *TXN_FormatSequence(Observed)));
	}


	/**
	 * Acquire and settle a BA handler, or prepare the editor and request a later invocation.
	 * Separate creation/save from editor opening with latent updates.
	 */
	class FTXN_PrimedGraphCommand : public IAutomationLatentCommand
	{
	public:
		FTXN_PrimedGraphCommand(FAutomationTestBase* InTest, double InSettleBudgetSeconds)
			: Test(InTest)
			, SettleBudgetSeconds(InSettleBudgetSeconds)
		{
		}

		virtual bool Update() override
		{
			switch (Phase)
			{
			case EPhase::TrySettle:
			{
				UBlueprint* Blueprint = TXN_ResolveFixture();
				UEdGraph* Graph = ClaireonBPEditorFixtures::FirstUbergraph(Blueprint);
				if (IsValid(Graph))
				{
					// Refresh the shared fixture before pumping any panel left stale by a prior test.
					if (!bRefreshedBeforeFirstSettle)
					{
						bRefreshedBeforeFirstSettle = true;
						TXN_RefreshOpenEditor(Blueprint);
					}
					ClaireonBASettleHelper::FSettleOptions Options;
					Options.HandlerTimeoutSeconds = 2.0;
					const ClaireonBASettleHelper::FSettleReport Report =
						ClaireonBASettleHelper::Settle(Graph, Options);

					if (Report.Settled())
					{
						Test->TestTrue(TEXT("the settle drove the graph the test asked for"),
							Report.bIntendedGraphConfirmed);
						OnSettled(Blueprint, Graph);
						return true;
					}

					if (!Report.bBlueprintAssistCompiledIn || !Report.bSlateAvailable)
					{
						Test->AddError(Report.Diagnostics);
						return true;
					}
					LastDiagnostics = Report.Diagnostics;
				}
				else
				{
					LastDiagnostics = TEXT("fixture absent");
				}

				if (GetCurrentRunTime() > SettleBudgetSeconds)
				{
					Phase = EPhase::Prepare;
				}
				return false;
			}

			case EPhase::Prepare:
			{
				const FString AssetPath = TXN_FixturePath();
				if (!IsValid(ClaireonBPEditorFixtures::Resolve(AssetPath)))
				{
					FString CreateError;
					UBlueprint* Created = ClaireonBPEditorFixtures::Create(AssetPath, CreateError);
					if (!IsValid(Created))
					{
						Test->AddError(FString::Printf(
							TEXT("could not create the operation fixture at %s: %s"),
							*AssetPath, *CreateError));
						return true;
					}
					if (!ClaireonBPEditorFixtures::Save(Created))
					{
						Test->AddWarning(FString::Printf(
							TEXT("the operation fixture at %s could not be saved; it will be left as an "
								 "unsaved dirty package, which raises the Restore-Packages modal on the "
								 "next editor launch and wedges the MCP proxy."), *AssetPath));
					}
					PreparedNote = TEXT("Created and saved the fixture");
					Phase = EPhase::Idle;
					return false;
				}
				Phase = EPhase::Open;
				return false;
			}

			case EPhase::Idle:
				if (++IdleUpdates < 3)
				{
					return false;
				}
				Phase = EPhase::Open;
				return false;

			case EPhase::Open:
			{
				UObject* Asset = ClaireonBPEditorFixtures::Resolve(TXN_FixturePath());
				UAssetEditorSubsystem* Subsystem = IsValid(GEditor)
					? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
					: nullptr;
				if (!IsValid(Asset) || !IsValid(Subsystem))
				{
					Test->AddError(TEXT("the operation fixture or the asset-editor subsystem went away."));
					return true;
				}

				// Open synchronously without a progress dialog.
				Subsystem->OpenEditorForAsset(Asset, EToolkitMode::Standalone,
					TSharedPtr<IToolkitHost>(), /*bShowProgressWindow*/ false);

				// Queue the graph tab for handler creation on the next real frame.
				UBlueprint* Blueprint = TXN_ResolveFixture();
				if (UEdGraph* Graph = ClaireonBPEditorFixtures::FirstUbergraph(Blueprint); IsValid(Graph))
				{
					ClaireonBASettleHelper::FSettleOptions Options;
					Options.HandlerTimeoutSeconds = 2.0;
					const ClaireonBASettleHelper::FSettleReport Report =
						ClaireonBASettleHelper::Settle(Graph, Options);
					LastDiagnostics = Report.Diagnostics;
					if (Report.Settled())
					{
						OnSettled(Blueprint, Graph);
						return true;
					}
				}

				Test->AddError(FString::Printf(
					TEXT("PREPARED, NOT SETTLED. %s at %s, opened its editor, and activated and ")
					TEXT("enqueued its graph tab. BlueprintAssist cannot build a graph handler inside a ")
					TEXT("tool call: its handler comes from a SetTimerForNextTick timer, and ")
					TEXT("FTimerManager::Tick early-returns for the rest of a frame once ")
					TEXT("UEditorEngine::Tick has ticked it -- which it always has by the time MCP ")
					TEXT("dispatch runs -- while GFrameCounter cannot advance during this call. ")
					TEXT("RE-RUN this suite: the intervening engine frames build the handler and the ")
					TEXT("second run settles. Last settle: %s"),
					PreparedNote.IsEmpty() ? TEXT("Reused the existing fixture") : *PreparedNote,
					*TXN_FixturePath(), *LastDiagnostics));
				return true;
			}

			default:
				return true;
			}
		}

	protected:
		/** Called once, with a BA handler confirmed on Graph and quiesced. */
		virtual void OnSettled(UBlueprint* Blueprint, UEdGraph* Graph) = 0;

		FAutomationTestBase* Test = nullptr;

	private:
		enum class EPhase : uint8
		{
			TrySettle,
			Prepare,
			Idle,
			Open
		};

		FString LastDiagnostics;
		FString PreparedNote;
		double SettleBudgetSeconds = 10.0;
		EPhase Phase = EPhase::TrySettle;
		int32 IdleUpdates = 0;
		bool bRefreshedBeforeFirstSettle = false;
	};

	/** Settle after mutation; callers explicitly choose whether to request FormatAllEvents. */
	static bool TXN_SettleAgain(FAutomationTestBase& Test, UEdGraph* Graph, const TCHAR* What,
		bool bFormatAllEvents, ClaireonBASettleHelper::FSettleReport* OutReport = nullptr)
	{
		ClaireonBASettleHelper::FSettleOptions Options;
		Options.HandlerTimeoutSeconds = 5.0;
		Options.bFormatAllEvents = bFormatAllEvents;
		const ClaireonBASettleHelper::FSettleReport Report = ClaireonBASettleHelper::Settle(Graph, Options);
		if (OutReport != nullptr)
		{
			*OutReport = Report;
		}
		if (!Report.Settled())
		{
			Test.AddError(FString::Printf(TEXT("the %s settle did not complete: %s"), What, *Report.Diagnostics));
			return false;
		}
		Test.AddInfo(FString::Printf(TEXT("%s settle: %s"), What, *Report.Diagnostics));
		return true;
	}

	/** bp_extract_composite on exactly one node. */
	static bool TXN_ExtractComposite(FAutomationTestBase& Test, const FString& AssetPath,
		const FString& GraphName, const FGuid& NodeGuid, const FString& NewName)
	{
		ClaireonBlueprintGraphTool_ExtractComposite Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("graph_name"), GraphName);
		Args->SetStringField(TEXT("new_name"), NewName);
		Args->SetStringField(TEXT("response_mode"), TEXT("status"));
		TArray<TSharedPtr<FJsonValue>> Guids;
		Guids.Add(MakeShared<FJsonValueString>(NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)));
		Args->SetArrayField(TEXT("node_guids"), Guids);

		const IClaireonTool::FToolResult Result = Tool.Execute(Args);
		if (Result.bIsError)
		{
			Test.AddError(FString::Printf(TEXT("bp_extract_composite failed: %s"), *Result.ErrorMessage));
			return false;
		}
		return true;
	}
}

// Compare the same mutation before and after explicit settlement.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorFormatNodeAddedRequiresTheSettle,
	"Claireon.BPEditor.BlueprintAssist.FormatNodeAddedRequiresTheSettle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorTxnInternal
{
	class FTXN_FormatNodeAddedCommand : public FTXN_PrimedGraphCommand
	{
	public:
		using FTXN_PrimedGraphCommand::FTXN_PrimedGraphCommand;

	protected:
		virtual void OnSettled(UBlueprint* Blueprint, UEdGraph* Graph) override
		{
			static const FString FormatNodeAdded = TEXT("Format Node Added");

			if (!TXN_CheckProvenance(*Test))
			{
				return;
			}

			FCanonicalNodes Canonical;
			FString Error;
			if (!TXN_ResetToCanonical(Blueprint, Graph, Canonical, Error))
			{
				Test->AddError(Error);
				return;
			}
			// Settle fixture reset before opening the measurement window.
			if (!TXN_SettleAgain(*Test, Graph, TEXT("post-reset"), /*bFormatAllEvents=*/false))
			{
				return;
			}

			// Read without pumping Slate while the tool holds the game thread, so BA has no opportunity to advance.
			FTxnWindow Window;
			if (!TXN_OpenWindow(Window, Error))
			{
				Test->AddError(Error);
				return;
			}

			UEdGraphNode* Probe = TXN_AddProbeNode(Graph);
			if (!IsValid(Probe))
			{
				Test->AddError(TEXT("the probe node could not be added, so neither half is measurable."));
				return;
			}

			TArray<FString> WithoutSettle;
			if (!TXN_ReadWindow(Window, WithoutSettle, Error))
			{
				Test->AddError(Error);
				return;
			}
			const bool bNothingWithoutSettle = (WithoutSettle.Num() == 0);
			Test->TestTrue(
				FString::Printf(
					TEXT("WITHOUT the settle, adding a node pushes NO transaction at all (observed: %s)"),
					*TXN_FormatSequence(WithoutSettle)),
				bNothingWithoutSettle);

			// Observe node movement after settlement; this configuration can move nodes without surviving transactions.
			ClaireonBASettleHelper::FSettleReport ProbeReport;
			if (!TXN_SettleAgain(*Test, Graph, TEXT("probe"), /*bFormatAllEvents=*/true, &ProbeReport))
			{
				return;
			}

			const bool bBaMovedNodes = (ProbeReport.NodesMoved > 0);
			Test->TestTrue(
				FString::Printf(
					TEXT("WITH the settle, BlueprintAssist repositioned %d node(s) -- inside a tool call ")
					TEXT("only this helper can make that happen"),
					ProbeReport.NodesMoved),
				bBaMovedNodes);

			Test->TestTrue(
				TEXT("it is the settle that drove BlueprintAssist, not merely something that happened "
					 "alongside it"),
				bNothingWithoutSettle && bBaMovedNodes);

			TArray<FString> WithSettle;
			if (!TXN_ReadWindow(Window, WithSettle, Error))
			{
				Test->AddError(Error);
				return;
			}
			Test->TestEqual(
				FString::Printf(
					TEXT("the number of transactions the settle leaves in the queue matches the ")
					TEXT("committed baseline (observed: %s)"),
					*TXN_FormatSequence(WithSettle)),
				WithSettle.Num(), ClaireonBPEditorBaselines::SettleAfterNodeAddTransactionCount);

			const bool bFormatNodeAddedSeen = WithSettle.Contains(FormatNodeAdded);
			Test->TestTrue(
				FString::Printf(
					TEXT("whether BA's '%s' transaction survives into the queue matches the committed ")
					TEXT("baseline (baseline says %s, observed %s)"),
					*FormatNodeAdded,
					ClaireonBPEditorBaselines::bFormatNodeAddedTransactionSurvives
						? TEXT("survives") : TEXT("discarded as transient"),
					bFormatNodeAddedSeen ? TEXT("survives") : TEXT("discarded as transient")),
				bFormatNodeAddedSeen == ClaireonBPEditorBaselines::bFormatNodeAddedTransactionSurvives);

			// Remove the probe and refresh the shared fixture before settling again.
			FBlueprintEditorUtils::RemoveNode(Blueprint, Probe, /*bDontRecompile=*/true);
			Graph->NotifyGraphChanged();
			TXN_RefreshOpenEditor(Blueprint);
			TXN_SettleAgain(*Test, Graph, TEXT("cleanup"), /*bFormatAllEvents=*/false);
		}
	};
}

bool FClaireonBPEditorFormatNodeAddedRequiresTheSettle::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPEditorTxnInternal;

	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("FSlateApplication is not initialized. This suite is EditorContext-only so that "
			"cannot happen through the commandlet; reaching it means the suite was scheduled "
			"somewhere it cannot assert anything."));
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FTXN_FormatNodeAddedCommand(this, 10.0));
	return true;
}

// Ungrouped extraction baseline after settlement.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorUngroupedExtractMatchesBaseline,
	"Claireon.BPEditor.Transactions.UngroupedExtractMatchesBaseline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorTxnInternal
{
	class FTXN_UngroupedExtractCommand : public FTXN_PrimedGraphCommand
	{
	public:
		using FTXN_PrimedGraphCommand::FTXN_PrimedGraphCommand;

	protected:
		virtual void OnSettled(UBlueprint* Blueprint, UEdGraph* Graph) override
		{
			if (!TXN_CheckProvenance(*Test))
			{
				return;
			}

			FCanonicalNodes Canonical;
			FString Error;
			if (!TXN_ResetToCanonical(Blueprint, Graph, Canonical, Error))
			{
				Test->AddError(Error);
				return;
			}
			if (!TXN_SettleAgain(*Test, Graph, TEXT("post-reset"), /*bFormatAllEvents=*/false))
			{
				return;
			}

			const FGuid Target = Canonical.Call->NodeGuid;
			const int32 CompositesBeforeExtract = TXN_CountComposites(Graph);

			FTxnWindow Window;
			if (!TXN_OpenWindow(Window, Error))
			{
				Test->AddError(Error);
				return;
			}

			if (!TXN_ExtractComposite(*Test, TXN_FixturePath(), Graph->GetName(), Target,
					TEXT("Gate1b4aComposite")))
			{
				return;
			}

			// Refresh widgets after collapse reparents the selected node.
			TXN_RefreshOpenEditor(Blueprint);

			// Capture after settlement to include delayed BA work.
			if (!TXN_SettleAgain(*Test, Graph, TEXT("post-extract"), /*bFormatAllEvents=*/false))
			{
				return;
			}

			TArray<FString> Observed;
			if (!TXN_ReadWindow(Window, Observed, Error))
			{
				Test->AddError(Error);
				return;
			}

			TXN_CompareToBaseline(*Test, TEXT("case 4a (ungrouped extract)"),
				ClaireonBPEditorBaselines::UngroupedExtractTransactions,
				UE_ARRAY_COUNT(ClaireonBPEditorBaselines::UngroupedExtractTransactions),
				Observed);

			Test->TestEqual(TEXT("the extraction added exactly one composite gateway"),
				TXN_CountComposites(Graph), CompositesBeforeExtract + 1);

			// Refresh the shared fixture for the next test.
			FCanonicalNodes Restored;
			FString RestoreError;
			if (!TXN_ResetToCanonical(Blueprint, Graph, Restored, RestoreError))
			{
				Test->AddWarning(FString::Printf(
					TEXT("could not restore the fixture after case 4a: %s"), *RestoreError));
			}
		}
	};
}

bool FClaireonBPEditorUngroupedExtractMatchesBaseline::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPEditorTxnInternal;

	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("FSlateApplication is not initialized; extraction refuses without it and this "
			"suite is EditorContext-only so it should never have been scheduled here."));
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FTXN_UngroupedExtractCommand(this, 10.0));
	return true;
}

// Grouped extraction baseline after settlement.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorGroupedExtractMatchesBaseline,
	"Claireon.BPEditor.Transactions.GroupedExtractMatchesBaseline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorTxnInternal
{
	class FTXN_GroupedExtractCommand : public FTXN_PrimedGraphCommand
	{
	public:
		using FTXN_PrimedGraphCommand::FTXN_PrimedGraphCommand;

	protected:
		virtual void OnSettled(UBlueprint* Blueprint, UEdGraph* Graph) override
		{
			if (!TXN_CheckProvenance(*Test))
			{
				return;
			}

			FCanonicalNodes Canonical;
			FString Error;
			if (!TXN_ResetToCanonical(Blueprint, Graph, Canonical, Error))
			{
				Test->AddError(Error);
				return;
			}
			if (!TXN_SettleAgain(*Test, Graph, TEXT("post-reset"), /*bFormatAllEvents=*/false))
			{
				return;
			}

			const FGuid Target = Canonical.Call->NodeGuid;
			const int32 CompositesBeforeExtract = TXN_CountComposites(Graph);

			FTxnWindow Window;
			if (!TXN_OpenWindow(Window, Error))
			{
				Test->AddError(Error);
				return;
			}

			// ---- begin_group -> extract -> explicit settle -> end_group
			{
				ClaireonTool_TransactionBeginGroup BeginTool;
				TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
				Args->SetStringField(TEXT("label"), TEXT("gate1b-4b-grouped-extract"));
				const IClaireonTool::FToolResult Begun = BeginTool.Execute(Args);
				if (Begun.bIsError)
				{
					Test->AddError(FString::Printf(TEXT("transaction_begin_group failed: %s"),
						*Begun.ErrorMessage));
					return;
				}
			}

			bool bExtracted = TXN_ExtractComposite(*Test, TXN_FixturePath(), Graph->GetName(), Target,
				TEXT("Gate1b4bComposite"));
			if (bExtracted)
			{
				// Refresh inside the group before settlement paints the panel.
				TXN_RefreshOpenEditor(Blueprint);
				bExtracted = TXN_SettleAgain(*Test, Graph, TEXT("in-group post-extract"), /*bFormatAllEvents=*/false);
			}

			{
				// Close the group even on failure so it cannot absorb later work.
				ClaireonTool_TransactionEndGroup EndTool;
				TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
				const IClaireonTool::FToolResult Ended = EndTool.Execute(Args);
				if (Ended.bIsError)
				{
					Test->AddError(FString::Printf(TEXT("transaction_end_group failed: %s"),
						*Ended.ErrorMessage));
					return;
				}
			}
			if (!bExtracted)
			{
				return;
			}

			const int32 CompositesAfterExtract = TXN_CountComposites(Graph);
			Test->TestEqual(TEXT("the extraction added exactly one composite gateway"),
				CompositesAfterExtract, CompositesBeforeExtract + 1);

			TArray<FString> Observed;
			if (!TXN_ReadWindow(Window, Observed, Error))
			{
				Test->AddError(Error);
				return;
			}

			TXN_CompareToBaseline(*Test, TEXT("case 4b (grouped extract)"),
				ClaireonBPEditorBaselines::GroupedExtractTransactions,
				UE_ARRAY_COUNT(ClaireonBPEditorBaselines::GroupedExtractTransactions),
				Observed);

			int32 UndoneCount = -1;
			{
				ClaireonTool_TransactionUndo UndoTool;
				TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
				Args->SetNumberField(TEXT("count"), 1);
				const IClaireonTool::FToolResult Undone = UndoTool.Execute(Args);
				if (Undone.bIsError || !Undone.Data.IsValid())
				{
					Test->AddError(FString::Printf(TEXT("transaction_undo failed: %s"),
						*Undone.ErrorMessage));
					return;
				}
				double AsNumber = -1.0;
				Undone.Data->TryGetNumberField(TEXT("undone_count"), AsNumber);
				UndoneCount = static_cast<int32>(AsNumber);
			}

			Test->TestEqual(TEXT("one undo reversed the baseline number of transactions"),
				UndoneCount, ClaireonBPEditorBaselines::GroupedUndoExpectedUndoneCount);

			// Refresh widgets after undo before a later test pumps Slate.
			TXN_RefreshOpenEditor(Blueprint);

			const int32 CompositesAfterUndo = TXN_CountComposites(Graph);
			const bool bGatewayGone = (CompositesAfterUndo == CompositesBeforeExtract);
			Test->AddInfo(FString::Printf(
				TEXT("case 4b undo: composite gateways %d before extract, %d after one undo (ubergraph ")
				TEXT("now holds %d node(s))"),
				CompositesBeforeExtract, CompositesAfterUndo, Graph->Nodes.Num()));
			Test->TestTrue(
				FString::Printf(
					TEXT("whether one undo removes the composite gateway matches the committed baseline ")
					TEXT("(baseline says %s, observed %s)"),
					ClaireonBPEditorBaselines::bGroupedUndoRemovesTheCompositeGateway
						? TEXT("removed") : TEXT("NOT removed"),
					bGatewayGone ? TEXT("removed") : TEXT("NOT removed")),
				bGatewayGone == ClaireonBPEditorBaselines::bGroupedUndoRemovesTheCompositeGateway);

			const int32 ForeignOuter = TXN_CountForeignOuterNodes(Graph);
			Test->AddInfo(FString::Printf(
				TEXT("case 4b undo: %d node(s) listed by the ubergraph whose Outer is not the ubergraph"),
				ForeignOuter));
			Test->TestEqual(
				TEXT("the number of nodes the undo leaves with a foreign Outer matches the committed baseline"),
				ForeignOuter, ClaireonBPEditorBaselines::GroupedUndoForeignOuterNodeCount);

			// Reset inconsistent membership/Outer state left by undo before the next Slate pump.
			FCanonicalNodes Restored;
			FString RestoreError;
			if (!TXN_ResetToCanonical(Blueprint, Graph, Restored, RestoreError))
			{
				Test->AddWarning(FString::Printf(
					TEXT("could not restore the fixture after case 4b: %s"), *RestoreError));
			}
		}
	};
}

bool FClaireonBPEditorGroupedExtractMatchesBaseline::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPEditorTxnInternal;

	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("FSlateApplication is not initialized; extraction refuses without it and this "
			"suite is EditorContext-only so it should never have been scheduled here."));
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FTXN_GroupedExtractCommand(this, 10.0));
	return true;
}

#endif // WITH_CLAIREON_TESTS && WITH_EDITOR
