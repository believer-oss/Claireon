// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Verify selection against both candidate widgets over the same graph, not just a tool round trip.
// Candidates use the binding seam because normal editor opening reuses the existing instance.
// Create and open fixtures in separate updates.

#include "Misc/AutomationTest.h"

#if WITH_CLAIREON_TESTS && WITH_EDITOR

#include "Tests/ClaireonBPEditorFixtures.h"

#include "ClaireonBlueprintHelpers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/Blueprint.h"
#include "GraphEditor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Tools/ClaireonBlueprintGraphTool_Open.h"
#include "Tools/ClaireonBlueprintGraphTool_SelectNode.h"
#include "Tools/ClaireonBlueprintGraphTool_Selection.h"

namespace ClaireonBPEditorSelectionInternal
{
	// Prefix helpers to avoid unity-build collisions.

	static IClaireonTool::FToolResult SEL_Get(const FString& SessionId)
	{
		ClaireonBlueprintGraphTool_SelectionGet Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		return Tool.Execute(Args);
	}

	static IClaireonTool::FToolResult SEL_Set(const FString& SessionId, const TArray<FString>& Guids)
	{
		ClaireonBlueprintGraphTool_SelectionSet Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Guid : Guids)
		{
			Values.Add(MakeShared<FJsonValueString>(Guid));
		}
		Args->SetArrayField(TEXT("node_guids"), Values);
		return Tool.Execute(Args);
	}

	static IClaireonTool::FToolResult SEL_Clear(const FString& SessionId)
	{
		ClaireonBlueprintGraphTool_SelectionClear Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		return Tool.Execute(Args);
	}

	static FString SEL_Str(const IClaireonTool::FToolResult& R, const TCHAR* Field)
	{
		FString Value;
		if (R.Data.IsValid())
		{
			R.Data->TryGetStringField(Field, Value);
		}
		return Value;
	}

	static FString SEL_HintTool(const IClaireonTool::FToolResult& R)
	{
		FString Value;
		if (R.Hints.Num() > 0)
		{
			R.Hints[0]->TryGetStringField(TEXT("tool"), Value);
		}
		return Value;
	}

	/** The node GUIDs a widget currently has selected, sorted so comparison is stable. */
	static TArray<FString> SEL_WidgetSelection(const TSharedRef<SGraphEditor>& Widget)
	{
		TArray<FString> Out;
		for (UObject* Selected : Widget->GetSelectedNodes())
		{
			if (const UEdGraphNode* Node = Cast<UEdGraphNode>(Selected); IsValid(Node))
			{
				Out.Add(Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			}
		}
		Out.Sort();
		return Out;
	}

	static TArray<FString> SEL_ReportedSelection(const IClaireonTool::FToolResult& R)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (R.Data.IsValid() && R.Data->TryGetArrayField(TEXT("selected_node_guids"), Values) && Values)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Guid;
				if (Value.IsValid() && Value->TryGetString(Guid))
				{
					Out.Add(Guid);
				}
			}
		}
		Out.Sort();
		return Out;
	}

	static FString SEL_Guid(const UEdGraphNode* Node)
	{
		return Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
	}

	/** Open the session in a later update than fixture creation to avoid reentrant loading. */
	class FSel_FixtureCommand : public IAutomationLatentCommand
	{
	public:
		FSel_FixtureCommand(FAutomationTestBase* InTest, const FString& InAssetPath)
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
				if (Graph->Nodes.Num() < 2)
				{
					Test->AddError(FString::Printf(
						TEXT("the fixture graph has %d node(s); these tests need at least two "
						     "so the two candidates can hold DIFFERENT selections."),
						Graph->Nodes.Num()));
					Phase = EPhase::Teardown;
					return false;
				}

				ClaireonBlueprintGraphTool_Open OpenTool;
				TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
				Args->SetStringField(TEXT("asset_path"), AssetPath);
				const IClaireonTool::FToolResult Opened = OpenTool.Execute(Args);
				if (Opened.bIsError || !Opened.Data.IsValid()
					|| !Opened.Data->TryGetStringField(TEXT("session_id"), SessionId))
				{
					Test->AddError(FString::Printf(TEXT("bp_open failed: %s"), *Opened.ErrorMessage));
					Phase = EPhase::Teardown;
					return false;
				}
				Data = ClaireonBlueprintGraphEditToolBase::FindToolData(SessionId);
				if (!Data)
				{
					Test->AddError(TEXT("the session reported an id it does not have tool data for."));
					Phase = EPhase::Teardown;
					return false;
				}

				RunBody(*Blueprint, *Graph);

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
		virtual void RunBody(UBlueprint& Blueprint, UEdGraph& Graph) = 0;

		FAutomationTestBase* Test = nullptr;
		FString AssetPath;
		FString SessionId;
		FBlueprintEditToolData* Data = nullptr;

	private:
		enum class EPhase : uint8 { Create, Settle, Body, Teardown };
		EPhase Phase = EPhase::Create;
		int32 SettleUpdates = 0;
	};
}

