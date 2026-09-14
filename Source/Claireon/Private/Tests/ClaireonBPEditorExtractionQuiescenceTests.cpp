// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Cross-check extraction recovery claims against the transaction buffer and graph state.
// Focus composite graphs explicitly; opening a tab without focus does not set GetFocusedGraph.
// Create and open fixtures in separate updates to avoid reentrant loading. Rebuild open
// editors after mutation before later Slate ticks can paint stale graph panels.

#include "Misc/AutomationTest.h"

#if WITH_CLAIREON_TESTS && WITH_EDITOR

#include "Tests/ClaireonBPEditorFixtures.h"
#include "Tools/ClaireonBPMutationResult.h"
#include "Tools/ClaireonBPSnapshot.h"
#include "Tools/ClaireonBlueprintGraphTool_Extract.h"

#include "BlueprintEditor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_CustomEvent.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Toolkits/IToolkit.h"
#include "Toolkits/IToolkitHost.h"

namespace ClaireonBPExtractQuiescenceInternal
{
	// Prefix helpers to avoid unity-build collisions.


	static const TCHAR* EXQ_StateApplied()          { return TEXT("applied_clean"); }
	static const TCHAR* EXQ_StateOperationFailed()  { return TEXT("applied_operation_failed"); }
	static const TCHAR* EXQ_StateValidationFailed() { return TEXT("applied_validation_failed"); }
	static const TCHAR* EXQ_StateRefused()          { return TEXT("refused"); }

	static FString EXQ_PhaseCollapse()
	{
		return ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::Collapse);
	}
	static FString EXQ_PhaseCompileValidate()
	{
		return ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::CompileValidate);
	}

	enum class EEXQKind : uint8
	{
		Function,
		Macro,
		Composite,
		Event,
	};

	static const TCHAR* EXQ_KindName(EEXQKind Kind)
	{
		switch (Kind)
		{
		case EEXQKind::Function:  return TEXT("function");
		case EEXQKind::Macro:     return TEXT("macro");
		case EEXQKind::Composite: return TEXT("composite");
		case EEXQKind::Event:     return TEXT("event");
		}
		return TEXT("unknown");
	}

	/** The FScopedTransaction title ApplyExtract opens, per kind. The cross-check reads this. */
	static FString EXQ_TransactionTitle(EEXQKind Kind)
	{
		return FString::Printf(TEXT("Extract %s"), EXQ_KindName(Kind));
	}


	static IClaireonTool::FToolResult EXQ_Extract(EEXQKind Kind, const FString& AssetPath,
		const FString& GraphName, const TArray<FGuid>& NodeGuids, const TCHAR* ResponseMode)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("graph_name"), GraphName);
		Args->SetStringField(TEXT("response_mode"), ResponseMode);

		TArray<TSharedPtr<FJsonValue>> Guids;
		for (const FGuid& Guid : NodeGuids)
		{
			Guids.Add(MakeShared<FJsonValueString>(Guid.ToString(EGuidFormats::DigitsWithHyphens)));
		}
		Args->SetArrayField(TEXT("node_guids"), Guids);

		switch (Kind)
		{
		case EEXQKind::Function:
		{
			ClaireonBlueprintGraphTool_ExtractFunction Tool;
			return Tool.Execute(Args);
		}
		case EEXQKind::Macro:
		{
			ClaireonBlueprintGraphTool_ExtractMacro Tool;
			return Tool.Execute(Args);
		}
		case EEXQKind::Composite:
		{
			ClaireonBlueprintGraphTool_ExtractComposite Tool;
			return Tool.Execute(Args);
		}
		default:
		{
			ClaireonBlueprintGraphTool_ExtractEvent Tool;
			return Tool.Execute(Args);
		}
		}
	}

	// Envelope readers reject absent fields.

	static bool EXQ_ReadString(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field, FString& Out)
	{
		if (!Data.IsValid() || !Data->TryGetStringField(Field, Out))
		{
			Test.AddError(FString::Printf(TEXT("the envelope omitted the string field '%s'."), Field));
			return false;
		}
		return true;
	}

	static bool EXQ_ReadBool(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field, bool& Out)
	{
		if (!Data.IsValid() || !Data->TryGetBoolField(Field, Out))
		{
			Test.AddError(FString::Printf(TEXT("the envelope omitted the bool field '%s'."), Field));
			return false;
		}
		return true;
	}

	static void EXQ_ExpectString(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field, const TCHAR* Expected, const TCHAR* What)
	{
		FString Observed;
		if (EXQ_ReadString(Test, Data, Field, Observed))
		{
			Test.TestEqual(FString::Printf(TEXT("%s: %s"), What, Field), Observed, FString(Expected));
		}
	}

	static void EXQ_ExpectBool(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field, bool bExpected, const TCHAR* What)
	{
		bool bObserved = !bExpected;
		if (EXQ_ReadBool(Test, Data, Field, bObserved))
		{
			Test.TestTrue(FString::Printf(TEXT("%s: %s is %s"), What, Field,
				bExpected ? TEXT("true") : TEXT("false")), bObserved == bExpected);
		}
	}

	static void EXQ_ExpectAbsent(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field, const TCHAR* Why)
	{
		if (Data.IsValid() && Data->HasField(Field))
		{
			Test.AddError(FString::Printf(TEXT("'%s' is present, and it must not be: %s"), Field, Why));
		}
	}

	/** Read undo_record_available, reporting absence rather than substituting a default. */
	static bool EXQ_UndoAvailable(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		bool& bOut)
	{
		return EXQ_ReadBool(Test, Data, TEXT("undo_record_available"), bOut);
	}


	static const TSharedPtr<FJsonObject>* EXQ_Delta(const TSharedPtr<FJsonObject>& Data)
	{
		const TSharedPtr<FJsonObject>* Delta = nullptr;
		if (Data.IsValid() && Data->TryGetObjectField(TEXT("operation_delta"), Delta)
			&& Delta != nullptr && Delta->IsValid())
		{
			return Delta;
		}
		return nullptr;
	}

	/** Match the effect class, kind, and exact target in the operation delta. */
	static bool EXQ_DeltaHasEntry(const TSharedPtr<FJsonObject>& Data,
		const TCHAR* EffectClass, const TCHAR* Kind, const FString& Target)
	{
		const TSharedPtr<FJsonObject>* Delta = EXQ_Delta(Data);
		if (Delta == nullptr)
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!(*Delta)->TryGetArrayField(TEXT("entries"), Entries) || Entries == nullptr)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Entries)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Entry) || Entry == nullptr)
			{
				continue;
			}
			FString ObservedClass;
			FString ObservedKind;
			FString ObservedTarget;
			(*Entry)->TryGetStringField(TEXT("effect_class"), ObservedClass);
			(*Entry)->TryGetStringField(TEXT("kind"), ObservedKind);
			(*Entry)->TryGetStringField(TEXT("target"), ObservedTarget);
			if (ObservedClass.Equals(EffectClass) && ObservedKind.Equals(Kind)
				&& ObservedTarget.Equals(Target, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static FString EXQ_NodeTarget(const UEdGraphNode& Node)
	{
		return FString::Printf(TEXT("node:%s"),
			*Node.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	}

	/** Require a comparable delta with a durable effect. */
	static void EXQ_ExpectDeltaProvesDurableEffect(FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Data, const TCHAR* What)
	{
		const TSharedPtr<FJsonObject>* Delta = EXQ_Delta(Data);
		if (Delta == nullptr)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: operation_delta is absent. The state on a retained-failure return is "
				     "selected FROM the observed diff, so a return without one reported a state "
				     "it did not measure."), What));
			return;
		}
		bool bComparable = false;
		bool bHasDurableEffect = false;
		(*Delta)->TryGetBoolField(TEXT("comparable"), bComparable);
		(*Delta)->TryGetBoolField(TEXT("has_durable_effect"), bHasDurableEffect);
		Test.TestTrue(FString::Printf(TEXT("%s: the delta is comparable"), What), bComparable);
		Test.TestTrue(FString::Printf(TEXT("%s: the delta proves a durable effect"), What),
			bHasDurableEffect);
	}

	// Observe transaction titles: queue length alone cannot attribute records after undo purging or transient removal.

	struct FEXQTxnWindow
	{
		int32 StartLength = INDEX_NONE;
		int32 StartUndoCount = INDEX_NONE;
		bool bValid = false;
	};

	static UTransBuffer* EXQ_Buffer()
	{
		return IsValid(GEditor) ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
	}

	static FEXQTxnWindow EXQ_OpenWindow(FAutomationTestBase& Test)
	{
		FEXQTxnWindow Window;
		UTransBuffer* Buffer = EXQ_Buffer();
		if (!IsValid(Buffer))
		{
			Test.AddError(TEXT("GEditor->Trans is not a UTransBuffer, so undo availability cannot be "
				"cross-checked against the editor. This suite is EditorContext-only precisely so "
				"that cannot happen."));
			return Window;
		}
		Window.StartLength = Buffer->GetQueueLength();
		Window.StartUndoCount = Buffer->GetUndoCount();
		Window.bValid = true;
		return Window;
	}

	/** Report unreadable windows through OutMeasured; do not treat them as absent transactions. */
	static bool EXQ_WindowHasTitle(const FEXQTxnWindow& Window, const FString& Title,
		bool& bOutMeasured)
	{
		bOutMeasured = false;
		UTransBuffer* Buffer = EXQ_Buffer();
		if (!Window.bValid || !IsValid(Buffer))
		{
			return false;
		}
		const int32 Start = FMath::Max(0, Window.StartLength - FMath::Max(0, Window.StartUndoCount));
		const int32 Length = Buffer->GetQueueLength();
		if (Length < Start)
		{
			return false;
		}
		bOutMeasured = true;
		for (int32 Index = Start; Index < Length; ++Index)
		{
			const FTransaction* Transaction = Buffer->GetTransaction(Index);
			if (Transaction != nullptr && Transaction->GetTitle().ToString().Equals(Title))
			{
				return true;
			}
		}
		return false;
	}

	/** Compare the envelope's undo claim with the operation's transaction title in the buffer. */
	static void EXQ_CrossCheckUndo(FAutomationTestBase& Test, const FEXQTxnWindow& Window,
		EEXQKind Kind, bool bEnvelopeSaysUndoAvailable, const TCHAR* What)
	{
		bool bMeasured = false;
		const bool bPresent = EXQ_WindowHasTitle(Window, EXQ_TransactionTitle(Kind), bMeasured);
		if (!bMeasured)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: the transaction window could not be read, so the envelope's "
				     "undo_record_available could not be corroborated. Reported as a measurement "
				     "failure rather than as agreement."), What));
			return;
		}
		Test.TestTrue(FString::Printf(
			TEXT("%s: the editor's buffer agrees with the envelope about the '%s' record "
			     "(envelope says %s, buffer says %s)"),
			What, *EXQ_TransactionTitle(Kind),
			bEnvelopeSaysUndoAvailable ? TEXT("available") : TEXT("gone"),
			bPresent ? TEXT("present") : TEXT("absent")),
			bPresent == bEnvelopeSaysUndoAvailable);
	}


	struct FEXQIsland
	{
		UK2Node_CustomEvent* Event = nullptr;
		UK2Node_CallFunction* Call = nullptr;
	};

	static UFunction* EXQ_PrintString()
	{
		return UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString"));
	}

	static UK2Node_CallFunction* EXQ_AddCall(UEdGraph& Graph, UFunction& Function, int32 X, int32 Y)
	{
		UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(&Graph);
		Graph.AddNode(Call, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Call->CreateNewGuid();
		Call->SetFromFunction(&Function);
		Call->NodePosX = X;
		Call->NodePosY = Y;
		Call->AllocateDefaultPins();
		return Call;
	}

	/** An event calling PrintString has one external exec entry and no outgoing exec or data boundary. */
	static bool EXQ_BuildIsland(UBlueprint& Blueprint, UEdGraph& Graph, const TCHAR* Suffix,
		int32 Row, FEXQIsland& Out, FString& OutError)
	{
		UFunction* PrintString = EXQ_PrintString();
		if (PrintString == nullptr)
		{
			OutError = TEXT("UKismetSystemLibrary::PrintString could not be resolved, so no fixture "
				"island can be built. That is an engine-side change, not a fixture problem.");
			return false;
		}

		UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(&Graph);
		Graph.AddNode(Event, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Event->CreateNewGuid();
		Event->CustomFunctionName = FName(*FString::Printf(TEXT("EXQ_Entry_%s"), Suffix));
		Event->NodePosX = 0;
		Event->NodePosY = Row * 320;
		Event->AllocateDefaultPins();

		UK2Node_CallFunction* Call = EXQ_AddCall(Graph, *PrintString, 400, Row * 320);

		UEdGraphPin* Then = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		UEdGraphPin* Exec = Call->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		if (Then == nullptr || Exec == nullptr)
		{
			OutError = FString::Printf(TEXT("island '%s' could not be wired: the %s pin is missing."),
				Suffix, Then == nullptr ? TEXT("event Then") : TEXT("call Execute"));
			return false;
		}
		Then->MakeLinkTo(Exec);

		Out.Event = Event;
		Out.Call = Call;
		return true;
	}

	/**
	 * Create two links from one exec output to cause link validation failure without disrupting skeleton generation.
	 * Use MakeLinkTo because the schema enforces a single link.
	 */
	static bool EXQ_BuildUncompilableIsland(UEdGraph& Graph, int32 Row, FString& OutError)
	{
		UFunction* PrintString = EXQ_PrintString();
		if (PrintString == nullptr)
		{
			OutError = TEXT("UKismetSystemLibrary::PrintString could not be resolved.");
			return false;
		}

		UK2Node_CustomEvent* Event = NewObject<UK2Node_CustomEvent>(&Graph);
		Graph.AddNode(Event, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Event->CreateNewGuid();
		Event->CustomFunctionName = TEXT("EXQ_Entry_Broken");
		Event->NodePosX = 0;
		Event->NodePosY = Row * 320;
		Event->AllocateDefaultPins();

		UK2Node_CallFunction* First = EXQ_AddCall(Graph, *PrintString, 400, Row * 320);
		UK2Node_CallFunction* Second = EXQ_AddCall(Graph, *PrintString, 400, Row * 320 + 160);

		UEdGraphPin* Then = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		UEdGraphPin* FirstExec = First->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		UEdGraphPin* SecondExec = Second->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		if (Then == nullptr || FirstExec == nullptr || SecondExec == nullptr)
		{
			OutError = TEXT("the uncompilable island could not be wired: an exec pin is missing.");
			return false;
		}
		Then->MakeLinkTo(FirstExec);
		Then->MakeLinkTo(SecondExec);
		if (Then->LinkedTo.Num() != 2)
		{
			OutError = FString::Printf(
				TEXT("the uncompilable island's exec output holds %d link(s), not 2, so the compile "
				     "would SUCCEED and the test would pass vacuously."), Then->LinkedTo.Num());
			return false;
		}
		return true;
	}

	static TSharedPtr<FBlueprintEditor> EXQ_OpenEditor(UBlueprint& Blueprint)
	{
		UAssetEditorSubsystem* Subsystem = IsValid(GEditor)
			? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
			: nullptr;
		if (!IsValid(Subsystem))
		{
			return nullptr;
		}
		IAssetEditorInstance* Instance = Subsystem->FindEditorForAsset(&Blueprint, /*bFocusIfOpen=*/false);
		if (Instance == nullptr)
		{
			// Open synchronously without a progress dialog.
			Subsystem->OpenEditorForAsset(&Blueprint, EToolkitMode::Standalone,
				TSharedPtr<IToolkitHost>(), /*bShowProgressWindow=*/false);
			Instance = Subsystem->FindEditorForAsset(&Blueprint, /*bFocusIfOpen=*/false);
		}
		if (Instance == nullptr)
		{
			return nullptr;
		}
		FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(Instance);
		return StaticCastSharedRef<FBlueprintEditor>(Toolkit->AsShared());
	}

	/** Set focus to update FocusedGraphEdPtr, which CollapseNodes reads. Return null if no editor is available. */
	static UEdGraph* EXQ_FocusAndReport(UBlueprint& Blueprint, UEdGraph* Graph)
	{
		TSharedPtr<FBlueprintEditor> Editor = EXQ_OpenEditor(Blueprint);
		if (!Editor.IsValid())
		{
			return nullptr;
		}
		Editor->OpenGraphAndBringToFront(Graph, /*bSetFocus=*/true);
		return Editor->GetFocusedGraph();
	}

	/** A second graph on the Blueprint, so the composite path can be pointed away from focus. */
	static UEdGraph* EXQ_AddFunctionGraph(UBlueprint& Blueprint, const TCHAR* Name)
	{
		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			&Blueprint, FName(Name), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!IsValid(NewGraph))
		{
			return nullptr;
		}
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(
			&Blueprint, NewGraph, /*bIsUserCreated=*/true, static_cast<UClass*>(nullptr));
		return NewGraph;
	}

	// Independent snapshot observation.

	static FClaireonBPSnapshot EXQ_Capture(UBlueprint& Blueprint)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint.GetAllGraphs(Graphs);
		FClaireonBPSnapshot Snapshot;
		ClaireonBPSnapshot::Capture(&Blueprint, Graphs,
			EClaireonBPSnapshotFamily::Extraction, Snapshot);
		return Snapshot;
	}

	static TSet<FGuid> EXQ_NodeGuids(const UEdGraph& Graph)
	{
		TSet<FGuid> Guids;
		for (const UEdGraphNode* Node : Graph.Nodes)
		{
			if (IsValid(Node))
			{
				Guids.Add(Node->NodeGuid);
			}
		}
		return Guids;
	}

	static int32 EXQ_CollectNewNodesOfClass(const UEdGraph& Graph, const TSet<FGuid>& Before,
		const UClass* Class, TArray<UEdGraphNode*>& OutNodes)
	{
		int32 Count = 0;
		for (UEdGraphNode* Node : Graph.Nodes)
		{
			if (IsValid(Node) && !Before.Contains(Node->NodeGuid) && Node->IsA(Class))
			{
				OutNodes.Add(Node);
				++Count;
			}
		}
		return Count;
	}


	/** Separate creation, body, and teardown with stack unwinds to avoid reentrant loading and closing during compile. */
	class FEXQ_FixtureCommand : public IAutomationLatentCommand
	{
	public:
		FEXQ_FixtureCommand(FAutomationTestBase* InTest, const FString& InAssetPath)
			: Test(InTest)
			, AssetPath(InAssetPath)
		{
		}

		virtual bool Update() override
		{
			switch (Phase)
			{
			case EPhase::Create:
			{
				FString CreateError;
				UBlueprint* Blueprint = ClaireonBPEditorFixtures::Create(AssetPath, CreateError);
				if (!IsValid(Blueprint))
				{
					Test->AddError(FString::Printf(TEXT("fixture creation failed at %s: %s"),
						*AssetPath, *CreateError));
					return true;
				}
				UEdGraph* Graph = ClaireonBPEditorFixtures::FirstUbergraph(Blueprint);
				if (!IsValid(Graph))
				{
					Test->AddError(TEXT("the fixture has no ubergraph to build islands in."));
					Phase = EPhase::Teardown;
					return false;
				}
				FString BuildError;
				if (!BuildFixture(*Blueprint, *Graph, BuildError))
				{
					Test->AddError(FString::Printf(TEXT("fixture setup failed: %s"), *BuildError));
					Phase = EPhase::Teardown;
					return false;
				}
				Graph->NotifyGraphChanged();
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				Phase = EPhase::Settle;
				return false;
			}

			case EPhase::Settle:
				if (++SettleUpdates < 3)
				{
					return false;
				}
				Phase = EPhase::Body;
				return false;

			case EPhase::Body:
			{
				UBlueprint* Blueprint = Cast<UBlueprint>(ClaireonBPEditorFixtures::Resolve(AssetPath));
				UEdGraph* Graph = ClaireonBPEditorFixtures::FirstUbergraph(Blueprint);
				if (!IsValid(Blueprint) || !IsValid(Graph))
				{
					Test->AddError(TEXT("the fixture went away before the body could run."));
					return true;
				}
				RunBody(*Blueprint, *Graph);

				// Rebuild widgets so later Slate pumps cannot paint a stale graph panel.
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				Phase = EPhase::Teardown;
				return false;
			}

			case EPhase::Teardown:
			default:
				ClaireonBPEditorFixtures::Teardown(AssetPath);
				return true;
			}
		}

	protected:
		/** Build the graph the body will operate on. Runs before any editor is opened. */
		virtual bool BuildFixture(UBlueprint& Blueprint, UEdGraph& Graph, FString& OutError) = 0;

		/** The measurement. Runs with the fixture resident and no create/save on the stack. */
		virtual void RunBody(UBlueprint& Blueprint, UEdGraph& Graph) = 0;

		FAutomationTestBase* Test = nullptr;
		FString AssetPath;

	private:
		enum class EPhase : uint8
		{
			Create,
			Settle,
			Body,
			Teardown,
		};

		EPhase Phase = EPhase::Create;
		int32 SettleUpdates = 0;
	};
}