using namespace ClaireonBPEditorSelectionInternal;


// Selection changes target the bound widget among two widgets sharing a graph.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorSelectionTargetsTheBoundWidget,
	"Claireon.BPEditor.Selection.TargetsTheBoundWidgetNotTheOtherCandidate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorSelectionInternal
{
	class FSel_TargetsBoundCommand : public FSel_FixtureCommand
	{
	public:
		using FSel_FixtureCommand::FSel_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& Blueprint, UEdGraph& Graph) override
		{
			ClaireonBlueprintEditorBindingSeam::FScopedSeam Seam;

			UEdGraphNode* NodeA = Graph.Nodes[0];
			UEdGraphNode* NodeB = Graph.Nodes[1];

			const TSharedRef<SGraphEditor> WidgetA = SNew(SGraphEditor).GraphToEdit(&Graph);
			const TSharedRef<SGraphEditor> WidgetB = SNew(SGraphEditor).GraphToEdit(&Graph);
			if (WidgetA == WidgetB)
			{
				Test->AddError(TEXT("the two candidates are the same widget; this test cannot "
				                    "distinguish the targeted one."));
				return;
			}

			// Seed different selections so isolation is observable.
			WidgetA->SetNodeSelection(NodeB, true);
			WidgetB->SetNodeSelection(NodeA, true);

			const FGuid InstanceA = ClaireonBlueprintEditorBindingSeam::RegisterInstance(&Graph, WidgetA);
			ClaireonBlueprintEditorBindingSeam::RegisterInstance(&Graph, WidgetB);
			Data->EditorBinding.BindToSeamInstance(&Blueprint, &Graph, InstanceA);

			const TArray<FString> SeedB = SEL_WidgetSelection(WidgetB);

			const IClaireonTool::FToolResult Set = SEL_Set(SessionId, { SEL_Guid(NodeA) });
			if (Set.bIsError)
			{
				Test->AddError(FString::Printf(TEXT("bp_selection_set failed: %s"), *Set.ErrorMessage));
				return;
			}

			Test->TestEqual(TEXT("the bound widget holds exactly the requested node"),
				SEL_WidgetSelection(WidgetA), TArray<FString>{ SEL_Guid(NodeA) });
			Test->TestEqual(TEXT("the other candidate was not touched"),
				SEL_WidgetSelection(WidgetB), SeedB);

			const IClaireonTool::FToolResult Got = SEL_Get(SessionId);
			Test->TestFalse(TEXT("bp_selection_get succeeds"), Got.bIsError);
			Test->TestEqual(TEXT("get reports what the bound widget actually holds"),
				SEL_ReportedSelection(Got), SEL_WidgetSelection(WidgetA));

			const IClaireonTool::FToolResult Cleared = SEL_Clear(SessionId);
			Test->TestFalse(TEXT("bp_selection_clear succeeds"), Cleared.bIsError);
			Test->TestEqual(TEXT("the bound widget is empty"),
				SEL_WidgetSelection(WidgetA).Num(), 0);
			Test->TestEqual(TEXT("the other candidate is STILL untouched by the clear"),
				SEL_WidgetSelection(WidgetB), SeedB);
		}
	};
}

bool FClaireonBPEditorSelectionTargetsTheBoundWidget::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FSel_TargetsBoundCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("SEL_Targets"))));
	return true;
}