// Canceled extraction: retained nodes and skeleton effects, but no undo record.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorCanceledShapeRetainsWithoutUndo,
	"Claireon.BPEditor.Extraction.CanceledShapeRetainsWithoutUndoRecord",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPExtractQuiescenceInternal
{
	class FEXQ_CanceledShapeCommand : public FEXQ_FixtureCommand
	{
	public:
		using FEXQ_FixtureCommand::FEXQ_FixtureCommand;

	protected:
		virtual bool BuildFixture(UBlueprint& Blueprint, UEdGraph& Graph, FString& OutError) override
		{
			return EXQ_BuildIsland(Blueprint, Graph, TEXT("Cancel"), 0, Island, OutError);
		}

		virtual void RunBody(UBlueprint& Blueprint, UEdGraph& Graph) override
		{
			if (!IsValid(Island.Call))
			{
				Test->AddError(TEXT("the island's call node did not survive fixture setup."));
				return;
			}
			const FGuid SelectedGuid = Island.Call->NodeGuid;
			const TSet<FGuid> Before = EXQ_NodeGuids(Graph);
			const FClaireonBPSnapshot BeforeSnapshot = EXQ_Capture(Blueprint);
			const FEXQTxnWindow Window = EXQ_OpenWindow(*Test);

			// The seam removes the event Then pin; subsequent mutation and reporting use the real path.
			IClaireonTool::FToolResult Result;
			{
				const ClaireonBPFaultInjection::FScopedFault Fault(EXQ_PhaseCollapse());
				Result = EXQ_Extract(EEXQKind::Event, AssetPath, Graph.GetName(),
					{ SelectedGuid }, TEXT("status"));
			}

			Test->TestTrue(TEXT("the canceled shape is reported as an error result"), Result.bIsError);
			if (!Result.Data.IsValid())
			{
				Test->AddError(TEXT("the canceled shape returned no structured data at all, so it "
					"reported no mutation state -- which is the defect gate 3c closed."));
				return;
			}

			EXQ_ExpectString(*Test, Result.Data, TEXT("mutation_state"),
				EXQ_StateOperationFailed(), TEXT("EXT-R1"));
			EXQ_ExpectBool(*Test, Result.Data, TEXT("mutation_retained"), true, TEXT("EXT-R1"));
			EXQ_ExpectString(*Test, Result.Data, TEXT("failed_phase"),
				*EXQ_PhaseCollapse(), TEXT("EXT-R1"));
			EXQ_ExpectString(*Test, Result.Data, TEXT("last_completed_phase"),
				kClaireonBPPhaseNone, TEXT("EXT-R1"));
			EXQ_ExpectBool(*Test, Result.Data, TEXT("asset_dirty"), true, TEXT("EXT-R1"));
			EXQ_ExpectBool(*Test, Result.Data, TEXT("asset_compiled"), false, TEXT("EXT-R1"));
			EXQ_ExpectString(*Test, Result.Data, TEXT("target_graph"),
				*Graph.GetName(), TEXT("EXT-R1"));
			EXQ_ExpectBool(*Test, Result.Data, TEXT("session_graph_stale"), true, TEXT("EXT-R1"));
			EXQ_ExpectString(*Test, Result.Data, TEXT("extract_kind"), TEXT("event"), TEXT("EXT-R1"));

			bool bUndoAvailable = true;
			if (EXQ_UndoAvailable(*Test, Result.Data, bUndoAvailable))
			{
				Test->TestFalse(TEXT("EXT-R1: undo_record_available is FALSE -- Cancel() popped the "
					"record without reverting anything"), bUndoAvailable);
			}

			if (Result.Hints.Num() > 0)
			{
				FString HintTool;
				Result.Hints[0]->TryGetStringField(TEXT("tool"), HintTool);
				Test->TestEqual(TEXT("EXT-R1: the hint points at inspection, not at undo"),
					HintTool, FString(TEXT("bp_get_graph")));
			}
			else
			{
				Test->AddError(TEXT("EXT-R1 returned no recovery hint."));
			}

			// Require the delta to name both added nodes and the skeleton effect.
			EXQ_ExpectDeltaProvesDurableEffect(*Test, Result.Data, TEXT("EXT-R1"));

			TArray<UEdGraphNode*> NewEvents;
			TArray<UEdGraphNode*> NewCalls;
			const int32 EventCount = EXQ_CollectNewNodesOfClass(
				Graph, Before, UK2Node_CustomEvent::StaticClass(), NewEvents);
			const int32 CallCount = EXQ_CollectNewNodesOfClass(
				Graph, Before, UK2Node_CallFunction::StaticClass(), NewCalls);
			Test->TestEqual(TEXT("EXT-R1: the custom event node is still in the graph"), EventCount, 1);
			Test->TestEqual(TEXT("EXT-R1: the call node is still in the graph"), CallCount, 1);

			for (UEdGraphNode* Node : NewEvents)
			{
				Test->TestTrue(FString::Printf(
					TEXT("EXT-R1: operation_delta names the retained custom event node %s"),
					*EXQ_NodeTarget(*Node)),
					EXQ_DeltaHasEntry(Result.Data, TEXT("graph_topology"), TEXT("added"),
						EXQ_NodeTarget(*Node)));
			}
			for (UEdGraphNode* Node : NewCalls)
			{
				Test->TestTrue(FString::Printf(
					TEXT("EXT-R1: operation_delta names the retained call node %s"),
					*EXQ_NodeTarget(*Node)),
					EXQ_DeltaHasEntry(Result.Data, TEXT("graph_topology"), TEXT("added"),
						EXQ_NodeTarget(*Node)));
			}

			// Skeleton regeneration can reuse the class pointer; observe the added UFunction.
			for (UEdGraphNode* Node : NewEvents)
			{
				const UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node);
				if (Event == nullptr)
				{
					continue;
				}
				const FString FunctionTarget = FString::Printf(TEXT("skeleton_function:%s"),
					*Event->CustomFunctionName.ToString());
				Test->TestTrue(FString::Printf(
					TEXT("EXT-R1: operation_delta names the force-regenerated skeleton through the "
					     "function it gained (%s)"), *FunctionTarget),
					EXQ_DeltaHasEntry(Result.Data, TEXT("generated_class"), TEXT("added"),
						FunctionTarget));
			}

			// Missing pins prevent relocation, so the original chain must remain intact.
			Test->TestTrue(TEXT("EXT-R1: the originally selected node is still in the source graph"),
				EXQ_NodeGuids(Graph).Contains(SelectedGuid));

			EXQ_CrossCheckUndo(*Test, Window, EEXQKind::Event, /*bEnvelopeSaysUndoAvailable=*/false,
				TEXT("EXT-R1"));

			const FClaireonBPSnapshot AfterSnapshot = EXQ_Capture(Blueprint);
			const FClaireonBPSnapshotDelta Observed =
				ClaireonBPSnapshot::Diff(BeforeSnapshot, AfterSnapshot);
			Test->TestTrue(TEXT("EXT-R1: the test's own snapshot diff also sees a durable effect"),
				Observed.HasDurableEffect());
		}

	private:
		FEXQIsland Island;
	};
}

bool FClaireonBPEditorCanceledShapeRetainsWithoutUndo::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPExtractQuiescenceInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FEXQ_CanceledShapeCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("ExqCancel"))));
	return true;
}

// Committed extraction failure: relocated selection and an available undo record.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorCommittedShapeRetainsWithUndo,
	"Claireon.BPEditor.Extraction.CommittedShapeRetainsWithUndoRecord",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPExtractQuiescenceInternal
{
	/** Shared committed-failure measurement. */
	static void EXQ_MeasureCommittedShape(FAutomationTestBase& Test, EEXQKind Kind,
		const FString& AssetPath, UBlueprint& Blueprint, UEdGraph& Graph,
		const FGuid& SelectedGuid)
	{
		const FString What = FString::Printf(TEXT("EXT-R2/%s"), EXQ_KindName(Kind));

		if (Kind == EEXQKind::Composite)
		{
			// Establish focus before testing the committed composite path.
			UEdGraph* Focused = EXQ_FocusAndReport(Blueprint, &Graph);
			if (Focused != &Graph)
			{
				Test.AddError(FString::Printf(
					TEXT("%s: the intended graph '%s' could not be focused (focused: '%s'), so the "
					     "composite path would refuse before collapsing and this measurement would "
					     "be of the refusal instead."),
					*What, *Graph.GetName(),
					IsValid(Focused) ? *Focused->GetName() : TEXT("<none>")));
				return;
			}
		}

		const TSet<FGuid> Before = EXQ_NodeGuids(Graph);
		const FEXQTxnWindow Window = EXQ_OpenWindow(Test);

		IClaireonTool::FToolResult Result;
		{
			const ClaireonBPFaultInjection::FScopedFault Fault(EXQ_PhaseCollapse());
			Result = EXQ_Extract(Kind, AssetPath, Graph.GetName(), { SelectedGuid }, TEXT("status"));
		}

		Test.TestTrue(FString::Printf(TEXT("%s: reported as an error result"), *What), Result.bIsError);
		if (!Result.Data.IsValid())
		{
			Test.AddError(FString::Printf(
				TEXT("%s returned no structured data, so it reported no mutation state on an asset "
				     "it had already mutated."), *What));
			return;
		}

		EXQ_ExpectString(Test, Result.Data, TEXT("mutation_state"),
			EXQ_StateOperationFailed(), *What);
		EXQ_ExpectBool(Test, Result.Data, TEXT("mutation_retained"), true, *What);
		EXQ_ExpectString(Test, Result.Data, TEXT("failed_phase"), *EXQ_PhaseCollapse(), *What);
		EXQ_ExpectString(Test, Result.Data, TEXT("last_completed_phase"),
			kClaireonBPPhaseNone, *What);
		EXQ_ExpectBool(Test, Result.Data, TEXT("asset_dirty"), true, *What);
		EXQ_ExpectBool(Test, Result.Data, TEXT("asset_compiled"), false, *What);
		EXQ_ExpectString(Test, Result.Data, TEXT("target_graph"), *Graph.GetName(), *What);
		EXQ_ExpectBool(Test, Result.Data, TEXT("session_graph_stale"), true, *What);
		EXQ_ExpectString(Test, Result.Data, TEXT("extract_kind"), EXQ_KindName(Kind), *What);

		bool bUndoAvailable = false;
		if (EXQ_UndoAvailable(Test, Result.Data, bUndoAvailable))
		{
			Test.TestTrue(FString::Printf(
				TEXT("%s: undo_record_available is TRUE -- the scoped transaction committed two "
				     "statements above the check"), *What), bUndoAvailable);
		}

		if (Result.Hints.Num() > 0)
		{
			FString HintTool;
			Result.Hints[0]->TryGetStringField(TEXT("tool"), HintTool);
			Test.TestEqual(FString::Printf(TEXT("%s: the hint offers the undo that exists"), *What),
				HintTool, FString(TEXT("transaction_undo")));
		}
		else
		{
			Test.AddError(FString::Printf(TEXT("%s returned no recovery hint."), *What));
		}

		EXQ_ExpectDeltaProvesDurableEffect(Test, Result.Data, *What);

		// Require the delta to name the selected node removed from the source graph.
		const FString SelectedTarget = FString::Printf(TEXT("node:%s"),
			*SelectedGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Test.TestTrue(FString::Printf(
			TEXT("%s: operation_delta records the selection leaving the source graph (%s)"),
			*What, *SelectedTarget),
			EXQ_DeltaHasEntry(Result.Data, TEXT("graph_topology"), TEXT("removed"), SelectedTarget));
		Test.TestFalse(FString::Printf(
			TEXT("%s: the selected node is no longer listed by the source graph"), *What),
			EXQ_NodeGuids(Graph).Contains(SelectedGuid));

		TArray<UEdGraphNode*> NewNodes;
		EXQ_CollectNewNodesOfClass(Graph, Before, UEdGraphNode::StaticClass(), NewNodes);
		Test.TestTrue(FString::Printf(TEXT("%s: the collapse left a gateway node behind"), *What),
			NewNodes.Num() >= 1);

		EXQ_CrossCheckUndo(Test, Window, Kind, /*bEnvelopeSaysUndoAvailable=*/true, *What);
	}

	class FEXQ_CommittedShapeCommand : public FEXQ_FixtureCommand
	{
	public:
		using FEXQ_FixtureCommand::FEXQ_FixtureCommand;

	protected:
		virtual bool BuildFixture(UBlueprint& Blueprint, UEdGraph& Graph, FString& OutError) override
		{
			return EXQ_BuildIsland(Blueprint, Graph, TEXT("Fn"), 0, Islands[0], OutError)
				&& EXQ_BuildIsland(Blueprint, Graph, TEXT("Mac"), 1, Islands[1], OutError)
				&& EXQ_BuildIsland(Blueprint, Graph, TEXT("Comp"), 2, Islands[2], OutError);
		}

		virtual void RunBody(UBlueprint& Blueprint, UEdGraph& Graph) override
		{
			static const EEXQKind Kinds[3] =
				{ EEXQKind::Function, EEXQKind::Macro, EEXQKind::Composite };
			for (int32 Index = 0; Index < 3; ++Index)
			{
				if (!IsValid(Islands[Index].Call))
				{
					Test->AddError(FString::Printf(
						TEXT("island %d's call node did not survive fixture setup."), Index));
					continue;
				}
				EXQ_MeasureCommittedShape(*Test, Kinds[Index], AssetPath, Blueprint, Graph,
					Islands[Index].Call->NodeGuid);
			}
		}

	private:
		FEXQIsland Islands[3];
	};
}

bool FClaireonBPEditorCommittedShapeRetainsWithUndo::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPExtractQuiescenceInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FEXQ_CommittedShapeCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("ExqCommit"))));
	return true;
}