// Reject invalid GUIDs before changing selection; undo cannot restore Slate selection state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorSelectionRefusesAtomically,
	"Claireon.BPEditor.Selection.BadGuidRefusesWithoutChangingTheSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorSelectionInternal
{
	class FSel_AtomicCommand : public FSel_FixtureCommand
	{
	public:
		using FSel_FixtureCommand::FSel_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& Blueprint, UEdGraph& Graph) override
		{
			ClaireonBlueprintEditorBindingSeam::FScopedSeam Seam;

			UEdGraphNode* NodeA = Graph.Nodes[0];
			UEdGraphNode* NodeB = Graph.Nodes[1];

			const TSharedRef<SGraphEditor> Widget = SNew(SGraphEditor).GraphToEdit(&Graph);
			const FGuid InstanceId = ClaireonBlueprintEditorBindingSeam::RegisterInstance(&Graph, Widget);
			Data->EditorBinding.BindToSeamInstance(&Blueprint, &Graph, InstanceId);

			Test->TestFalse(TEXT("seeding succeeds"), SEL_Set(SessionId, { SEL_Guid(NodeB) }).bIsError);
			const TArray<FString> Seeded = SEL_WidgetSelection(Widget);
			Test->TestEqual(TEXT("the seed took"), Seeded, TArray<FString>{ SEL_Guid(NodeB) });

			const IClaireonTool::FToolResult Refused = SEL_Set(SessionId,
				{ SEL_Guid(NodeA), TEXT("DEADBEEFDEADBEEFDEADBEEFDEADBEEF") });
			Test->TestTrue(TEXT("the whole set is refused"), Refused.bIsError);
			Test->TestEqual(TEXT("and the reason names the unresolved node, not the window"),
				SEL_Str(Refused, TEXT("refusal_reason")), FString(TEXT("node_not_found")));
			Test->TestEqual(TEXT("the seeded selection is exactly as it was"),
				SEL_WidgetSelection(Widget), Seeded);

			const IClaireonTool::FToolResult Deduped = SEL_Set(SessionId,
				{ SEL_Guid(NodeA), SEL_Guid(NodeA), SEL_Guid(NodeB) });
			Test->TestFalse(TEXT("a duplicated GUID is accepted"), Deduped.bIsError);
			double Removed = -1.0;
			if (Deduped.Data.IsValid())
			{
				Deduped.Data->TryGetNumberField(TEXT("duplicates_removed"), Removed);
			}
			Test->TestEqual(TEXT("and the duplicate is reported as removed"), Removed, 1.0);
			TArray<FString> Expected = { SEL_Guid(NodeA), SEL_Guid(NodeB) };
			Expected.Sort();
			Test->TestEqual(TEXT("both distinct nodes are selected"),
				SEL_WidgetSelection(Widget), Expected);
		}
	};
}

bool FClaireonBPEditorSelectionRefusesAtomically::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FSel_AtomicCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("SEL_Atomic"))));
	return true;
}


// Selection changes create no undo record.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorSelectionOpensNoTransaction,
	"Claireon.BPEditor.Selection.SelectingCreatesNoUndoEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorSelectionInternal
{
	class FSel_NoTransactionCommand : public FSel_FixtureCommand
	{
	public:
		using FSel_FixtureCommand::FSel_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& Blueprint, UEdGraph& Graph) override
		{
			ClaireonBlueprintEditorBindingSeam::FScopedSeam Seam;

			UTransBuffer* Buffer = IsValid(GEditor) ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
			if (!IsValid(Buffer))
			{
				Test->AddError(TEXT("no transaction buffer, so 'created no undo entry' is not "
				                    "measurable here."));
				return;
			}

			const TSharedRef<SGraphEditor> Widget = SNew(SGraphEditor).GraphToEdit(&Graph);
			const FGuid InstanceId = ClaireonBlueprintEditorBindingSeam::RegisterInstance(&Graph, Widget);
			Data->EditorBinding.BindToSeamInstance(&Blueprint, &Graph, InstanceId);

			const int32 QueueBefore = Buffer->GetQueueLength();
			const int32 UndoneBefore = Buffer->GetUndoCount();

			Test->TestFalse(TEXT("set succeeds"), SEL_Set(SessionId, { SEL_Guid(Graph.Nodes[0]) }).bIsError);
			Test->TestFalse(TEXT("clear succeeds"), SEL_Clear(SessionId).bIsError);

			Test->TestEqual(TEXT("no transaction was appended"),
				Buffer->GetQueueLength(), QueueBefore);
			Test->TestEqual(TEXT("and the undone region did not move"),
				Buffer->GetUndoCount(), UndoneBefore);
		}
	};
}

bool FClaireonBPEditorSelectionOpensNoTransaction::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FSel_NoTransactionCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("SEL_NoTxn"))));
	return true;
}