// Compare canceled and committed failures through the same result builder.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorShapesDisagreeOnUndoAvailability,
	"Claireon.BPEditor.Extraction.TheTwoShapesDisagreeOnUndoAvailability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPExtractQuiescenceInternal
{
	class FEXQ_OppositionCommand : public FEXQ_FixtureCommand
	{
	public:
		using FEXQ_FixtureCommand::FEXQ_FixtureCommand;

	protected:
		virtual bool BuildFixture(UBlueprint& Blueprint, UEdGraph& Graph, FString& OutError) override
		{
			return EXQ_BuildIsland(Blueprint, Graph, TEXT("OppEvent"), 0, Islands[0], OutError)
				&& EXQ_BuildIsland(Blueprint, Graph, TEXT("OppFn"), 1, Islands[1], OutError);
		}

		virtual void RunBody(UBlueprint& Blueprint, UEdGraph& Graph) override
		{
			if (!IsValid(Islands[0].Call) || !IsValid(Islands[1].Call))
			{
				Test->AddError(TEXT("the opposition fixture's call nodes did not survive setup."));
				return;
			}

			// Shape 1: the event path, cancelled.
			bool bCanceledUndo = true;
			bool bCanceledRead = false;
			FString CanceledState;
			{
				const ClaireonBPFaultInjection::FScopedFault Fault(EXQ_PhaseCollapse());
				const IClaireonTool::FToolResult Result = EXQ_Extract(EEXQKind::Event, AssetPath,
					Graph.GetName(), { Islands[0].Call->NodeGuid }, TEXT("status"));
				bCanceledRead = EXQ_UndoAvailable(*Test, Result.Data, bCanceledUndo);
				EXQ_ReadString(*Test, Result.Data, TEXT("mutation_state"), CanceledState);
			}

			// Shape 2: the function path, committed. Same builder, same reported state.
			bool bCommittedUndo = false;
			bool bCommittedRead = false;
			FString CommittedState;
			{
				const ClaireonBPFaultInjection::FScopedFault Fault(EXQ_PhaseCollapse());
				const IClaireonTool::FToolResult Result = EXQ_Extract(EEXQKind::Function, AssetPath,
					Graph.GetName(), { Islands[1].Call->NodeGuid }, TEXT("status"));
				bCommittedRead = EXQ_UndoAvailable(*Test, Result.Data, bCommittedUndo);
				EXQ_ReadString(*Test, Result.Data, TEXT("mutation_state"), CommittedState);
			}

			if (!bCanceledRead || !bCommittedRead)
			{
				Test->AddError(TEXT("one of the two shapes did not report undo_record_available at "
					"all, so the opposition could not be measured. Reported as a measurement "
					"failure rather than as agreement or disagreement."));
				return;
			}

			Test->TestEqual(TEXT("both shapes report the same mutation_state"),
				CanceledState, CommittedState);
			Test->TestEqual(TEXT("and that state is applied_operation_failed"),
				CanceledState, FString(EXQ_StateOperationFailed()));

			Test->TestTrue(
				TEXT("the two shapes DISAGREE on undo_record_available, observed from the two "
				     "returned envelopes rather than from the source"),
				bCanceledUndo != bCommittedUndo);
			Test->TestFalse(TEXT("the canceled shape's observed value is false"), bCanceledUndo);
			Test->TestTrue(TEXT("the committed shape's observed value is true"), bCommittedUndo);
		}

	private:
		FEXQIsland Islands[2];
	};
}

bool FClaireonBPEditorShapesDisagreeOnUndoAvailability::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPExtractQuiescenceInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FEXQ_OppositionCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("ExqOppose"))));
	return true;
}

// Resolving a graph activates its tab and updates focus even with bSetFocus=false.
// Verify extraction affects the resolved graph and independently test the mismatch guard.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorCompositeFocusAssertionHolds,
	"Claireon.BPEditor.Extraction.CompositeFocusAssertionHoldsInEditor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPExtractQuiescenceInternal
{
	static int32 EXQ_CountComposites(const UEdGraph& Graph)
	{
		int32 Count = 0;
		for (const UEdGraphNode* Node : Graph.Nodes)
		{
			if (Cast<const UK2Node_Composite>(Node) != nullptr)
			{
				++Count;
			}
		}
		return Count;
	}

	class FEXQ_FocusAssertionCommand : public FEXQ_FixtureCommand
	{
	public:
		using FEXQ_FixtureCommand::FEXQ_FixtureCommand;

	protected:
		virtual bool BuildFixture(UBlueprint& Blueprint, UEdGraph& Graph, FString& OutError) override
		{
			if (!EXQ_BuildIsland(Blueprint, Graph, TEXT("Focus"), 0, Island, OutError))
			{
				return false;
			}
			// A SECOND graph, so "the resolved graph" and "the focused graph" can differ.
			Other = EXQ_AddFunctionGraph(Blueprint, TEXT("EXQ_OtherGraph"));
			if (!IsValid(Other))
			{
				OutError = TEXT("the second graph could not be added, so the mismatch setup cannot "
					"be constructed and the test would pass for nothing.");
				return false;
			}
			UFunction* PrintString = EXQ_PrintString();
			if (PrintString == nullptr)
			{
				OutError = TEXT("UKismetSystemLibrary::PrintString could not be resolved.");
				return false;
			}
			OtherCall = EXQ_AddCall(*Other, *PrintString, 400, 0);
			return IsValid(OtherCall);
		}

		virtual void RunBody(UBlueprint& Blueprint, UEdGraph& Graph) override
		{
			if (!IsValid(Other) || !IsValid(OtherCall))
			{
				Test->AddError(TEXT("the second graph did not survive fixture setup."));
				return;
			}

			// Focus the ubergraph, then resolve the other graph by name.
			UEdGraph* FocusedBefore = EXQ_FocusAndReport(Blueprint, &Graph);
			if (FocusedBefore != &Graph)
			{
				Test->AddError(FString::Printf(
					TEXT("could not focus '%s' (focused: '%s'), so the mismatch setup does not "
					     "exist and nothing below would prove anything."),
					*Graph.GetName(),
					IsValid(FocusedBefore) ? *FocusedBefore->GetName() : TEXT("<none>")));
				return;
			}
			Test->TestTrue(
				TEXT("EXT-A1: before the call, the graph to resolve is NOT the focused one"),
				Other != FocusedBefore);

			const TSet<FGuid> UbergraphBefore = EXQ_NodeGuids(Graph);
			const int32 UbergraphCompositesBefore = EXQ_CountComposites(Graph);
			const int32 OtherCompositesBefore = EXQ_CountComposites(*Other);

			const IClaireonTool::FToolResult Result = EXQ_Extract(EEXQKind::Composite, AssetPath,
				Other->GetName(), { OtherCall->NodeGuid }, TEXT("status"));

			TSharedPtr<FBlueprintEditor> Editor = EXQ_OpenEditor(Blueprint);
			UEdGraph* FocusedAfter = Editor.IsValid() ? Editor->GetFocusedGraph() : nullptr;
			Test->TestTrue(
				TEXT("EXT-A1: obtaining the graph editor for the resolved graph FOCUSED it -- "
				     "OpenDocument activates the tab and OnTabActivated assigns "
				     "FocusedGraphEdPtr -- which is why the mismatch the gate expected cannot "
				     "arise"),
				FocusedAfter == Other);

			if (Result.bIsError)
			{
				Test->AddError(FString::Printf(
					TEXT("EXT-A1: the composite extraction failed, so the safety property could "
					     "not be measured: %s"), *Result.ErrorMessage));
			}
			else
			{
				EXQ_ExpectString(*Test, Result.Data, TEXT("mutation_state"),
					EXQ_StateApplied(), TEXT("EXT-A1"));
				Test->TestTrue(
					TEXT("EXT-A1: the composite landed in the RESOLVED graph"),
					EXQ_CountComposites(*Other) == OtherCompositesBefore + 1);
				Test->TestTrue(
					TEXT("EXT-A1: the previously-focused graph gained NO composite"),
					EXQ_CountComposites(Graph) == UbergraphCompositesBefore);
				Test->TestTrue(
					TEXT("EXT-A1: the previously-focused graph's node set is unchanged"),
					EXQ_NodeGuids(Graph).Difference(UbergraphBefore).Num() == 0
						&& UbergraphBefore.Difference(EXQ_NodeGuids(Graph)).Num() == 0);
				Test->TestFalse(
					TEXT("EXT-A1: the selected node left the resolved graph"),
					EXQ_NodeGuids(*Other).Contains(OtherCall->NodeGuid));
			}

			// Exercise the focus guard with matching, different, and absent graphs.
			{
				const FString Mismatch =
					ClaireonExtractQuiescence::FocusedGraphMismatchRefusal(Other, &Graph);
				Test->TestFalse(TEXT("EXT-A1: a DIFFERENT focused graph is refused"),
					Mismatch.IsEmpty());
				Test->TestTrue(TEXT("EXT-A1: the refusal names the resolved graph"),
					Mismatch.Contains(Other->GetName()));
				Test->TestTrue(TEXT("EXT-A1: the refusal names the focused graph"),
					Mismatch.Contains(Graph.GetName()));

				// The refusal precedes mutation; its claim does not depend on a snapshot.
				Test->TestTrue(TEXT("EXT-A1: the refusal states no transaction was opened"),
					Mismatch.Contains(TEXT("no transaction was opened")));
				Test->TestTrue(TEXT("EXT-A1: the refusal states no mutating API was called"),
					Mismatch.Contains(TEXT("no mutating API was called")));
			}
			{
				const FString NoFocus =
					ClaireonExtractQuiescence::FocusedGraphMismatchRefusal(Other, nullptr);
				Test->TestFalse(TEXT("EXT-A1: NO focused graph is refused too"), NoFocus.IsEmpty());
				Test->TestTrue(TEXT("EXT-A1: and the refusal says which case it is"),
					NoFocus.Contains(TEXT("no focused graph")));
			}
			Test->TestTrue(TEXT("EXT-A1: a matching focused graph is NOT refused"),
				ClaireonExtractQuiescence::FocusedGraphMismatchRefusal(&Graph, &Graph).IsEmpty());
		}

	private:
		FEXQIsland Island;
		UEdGraph* Other = nullptr;
		UK2Node_CallFunction* OtherCall = nullptr;
	};
}