// Remove only the widget to test recovery for an otherwise valid binding.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorSelectionNoWindowRefusalIsExercised,
	"Claireon.BPEditor.Selection.LostWidgetRefusesAndNamesTheRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorSelectionInternal
{
	class FSel_NoWindowCommand : public FSel_FixtureCommand
	{
	public:
		using FSel_FixtureCommand::FSel_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& Blueprint, UEdGraph& Graph) override
		{
			ClaireonBlueprintEditorBindingSeam::FScopedSeam Seam;

			const TSharedRef<SGraphEditor> Widget = SNew(SGraphEditor).GraphToEdit(&Graph);
			const FGuid InstanceId = ClaireonBlueprintEditorBindingSeam::RegisterInstance(&Graph, Widget);
			Data->EditorBinding.BindToSeamInstance(&Blueprint, &Graph, InstanceId);

			const FString GoodGuid = SEL_Guid(Graph.Nodes[0]);
			Test->TestFalse(TEXT("the selection tools work before the widget is removed"),
				SEL_Set(SessionId, { GoodGuid }).bIsError);
			Test->TestFalse(TEXT("and get works too"), SEL_Get(SessionId).bIsError);

			ClaireonBlueprintEditorBindingSeam::CloseGraphTab(InstanceId);
			Test->TestTrue(TEXT("the instance is still registered"),
				ClaireonBlueprintEditorBindingSeam::IsInstanceRegistered(InstanceId));

			const IClaireonTool::FToolResult Refused = SEL_Set(SessionId, { GoodGuid });
			Test->TestTrue(TEXT("the call refuses"), Refused.bIsError);
			Test->TestEqual(TEXT("with the no-window reason"),
				SEL_Str(Refused, TEXT("refusal_reason")), FString(TEXT("no_editor_window")));
			Test->TestEqual(TEXT("and the binding evidence says the tab went, not the editor"),
				SEL_Str(Refused, TEXT("binding")), FString(TEXT("graph_tab_closed")));
			Test->TestEqual(TEXT("and the hint names the tool that replaces a dead session"),
				SEL_HintTool(Refused), FString(TEXT("bp_close")));

			Test->TestEqual(TEXT("get refuses identically"),
				SEL_Str(SEL_Get(SessionId), TEXT("refusal_reason")), FString(TEXT("no_editor_window")));
			Test->TestEqual(TEXT("clear refuses identically"),
				SEL_Str(SEL_Clear(SessionId), TEXT("refusal_reason")), FString(TEXT("no_editor_window")));

			// A session with no binding requires bp_open instead.
			Data->EditorBinding.Clear();
			const IClaireonTool::FToolResult Unbound = SEL_Get(SessionId);
			Test->TestTrue(TEXT("an unbound session also refuses"), Unbound.bIsError);
			Test->TestEqual(TEXT("with the same reason"),
				SEL_Str(Unbound, TEXT("refusal_reason")), FString(TEXT("no_editor_window")));
			Test->TestEqual(TEXT("but different evidence"),
				SEL_Str(Unbound, TEXT("binding")), FString(TEXT("not_bound")));
			Test->TestEqual(TEXT("and a different recovery: open a window, not a new session"),
				SEL_HintTool(Unbound), FString(TEXT("bp_open")));
		}
	};
}

bool FClaireonBPEditorSelectionNoWindowRefusalIsExercised::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FSel_NoWindowCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("SEL_NoWindow"))));
	return true;
}


// Cursor movement changes the view while preserving selection.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorSelectionCursorDoesNotSelect,
	"Claireon.BPEditor.Selection.CursorMovesTheViewNotTheSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorSelectionInternal
{
	class FSel_CursorCommand : public FSel_FixtureCommand
	{
	public:
		using FSel_FixtureCommand::FSel_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& Blueprint, UEdGraph& Graph) override
		{
			ClaireonBlueprintEditorBindingSeam::FScopedSeam Seam;

			UEdGraphNode* NodeA = Graph.Nodes[0];
			UEdGraphNode* NodeB = Graph.Nodes[1];

			const TSharedRef<SGraphEditor> Widget = SNew(SGraphEditor).GraphToEdit(&Graph);
			const FGuid InstanceId = ClaireonBlueprintEditorBindingSeam::RegisterInstance(&Graph, Widget);
			Data->EditorBinding.BindToSeamInstance(&Blueprint, &Graph, InstanceId);

			Test->TestFalse(TEXT("seeding the selection succeeds"),
				SEL_Set(SessionId, { SEL_Guid(NodeA) }).bIsError);
			const TArray<FString> Seeded = SEL_WidgetSelection(Widget);

			ClaireonBlueprintGraphTool_SelectNode CursorTool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("session_id"), SessionId);
			Args->SetStringField(TEXT("node_guid"), SEL_Guid(NodeB));
			Test->TestFalse(TEXT("bp_cursor_to_node succeeds"), CursorTool.Execute(Args).bIsError);
			Test->TestTrue(TEXT("the cursor really did move"),
				Data->Cursor.FocusedNodeGuid == NodeB->NodeGuid);

			Test->TestEqual(TEXT("the selection is untouched by the cursor move"),
				SEL_WidgetSelection(Widget), Seeded);
			Test->TestEqual(TEXT("and bp_selection_get agrees"),
				SEL_ReportedSelection(SEL_Get(SessionId)), Seeded);
		}
	};
}

bool FClaireonBPEditorSelectionCursorDoesNotSelect::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FSel_CursorCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("SEL_Cursor"))));
	return true;
}

#endif // WITH_CLAIREON_TESTS && WITH_EDITOR