bool FClaireonBPEditorCompositeFocusAssertionHolds::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPExtractQuiescenceInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FEXQ_FocusAssertionCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("ExqFocus"))));
	return true;
}

// A compile failure after retained mutation must produce a failed result.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorFailedCompileIsNotSuccess,
	"Claireon.BPEditor.Extraction.FailedCompileIsNotReportedAsSuccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPExtractQuiescenceInternal
{
	class FEXQ_FailedCompileCommand : public FEXQ_FixtureCommand
	{
	public:
		using FEXQ_FixtureCommand::FEXQ_FixtureCommand;

	protected:
		virtual bool BuildFixture(UBlueprint& Blueprint, UEdGraph& Graph, FString& OutError) override
		{
			return EXQ_BuildIsland(Blueprint, Graph, TEXT("Compile"), 0, Island, OutError)
				&& EXQ_BuildUncompilableIsland(Graph, 1, OutError);
		}

		virtual void RunBody(UBlueprint& Blueprint, UEdGraph& Graph) override
		{
			if (!IsValid(Island.Call))
			{
				Test->AddError(TEXT("the island's call node did not survive fixture setup."));
				return;
			}
			const FGuid SelectedGuid = Island.Call->NodeGuid;
			const FEXQTxnWindow Window = EXQ_OpenWindow(*Test);

			const IClaireonTool::FToolResult Result = EXQ_Extract(EEXQKind::Function, AssetPath,
				Graph.GetName(), { SelectedGuid }, TEXT("status"));

			Test->TestEqual(
				TEXT("EXT-R3: the Blueprint really is in BS_Error after the tool's compile"),
				static_cast<int32>(Blueprint.Status), static_cast<int32>(BS_Error));

			Test->TestTrue(TEXT("EXT-R3: a failed compile is an error result, never summary: ok"),
				Result.bIsError);
			if (!Result.Data.IsValid())
			{
				Test->AddError(TEXT("EXT-R3 returned no structured data."));
				return;
			}

			EXQ_ExpectString(*Test, Result.Data, TEXT("mutation_state"),
				EXQ_StateValidationFailed(), TEXT("EXT-R3"));
			EXQ_ExpectBool(*Test, Result.Data, TEXT("mutation_retained"), true, TEXT("EXT-R3"));
			EXQ_ExpectString(*Test, Result.Data, TEXT("failed_phase"),
				*EXQ_PhaseCompileValidate(), TEXT("EXT-R3"));
			EXQ_ExpectString(*Test, Result.Data, TEXT("last_completed_phase"),
				*EXQ_PhaseCollapse(), TEXT("EXT-R3"));
			EXQ_ExpectString(*Test, Result.Data, TEXT("engine_compile_status"),
				TEXT("failed"), TEXT("EXT-R3"));
			EXQ_ExpectBool(*Test, Result.Data, TEXT("undo_record_available"), true, TEXT("EXT-R3"));
			EXQ_ExpectString(*Test, Result.Data, TEXT("target_graph"),
				*Graph.GetName(), TEXT("EXT-R3"));
			EXQ_ExpectBool(*Test, Result.Data, TEXT("session_graph_stale"), true, TEXT("EXT-R3"));

			// A compile ran and failed; asset_compiled=false would incorrectly imply it was skipped.
			EXQ_ExpectAbsent(*Test, Result.Data, TEXT("asset_dirty"),
				TEXT("a compile ran and failed; engine_compile_status carries that"));
			EXQ_ExpectAbsent(*Test, Result.Data, TEXT("asset_compiled"),
				TEXT("a compile ran and failed; asset_compiled: false would claim none was attempted"));

			EXQ_ExpectDeltaProvesDurableEffect(*Test, Result.Data, TEXT("EXT-R3"));

			Test->TestFalse(TEXT("EXT-R3: the extraction applied -- the selection left the graph"),
				EXQ_NodeGuids(Graph).Contains(SelectedGuid));

			EXQ_CrossCheckUndo(*Test, Window, EEXQKind::Function,
				/*bEnvelopeSaysUndoAvailable=*/true, TEXT("EXT-R3"));
		}

	private:
		FEXQIsland Island;
	};
}

bool FClaireonBPEditorFailedCompileIsNotSuccess::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPExtractQuiescenceInternal;

	// Register expected compiler errors before the latent command's first compile; structural changes can emit them repeatedly.
	AddExpectedMessagePlain(TEXT("cannot have more than one connection"),
		EAutomationExpectedMessageFlags::Contains, /*Occurrences=*/0);

	ADD_LATENT_AUTOMATION_COMMAND(FEXQ_FailedCompileCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("ExqCompile"))));
	return true;
}

// Successful extraction reports clean mutation state for all kinds and both response modes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorSuccessPathsStayClean,
	"Claireon.BPEditor.Extraction.SuccessPathsStayCleanOnAllFourKinds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPExtractQuiescenceInternal
{
	class FEXQ_SuccessCommand : public FEXQ_FixtureCommand
	{
	public:
		using FEXQ_FixtureCommand::FEXQ_FixtureCommand;

	protected:
		virtual bool BuildFixture(UBlueprint& Blueprint, UEdGraph& Graph, FString& OutError) override
		{
			return EXQ_BuildIsland(Blueprint, Graph, TEXT("OkFn"), 0, Islands[0], OutError)
				&& EXQ_BuildIsland(Blueprint, Graph, TEXT("OkMac"), 1, Islands[1], OutError)
				&& EXQ_BuildIsland(Blueprint, Graph, TEXT("OkComp"), 2, Islands[2], OutError)
				&& EXQ_BuildIsland(Blueprint, Graph, TEXT("OkEvt"), 3, Islands[3], OutError);
		}

		virtual void RunBody(UBlueprint& Blueprint, UEdGraph& Graph) override
		{
			static const EEXQKind Kinds[4] =
				{ EEXQKind::Function, EEXQKind::Macro, EEXQKind::Composite, EEXQKind::Event };
			// Exercise the full-response branch alongside the default status response.
			static const TCHAR* Modes[4] =
				{ TEXT("status"), TEXT("status"), TEXT("status"), TEXT("full") };

			for (int32 Index = 0; Index < 4; ++Index)
			{
				const EEXQKind Kind = Kinds[Index];
				const FString What = FString::Printf(TEXT("EXT-R5/%s"), EXQ_KindName(Kind));

				if (!IsValid(Islands[Index].Call))
				{
					Test->AddError(FString::Printf(
						TEXT("%s: the island's call node did not survive setup."), *What));
					continue;
				}
				const FGuid SelectedGuid = Islands[Index].Call->NodeGuid;

				if (Kind == EEXQKind::Composite)
				{
					// Refocus after earlier compiles rebuild graph widgets.
					UEdGraph* Focused = EXQ_FocusAndReport(Blueprint, &Graph);
					if (Focused != &Graph)
					{
						Test->AddError(FString::Printf(
							TEXT("%s: could not focus '%s' (focused: '%s'), so the composite path "
							     "would refuse and a clean success is not measurable."),
							*What, *Graph.GetName(),
							IsValid(Focused) ? *Focused->GetName() : TEXT("<none>")));
						continue;
					}
				}

				const IClaireonTool::FToolResult Result = EXQ_Extract(Kind, AssetPath,
					Graph.GetName(), { SelectedGuid }, Modes[Index]);

				if (Result.bIsError)
				{
					Test->AddError(FString::Printf(
						TEXT("%s: a normal extraction FAILED after the gate 3c repair: %s"),
						*What, *Result.ErrorMessage));
					continue;
				}
				if (!Result.Data.IsValid())
				{
					Test->AddError(FString::Printf(
						TEXT("%s: succeeded with no structured data."), *What));
					continue;
				}

				EXQ_ExpectString(*Test, Result.Data, TEXT("mutation_state"),
					EXQ_StateApplied(), *What);
				EXQ_ExpectBool(*Test, Result.Data, TEXT("mutation_retained"), false, *What);
				EXQ_ExpectString(*Test, Result.Data, TEXT("last_completed_phase"),
					*EXQ_PhaseCompileValidate(), *What);
				EXQ_ExpectString(*Test, Result.Data, TEXT("engine_compile_status"),
					TEXT("succeeded"), *What);
				EXQ_ExpectBool(*Test, Result.Data, TEXT("undo_record_available"), true, *What);
				EXQ_ExpectString(*Test, Result.Data, TEXT("extract_kind"),
					EXQ_KindName(Kind), *What);

				EXQ_ExpectAbsent(*Test, Result.Data, TEXT("failed_phase"),
					TEXT("no phase failed on a clean extraction"));
				EXQ_ExpectAbsent(*Test, Result.Data, TEXT("asset_dirty"),
					TEXT("the asset was compiled, so the dirty-and-uncompiled pair is not emitted"));

				FString ExtractedGraph;
				FString GatewayNode;
				EXQ_ReadString(*Test, Result.Data, TEXT("extracted_graph"), ExtractedGraph);
				EXQ_ReadString(*Test, Result.Data, TEXT("gateway_node"), GatewayNode);
				Test->TestFalse(FString::Printf(TEXT("%s: extracted_graph is named"), *What),
					ExtractedGraph.IsEmpty());
				Test->TestTrue(
					FString::Printf(TEXT("%s: a gateway node was identified"), *What),
					!GatewayNode.Equals(TEXT("<null>")) && !GatewayNode.IsEmpty());

				double NodesExtracted = -1.0;
				if (Result.Data->TryGetNumberField(TEXT("nodes_extracted"), NodesExtracted))
				{
					Test->TestEqual(FString::Printf(TEXT("%s: nodes_extracted"), *What),
						static_cast<int32>(NodesExtracted), 1);
				}
				else
				{
					Test->AddError(FString::Printf(TEXT("%s: nodes_extracted is absent."), *What));
				}

				Test->TestTrue(FString::Printf(
					TEXT("%s: the Blueprint is not in BS_Error after a clean extraction"), *What),
					Blueprint.Status != BS_Error);

				// The event path replaces the call site rather than relocating it, so it is the
				// one kind whose selected node stays in the graph. Every other kind moved it.
				const bool bStillPresent = EXQ_NodeGuids(Graph).Contains(SelectedGuid);
				if (Kind == EEXQKind::Event)
				{
					Test->TestTrue(FString::Printf(
						TEXT("%s: the event path leaves the selection in place and rewires it"),
						*What), bStillPresent);
				}
				else
				{
					Test->TestFalse(FString::Printf(
						TEXT("%s: the selection was relocated out of the source graph"), *What),
						bStillPresent);
				}
			}
		}

	private:
		FEXQIsland Islands[4];
	};
}

bool FClaireonBPEditorSuccessPathsStayClean::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPExtractQuiescenceInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FEXQ_SuccessCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("ExqOk"))));
	return true;
}

#endif // WITH_CLAIREON_TESTS && WITH_EDITOR
