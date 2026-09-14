// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Cross-check transaction recovery reports against graph state and transaction identities.
// Equal titles do not identify which edits were undone. Fault injection exercises failed undo paths.
// Do not use the absence of Format Node Added as recovery evidence: project settings disable it.

#include "Misc/AutomationTest.h"

#if WITH_CLAIREON_TESTS && WITH_EDITOR

#include "Tests/ClaireonBPEditorFixtures.h"
#include "Tools/ClaireonBPSnapshot.h"
#include "Tools/ClaireonBlueprintGraphTool_MoveNode.h"
#include "Tools/ClaireonBlueprintGraphTool_RemoveNode.h"
#include "Tools/ClaireonTool_TransactionBeginGroup.h"
#include "Tools/ClaireonTool_TransactionEndGroup.h"
#include "Tools/ClaireonTool_TransactionRedo.h"
#include "Tools/ClaireonTool_TransactionRollbackGroup.h"
#include "Tools/ClaireonTool_TransactionUndo.h"
#include "Tools/ClaireonTransactionGroupState.h"

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
#include "K2Node_CustomEvent.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/Guid.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace ClaireonBPEditorTxnShapesInternal
{
	// Prefix helpers to avoid unity-build collisions.

	// Centralize the acknowledgement argument used by rollback tests; default-refusal tests omit it.
	static constexpr TCHAR AcknowledgeUnsettledFormattingArg[] = TEXT("acknowledge_unsettled_formatting");

	/** Reason identifying an unsettled-formatting refusal. */
	static constexpr TCHAR RefusalReasonUnprovable[] = TEXT("formatting_settlement_unprovable");

	// Envelope readers reject absent fields.

	static bool TXS_ReadBool(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field, bool& Out)
	{
		if (!Data.IsValid() || !Data->TryGetBoolField(Field, Out))
		{
			Test.AddError(FString::Printf(
				TEXT("the result omitted '%s'. An absent field must never read as false -- this gate "
					 "exists because tools reported facts they had not established."), Field));
			return false;
		}
		return true;
	}

	static bool TXS_ReadInt(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field, int32& Out)
	{
		double AsNumber = 0.0;
		if (!Data.IsValid() || !Data->TryGetNumberField(Field, AsNumber))
		{
			Test.AddError(FString::Printf(TEXT("the result omitted '%s'."), Field));
			return false;
		}
		Out = static_cast<int32>(AsNumber);
		return true;
	}

	static bool TXS_ReadString(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field, FString& Out)
	{
		if (!Data.IsValid() || !Data->TryGetStringField(Field, Out))
		{
			Test.AddError(FString::Printf(TEXT("the result omitted '%s'."), Field));
			return false;
		}
		return true;
	}

	static bool TXS_ReadStringArray(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field, TArray<FString>& Out)
	{
		Out.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(Field, Array) || Array == nullptr)
		{
			Test.AddError(FString::Printf(TEXT("the result omitted the '%s' array."), Field));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Array)
		{
			Out.Add(Value.IsValid() ? Value->AsString() : FString());
		}
		return true;
	}

	/** TestEqual on FString is case-INSENSITIVE, so string equality goes through this. */
	static void TXS_ExpectString(FAutomationTestBase& Test, const TCHAR* What,
		const FString& Observed, const FString& Expected)
	{
		Test.TestTrue(
			FString::Printf(TEXT("%s: expected '%s', observed '%s'"), What, *Expected, *Observed),
			Observed.Equals(Expected, ESearchCase::CaseSensitive));
	}

	static void TXS_ExpectBoolField(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field, bool bExpected)
	{
		bool bObserved = !bExpected;
		if (TXS_ReadBool(Test, Data, Field, bObserved))
		{
			Test.TestTrue(FString::Printf(TEXT("%s is %s"), Field, bExpected ? TEXT("true") : TEXT("false")),
				bObserved == bExpected);
		}
	}

	static void TXS_ExpectIntField(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field, int32 Expected)
	{
		int32 Observed = Expected - 1;
		if (TXS_ReadInt(Test, Data, Field, Observed))
		{
			Test.TestEqual(FString::Printf(TEXT("%s"), Field), Observed, Expected);
		}
	}

	/** The tool named by a {tool, reason} guidance hint, or empty when there is no hint. */
	static FString TXS_HintTool(const IClaireonTool::FToolResult& Result)
	{
		FString ToolName;
		if (Result.Hints.Num() > 0)
		{
			Result.Hints[0]->TryGetStringField(TEXT("tool"), ToolName);
		}
		return ToolName;
	}


	/** Count output links only so each wire appears once. */
	struct FGraphShape
	{
		int32 NodeCount = 0;
		int32 LinkCount = 0;
		TSet<FGuid> NodeGuids;

		bool Matches(const FGraphShape& Other) const
		{
			return NodeCount == Other.NodeCount
				&& LinkCount == Other.LinkCount
				&& NodeGuids.Difference(Other.NodeGuids).Num() == 0
				&& Other.NodeGuids.Difference(NodeGuids).Num() == 0;
		}

		FString Describe() const
		{
			return FString::Printf(TEXT("%d node(s), %d wire(s)"), NodeCount, LinkCount);
		}
	};

	static FGraphShape TXS_Capture(const UEdGraph* Graph)
	{
		FGraphShape Shape;
		if (!IsValid(Graph))
		{
			return Shape;
		}
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node))
			{
				continue;
			}
			++Shape.NodeCount;
			Shape.NodeGuids.Add(Node->NodeGuid);
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin != nullptr && Pin->Direction == EGPD_Output)
				{
					Shape.LinkCount += Pin->LinkedTo.Num();
				}
			}
		}
		return Shape;
	}

	static void TXS_ExpectShape(FAutomationTestBase& Test, const TCHAR* What,
		const FGraphShape& Observed, const FGraphShape& Expected)
	{
		Test.TestTrue(
			FString::Printf(TEXT("%s: expected %s with the same node identities, observed %s"),
				What, *Expected.Describe(), *Observed.Describe()),
			Observed.Matches(Expected));
	}

	static UEdGraphNode* TXS_FindNode(UEdGraph* Graph, const FGuid& NodeGuid)
	{
		if (IsValid(Graph))
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (IsValid(Node) && Node->NodeGuid == NodeGuid)
				{
					return Node;
				}
			}
		}
		return nullptr;
	}

	/** Compare positions and function references as well as node identity. */
	struct FNodeFacts
	{
		bool bPresent = false;
		int32 PosX = 0;
		int32 PosY = 0;
		FString MemberName;

		FString Describe() const
		{
			return bPresent
				? FString::Printf(TEXT("present at (%d,%d) calling '%s'"), PosX, PosY, *MemberName)
				: FString(TEXT("absent"));
		}
	};

	static FNodeFacts TXS_ReadNodeFacts(UEdGraph* Graph, const FGuid& NodeGuid)
	{
		FNodeFacts Facts;
		UEdGraphNode* Node = TXS_FindNode(Graph, NodeGuid);
		if (!IsValid(Node))
		{
			return Facts;
		}
		Facts.bPresent = true;
		Facts.PosX = Node->NodePosX;
		Facts.PosY = Node->NodePosY;
		if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node); IsValid(Call))
		{
			Facts.MemberName = Call->FunctionReference.GetMemberName().ToString();
		}
		return Facts;
	}

	static void TXS_ExpectNodeFacts(FAutomationTestBase& Test, const TCHAR* What,
		const FNodeFacts& Observed, const FNodeFacts& Expected)
	{
		Test.TestTrue(
			FString::Printf(TEXT("%s: expected the node %s, observed %s"),
				What, *Expected.Describe(), *Observed.Describe()),
			Observed.bPresent == Expected.bPresent
				&& Observed.PosX == Expected.PosX
				&& Observed.PosY == Expected.PosY
				&& Observed.MemberName.Equals(Expected.MemberName, ESearchCase::CaseSensitive));
	}


	static UTransBuffer* TXS_Buffer()
	{
		return IsValid(GEditor) ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
	}

	/** Entries that can still be undone: the queue minus the already-undone tail. */
	static int32 TXS_UndoableCount()
	{
		const UTransBuffer* Buffer = TXS_Buffer();
		return IsValid(Buffer) ? FMath::Max(0, Buffer->GetQueueLength() - Buffer->GetUndoCount()) : 0;
	}

	/** Title of the entry an ordinary undo would target next, or empty when there is none. */
	static FString TXS_UndoHeadTitle()
	{
		const UTransBuffer* Buffer = TXS_Buffer();
		if (!IsValid(Buffer))
		{
			return FString();
		}
		const int32 HeadIndex = Buffer->GetQueueLength() - Buffer->GetUndoCount() - 1;
		if (HeadIndex < 0 || HeadIndex >= Buffer->GetQueueLength())
		{
			return FString();
		}
		const FTransaction* Transaction = Buffer->GetTransaction(HeadIndex);
		return Transaction != nullptr ? Transaction->GetTitle().ToString() : FString();
	}

	/** Capture undoable IDs newest-first before invoking the tool. */
	static TArray<FString> TXS_UndoableIdsMostRecentFirst(int32 MaxEntries)
	{
		TArray<FString> Ids;
		const UTransBuffer* Buffer = TXS_Buffer();
		if (!IsValid(Buffer))
		{
			return Ids;
		}
		const int32 Head = Buffer->GetQueueLength() - Buffer->GetUndoCount() - 1;
		for (int32 Index = Head; Index >= 0 && Ids.Num() < MaxEntries; --Index)
		{
			if (const FTransaction* Transaction = Buffer->GetTransaction(Index))
			{
				Ids.Add(Transaction->GetId().ToString(EGuidFormats::DigitsWithHyphens));
			}
			else
			{
				Ids.Add(FString());
			}
		}
		return Ids;
	}

	/**
	 * Temporarily replace the transactor to exercise absent/empty buffers without resetting user history.
	 * Do not Initialize the replacement, which would register an engine delegate.
	 */
	struct FScopedTransactorSwap
	{
		explicit FScopedTransactorSwap(UTransactor* Replacement)
		{
			if (IsValid(GEditor))
			{
				Saved.Reset(GEditor->Trans);
				GEditor->Trans = Replacement;
				bSwapped = true;
			}
		}

		~FScopedTransactorSwap()
		{
			if (bSwapped && IsValid(GEditor))
			{
				GEditor->Trans = Saved.Get();
				Saved.Reset();
			}
		}

		FScopedTransactorSwap(const FScopedTransactorSwap&) = delete;
		FScopedTransactorSwap& operator=(const FScopedTransactorSwap&) = delete;

	private:
		// Keep the original transactor strongly referenced during the swap.
		TStrongObjectPtr<UTransactor> Saved;
		bool bSwapped = false;
	};


	static IClaireonTool::FToolResult TXS_BeginGroup(const FString& Label)
	{
		ClaireonTool_TransactionBeginGroup Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("label"), Label);
		return Tool.Execute(Args);
	}

	static IClaireonTool::FToolResult TXS_EndGroup()
	{
		ClaireonTool_TransactionEndGroup Tool;
		return Tool.Execute(MakeShared<FJsonObject>());
	}

	/** Rollback without acknowledgement. */
	static IClaireonTool::FToolResult TXS_RollbackDefault()
	{
		ClaireonTool_TransactionRollbackGroup Tool;
		return Tool.Execute(MakeShared<FJsonObject>());
	}

	/** Rollback with unsettled-formatting acknowledgement. */
	static IClaireonTool::FToolResult TXS_RollbackAcknowledged(FAutomationTestBase& Test)
	{
		ClaireonTool_TransactionRollbackGroup Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetBoolField(AcknowledgeUnsettledFormattingArg, true);
		IClaireonTool::FToolResult Result = Tool.Execute(Args);

		FString RefusalReason;
		if (Result.Data.IsValid()
			&& Result.Data->TryGetStringField(TEXT("refusal_reason"), RefusalReason)
			&& RefusalReason.Equals(RefusalReasonUnprovable, ESearchCase::CaseSensitive))
		{
			Test.AddError(FString::Printf(
				TEXT("transaction_rollback_group REFUSED a call that passed '%s', so that argument no "
					 "longer opens a path through the refusal. Cases 5 and 6 are the only shapes that "
					 "depend on it -- they are the only ones that reach a real rollback -- and they "
					 "cannot be satisfied without it. If the argument was deliberately withdrawn, these "
					 "two cases must be re-specified, not quietly re-pointed at the refusal that case 7 "
					 "already covers."),
				AcknowledgeUnsettledFormattingArg));
		}
		return Result;
	}

	static IClaireonTool::FToolResult TXS_Undo(int32 Count)
	{
		ClaireonTool_TransactionUndo Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetNumberField(TEXT("count"), Count);
		return Tool.Execute(Args);
	}

	static IClaireonTool::FToolResult TXS_Redo(int32 Count)
	{
		ClaireonTool_TransactionRedo Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetNumberField(TEXT("count"), Count);
		return Tool.Execute(Args);
	}

	static IClaireonTool::FToolResult TXS_RemoveNode(const FString& AssetPath,
		const FString& GraphName, const FGuid& NodeGuid)
	{
		ClaireonBlueprintGraphTool_RemoveNode Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("graph_name"), GraphName);
		Args->SetStringField(TEXT("node_guid"), NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Args->SetStringField(TEXT("response_mode"), TEXT("status"));
		return Tool.Execute(Args);
	}

	static IClaireonTool::FToolResult TXS_MoveNode(const FString& AssetPath,
		const FString& GraphName, const FGuid& NodeGuid, int32 X, int32 Y)
	{
		ClaireonBlueprintGraphTool_MoveNode Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("graph_name"), GraphName);
		Args->SetStringField(TEXT("node_guid"), NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Args->SetNumberField(TEXT("position_x"), X);
		Args->SetNumberField(TEXT("position_y"), Y);
		Args->SetStringField(TEXT("response_mode"), TEXT("status"));
		return Tool.Execute(Args);
	}

	/** Close any group left open by an early exit so it cannot absorb later editor work. */
	struct FGroupSafetyNet
	{
		explicit FGroupSafetyNet(FAutomationTestBase& InTest) : Test(InTest) {}

		~FGroupSafetyNet()
		{
			if (ClaireonTransactionGroupState::bGroupActive)
			{
				const FString Leaked = ClaireonTransactionGroupState::ActiveGroupLabel;
				TXS_EndGroup();
				Test.AddWarning(FString::Printf(
					TEXT("the test returned with the transaction group '%s' still open; the safety net "
						 "closed it. The group is editor-wide, so leaving it open would sweep later "
						 "tests into it."), *Leaked));
			}
		}

		FGroupSafetyNet(const FGroupSafetyNet&) = delete;
		FGroupSafetyNet& operator=(const FGroupSafetyNet&) = delete;

	private:
		FAutomationTestBase& Test;
	};

	/** Restore the original undo position with bounded engine redo calls; warn if restoration stops early. */
	struct FUndoPositionNet
	{
		explicit FUndoPositionNet(FAutomationTestBase& InTest)
			: Test(InTest)
			, StartUndoCount(IsValid(TXS_Buffer()) ? TXS_Buffer()->GetUndoCount() : 0)
		{
		}

		~FUndoPositionNet()
		{
			UTransBuffer* Buffer = TXS_Buffer();
			if (!IsValid(GEditor) || !IsValid(Buffer))
			{
				return;
			}
			const int32 Observed = Buffer->GetUndoCount();
			for (int32 Remaining = Observed - StartUndoCount; Remaining > 0; --Remaining)
			{
				if (!GEditor->RedoTransaction())
				{
					break;
				}
			}
			const int32 Restored = Buffer->GetUndoCount();
			if (Restored != StartUndoCount)
			{
				Test.AddWarning(FString::Printf(
					TEXT("the test returned with %d transaction(s) undone and the safety net restored "
						 "the position only to %d, not the %d it started at. The buffer is "
						 "editor-wide, so a residual undone tail is inherited by whatever reads it "
						 "next."), Observed, Restored, StartUndoCount));
			}
		}

		FUndoPositionNet(const FUndoPositionNet&) = delete;
		FUndoPositionNet& operator=(const FUndoPositionNet&) = delete;

	private:
		FAutomationTestBase& Test;
		int32 StartUndoCount = 0;
	};


	/** The canonical fixture graph: a custom event whose Then reaches one PrintString call. */
	struct FCanonicalGraph
	{
		FGuid EntryGuid;
		FGuid CallGuid;
	};

	/** Rebuild the canonical two-node graph for pre/post comparisons. */
	static bool TXS_BuildCanonical(UBlueprint& Blueprint, UEdGraph& Graph,
		FCanonicalGraph& Out, FString& OutError)
	{
		TArray<UEdGraphNode*> Existing = Graph.Nodes;
		for (UEdGraphNode* Node : Existing)
		{
			if (!IsValid(Node))
			{
				continue;
			}
			if (Node->GetOuter() == &Graph)
			{
				FBlueprintEditorUtils::RemoveNode(&Blueprint, Node, /*bDontRecompile=*/true);
			}
			else
			{
				Graph.RemoveNode(Node);
			}
		}
		if (Graph.Nodes.Num() != 0)
		{
			OutError = FString::Printf(TEXT("the wipe left %d node(s) in '%s'."),
				Graph.Nodes.Num(), *Graph.GetName());
			return false;
		}

		UFunction* PrintString = UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString"));
		if (PrintString == nullptr)
		{
			OutError = TEXT("UKismetSystemLibrary::PrintString could not be resolved, so the canonical "
				"graph cannot be built. That is an engine-side change, not a fixture problem.");
			return false;
		}

		// Set RF_Transactional explicitly; AddNode does not, and Modify otherwise records no node state or wires.
		UK2Node_CustomEvent* Entry = NewObject<UK2Node_CustomEvent>(&Graph, NAME_None, RF_Transactional);
		Graph.AddNode(Entry, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Entry->CreateNewGuid();
		Entry->CustomFunctionName = TEXT("CanonicalEntry");
		Entry->NodePosX = 0;
		Entry->NodePosY = 0;
		Entry->AllocateDefaultPins();

		// Set the function reference before allocating call pins.
		UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(&Graph, NAME_None, RF_Transactional);
		Graph.AddNode(Call, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Call->CreateNewGuid();
		Call->SetFromFunction(PrintString);
		Call->NodePosX = 400;
		Call->NodePosY = 0;
		Call->AllocateDefaultPins();

		UEdGraphPin* Then = Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		UEdGraphPin* Exec = Call->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		if (Then == nullptr || Exec == nullptr)
		{
			OutError = FString::Printf(TEXT("the canonical graph could not be wired: %s pin missing."),
				Then == nullptr ? TEXT("event Then") : TEXT("call Execute"));
			return false;
		}
		Then->MakeLinkTo(Exec);

		Out.EntryGuid = Entry->NodeGuid;
		Out.CallGuid = Call->NodeGuid;
		return true;
	}

	/**
	 * Separate fixture creation, body, and teardown with stack unwinds.
	 * Rollback tests do not open editors or invoke the test-only BA settlement helper.
	 */
	class FTXS_FixtureCommand : public IAutomationLatentCommand
	{
	public:
		FTXS_FixtureCommand(FAutomationTestBase* InTest, const FString& InAssetPath)
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
					Test->AddError(TEXT("the fixture has no ubergraph to build the canonical graph in."));
					Phase = EPhase::Teardown;
					return false;
				}
				FString BuildError;
				if (!TXS_BuildCanonical(*Blueprint, *Graph, Canonical, BuildError))
				{
					Test->AddError(FString::Printf(TEXT("fixture setup failed: %s"), *BuildError));
					Phase = EPhase::Teardown;
					return false;
				}
				GraphName = Graph->GetName();
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
				{
					// Construct the group guard last so it closes first; an open group blocks redo cleanup.
					FUndoPositionNet UndoNet(*Test);
					FGroupSafetyNet SafetyNet(*Test);
					RunBody(*Blueprint, *Graph);
				}
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
		FString GraphName;
		FCanonicalGraph Canonical;

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

	/** Guard every body: this suite is EditorContext-only and must never run without one. */
	static bool TXS_RequireBuffer(FAutomationTestBase& Test)
	{
		if (!IsValid(GEditor))
		{
			Test.AddError(TEXT("GEditor is null; this suite is EditorContext-only and must not have "
				"been discovered here."));
			return false;
		}
		if (!IsValid(TXS_Buffer()))
		{
			Test.AddError(TEXT("GEditor->Trans is not a UTransBuffer. Every assertion in this file "
				"depends on a real transactor, so this is a hard stop rather than a skip."));
			return false;
		}
		return true;
	}
}

// Undo restores the pre-edit graph.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorUndoRestoresThePreEditState,
	"Claireon.BPEditor.Transactions.UndoRestoresThePreEditState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorTxnShapesInternal
{
	class FTXS_UndoRestoresCommand : public FTXS_FixtureCommand
	{
	public:
		using FTXS_FixtureCommand::FTXS_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph) override
		{
			if (!TXS_RequireBuffer(*Test))
			{
				return;
			}

			const FGraphShape Before = TXS_Capture(&Graph);
			const FNodeFacts CallBefore = TXS_ReadNodeFacts(&Graph, Canonical.CallGuid);
			Test->TestTrue(TEXT("the canonical graph starts with two wired nodes"),
				Before.NodeCount == 2 && Before.LinkCount == 1 && CallBefore.bPresent);

			const IClaireonTool::FToolResult Removed =
				TXS_RemoveNode(AssetPath, GraphName, Canonical.CallGuid);
			if (Removed.bIsError)
			{
				Test->AddError(FString::Printf(TEXT("bp_remove_node failed: %s"), *Removed.ErrorMessage));
				return;
			}

			const FGraphShape AfterEdit = TXS_Capture(&Graph);
			Test->TestTrue(
				FString::Printf(TEXT("the removal changed the graph (observed %s, was %s)"),
					*AfterEdit.Describe(), *Before.Describe()),
				AfterEdit.NodeCount == Before.NodeCount - 1 && AfterEdit.LinkCount == Before.LinkCount - 1);
			Test->TestFalse(TEXT("the removed node is gone from the graph"),
				TXS_ReadNodeFacts(&Graph, Canonical.CallGuid).bPresent);

			const int32 UndoableBefore = TXS_UndoableCount();
			const IClaireonTool::FToolResult Undone = TXS_Undo(1);

			Test->TestFalse(TEXT("a full-count undo is not an error"), Undone.bIsError);
			TXS_ExpectIntField(*Test, Undone.Data, TEXT("requested_count"), 1);
			TXS_ExpectIntField(*Test, Undone.Data, TEXT("undone_count"), 1);
			TArray<FString> Titles;
			TArray<FString> Ids;
			if (TXS_ReadStringArray(*Test, Undone.Data, TEXT("transactions"), Titles))
			{
				Test->TestEqual(TEXT("one title is reported for one reversal"), Titles.Num(), 1);
			}
			if (TXS_ReadStringArray(*Test, Undone.Data, TEXT("undone_transaction_ids"), Ids))
			{
				Test->TestEqual(TEXT("one identity is reported for one reversal"), Ids.Num(), 1);
			}

			Test->TestEqual(TEXT("the transactor advanced by exactly one undo"),
				TXS_UndoableCount(), UndoableBefore - 1);
			TXS_ExpectShape(*Test, TEXT("the graph after the undo"), TXS_Capture(&Graph), Before);
			TXS_ExpectNodeFacts(*Test, TEXT("the restored node"),
				TXS_ReadNodeFacts(&Graph, Canonical.CallGuid), CallBefore);
		}
	};
}

bool FClaireonBPEditorUndoRestoresThePreEditState::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPEditorTxnShapesInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FTXS_UndoRestoresCommand(this,
		ClaireonBPEditorFixtures::UniquePath(TEXT("TxsUndo"))));
	return true;
}

// Redo restores the edit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorRedoReappliesTheUndoneEdit,
	"Claireon.BPEditor.Transactions.RedoReappliesTheUndoneEdit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorTxnShapesInternal
{
	class FTXS_RedoCommand : public FTXS_FixtureCommand
	{
	public:
		using FTXS_FixtureCommand::FTXS_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph) override
		{
			if (!TXS_RequireBuffer(*Test))
			{
				return;
			}

			const FGraphShape Before = TXS_Capture(&Graph);

			if (TXS_RemoveNode(AssetPath, GraphName, Canonical.CallGuid).bIsError)
			{
				Test->AddError(TEXT("bp_remove_node failed, so there is nothing to undo and redo."));
				return;
			}
			const FGraphShape AfterEdit = TXS_Capture(&Graph);

			const IClaireonTool::FToolResult Undone = TXS_Undo(1);
			if (Undone.bIsError)
			{
				Test->AddError(FString::Printf(TEXT("the undo this case builds on failed: %s"),
					*Undone.ErrorMessage));
				return;
			}
			TXS_ExpectShape(*Test, TEXT("the graph after the undo"), TXS_Capture(&Graph), Before);

			const int32 RedoableBefore = IsValid(TXS_Buffer()) ? TXS_Buffer()->GetUndoCount() : 0;
			const IClaireonTool::FToolResult Redone = TXS_Redo(1);

			Test->TestFalse(TEXT("a full-count redo is not an error"), Redone.bIsError);
			TXS_ExpectIntField(*Test, Redone.Data, TEXT("requested_count"), 1);
			TXS_ExpectIntField(*Test, Redone.Data, TEXT("redone_count"), 1);
			TArray<FString> Ids;
			if (TXS_ReadStringArray(*Test, Redone.Data, TEXT("redone_transaction_ids"), Ids))
			{
				Test->TestEqual(TEXT("one identity is reported for one reapplication"), Ids.Num(), 1);
			}

			Test->TestEqual(TEXT("the transactor consumed exactly one redo"),
				IsValid(TXS_Buffer()) ? TXS_Buffer()->GetUndoCount() : -1, RedoableBefore - 1);
			TXS_ExpectShape(*Test, TEXT("the graph after the redo"), TXS_Capture(&Graph), AfterEdit);
			Test->TestFalse(TEXT("the redone removal took the node away again"),
				TXS_ReadNodeFacts(&Graph, Canonical.CallGuid).bPresent);
		}
	};
}

bool FClaireonBPEditorRedoReappliesTheUndoneEdit::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPEditorTxnShapesInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FTXS_RedoCommand(this,
		ClaireonBPEditorFixtures::UniquePath(TEXT("TxsRedo"))));
	return true;
}

// Undo two of three same-title edits and verify transaction identities and graph state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorUndoneCountMatchesWhatWasReversed,
	"Claireon.BPEditor.Transactions.UndoneCountMatchesWhatWasReversed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorTxnShapesInternal
{
	/** Three moves to distinct X positions. Returns false and reports on any failure. */
	static bool TXS_DriveThreeMoves(FAutomationTestBase& Test, const FString& AssetPath,
		const FString& GraphName, const FGuid& NodeGuid, int32& OutStartLength)
	{
		UTransBuffer* Buffer = TXS_Buffer();
		if (!IsValid(Buffer))
		{
			Test.AddError(TEXT("no transaction buffer to measure against."));
			return false;
		}

		// New transactions purge the undone tail; start the window at queue length minus undo count.
		OutStartLength = Buffer->GetQueueLength() - Buffer->GetUndoCount();

		const int32 Positions[3] = { 100, 200, 300 };
		for (int32 Index = 0; Index < 3; ++Index)
		{
			const IClaireonTool::FToolResult Moved =
				TXS_MoveNode(AssetPath, GraphName, NodeGuid, Positions[Index], 0);
			if (Moved.bIsError)
			{
				Test.AddError(FString::Printf(TEXT("bp_move_node #%d failed: %s"),
					Index + 1, *Moved.ErrorMessage));
				return false;
			}
		}

		Buffer = TXS_Buffer();
		if (!IsValid(Buffer))
		{
			Test.AddError(TEXT("the transaction buffer went away mid-sequence."));
			return false;
		}
		const int32 Appended = Buffer->GetQueueLength() - OutStartLength;
		if (Appended != 3 || Buffer->GetUndoCount() != 0)
		{
			Test.AddError(FString::Printf(
				TEXT("three moves were expected to append exactly three undoable transactions; the "
					 "queue grew by %d with %d already undone. The identity cross-check below would "
					 "otherwise compare against the wrong entries, so this is a measurement failure "
					 "rather than a tool defect."),
				Appended, Buffer->GetUndoCount()));
			return false;
		}
		return true;
	}

	class FTXS_UndoneCountCommand : public FTXS_FixtureCommand
	{
	public:
		using FTXS_FixtureCommand::FTXS_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph) override
		{
			if (!TXS_RequireBuffer(*Test))
			{
				return;
			}

			int32 StartLength = 0;
			if (!TXS_DriveThreeMoves(*Test, AssetPath, GraphName, Canonical.CallGuid, StartLength))
			{
				return;
			}

			Test->TestEqual(TEXT("the third move is the one the graph is showing"),
				TXS_ReadNodeFacts(&Graph, Canonical.CallGuid).PosX, 300);

			const TArray<FString> ExpectedIds = TXS_UndoableIdsMostRecentFirst(3);
			const int32 UndoableBefore = TXS_UndoableCount();

			const IClaireonTool::FToolResult Undone = TXS_Undo(2);

			Test->TestFalse(TEXT("a full-count undo is not an error"), Undone.bIsError);
			TXS_ExpectIntField(*Test, Undone.Data, TEXT("requested_count"), 2);
			TXS_ExpectIntField(*Test, Undone.Data, TEXT("undone_count"), 2);

			TArray<FString> ReportedIds;
			if (TXS_ReadStringArray(*Test, Undone.Data, TEXT("undone_transaction_ids"), ReportedIds)
				&& ExpectedIds.Num() >= 2)
			{
				Test->TestEqual(TEXT("two identities are reported for two reversals"), ReportedIds.Num(), 2);
				if (ReportedIds.Num() == 2)
				{
					TXS_ExpectString(*Test, TEXT("the first reversal's identity"),
						ReportedIds[0], ExpectedIds[0]);
					TXS_ExpectString(*Test, TEXT("the second reversal's identity"),
						ReportedIds[1], ExpectedIds[1]);
					Test->TestTrue(TEXT("the two reported identities are distinct"),
						!ReportedIds[0].Equals(ReportedIds[1], ESearchCase::CaseSensitive));
				}
			}

			TArray<FString> Titles;
			if (TXS_ReadStringArray(*Test, Undone.Data, TEXT("transactions"), Titles) && Titles.Num() == 2)
			{
				Test->TestTrue(TEXT("both reversals carry the same title, which is why identities exist"),
					Titles[0].Equals(Titles[1], ESearchCase::CaseSensitive));
			}

			Test->TestEqual(TEXT("the transactor advanced by exactly two undos"),
				TXS_UndoableCount(), UndoableBefore - 2);
			Test->TestEqual(
				TEXT("the graph shows exactly two reversals: the first move's position, not the "
					 "second's and not the third's"),
				TXS_ReadNodeFacts(&Graph, Canonical.CallGuid).PosX, 100);
		}
	};
}

bool FClaireonBPEditorUndoneCountMatchesWhatWasReversed::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPEditorTxnShapesInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FTXS_UndoneCountCommand(this,
		ClaireonBPEditorFixtures::UniquePath(TEXT("TxsCount"))));
	return true;
}

// Acknowledged group rollback restores the pre-edit graph without test-only settlement.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorRollbackGroupRestoresThePreEditState,
	"Claireon.BPEditor.Transactions.RollbackGroupRestoresThePreEditState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorTxnShapesInternal
{
	class FTXS_RollbackSuccessCommand : public FTXS_FixtureCommand
	{
	public:
		using FTXS_FixtureCommand::FTXS_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph) override
		{
			if (!TXS_RequireBuffer(*Test))
			{
				return;
			}

			const FString Label = TEXT("gate7-case5-rollback-success");
			const FString ExpectedTitle = ClaireonTransactionGroupState::MakeGroupTitle(Label);

			const FGraphShape Before = TXS_Capture(&Graph);
			const FNodeFacts CallBefore = TXS_ReadNodeFacts(&Graph, Canonical.CallGuid);

			if (TXS_BeginGroup(Label).bIsError)
			{
				Test->AddError(TEXT("transaction_begin_group failed, so there is no group to roll back."));
				return;
			}
			if (TXS_RemoveNode(AssetPath, GraphName, Canonical.CallGuid).bIsError)
			{
				Test->AddError(TEXT("bp_remove_node failed inside the group."));
				return;
			}
			Test->TestTrue(TEXT("the edit landed inside the group"),
				TXS_Capture(&Graph).NodeCount == Before.NodeCount - 1);

			const IClaireonTool::FToolResult Rolled = TXS_RollbackAcknowledged(*Test);

			Test->TestFalse(TEXT("a rollback that actually rolled back is not an error"), Rolled.bIsError);
			TXS_ExpectBoolField(*Test, Rolled.Data, TEXT("group_closed"), true);
			TXS_ExpectBoolField(*Test, Rolled.Data, TEXT("group_rolled_back"), true);
			TXS_ExpectBoolField(*Test, Rolled.Data, TEXT("safety_acknowledged_by_caller"), true);
			// Acknowledgement accepts unsettled-formatting risk; it does not prove safety.
			TXS_ExpectBoolField(*Test, Rolled.Data, TEXT("rollback_group_safe"), false);
			FString ReportedLabel;
			if (TXS_ReadString(*Test, Rolled.Data, TEXT("label"), ReportedLabel))
			{
				TXS_ExpectString(*Test, TEXT("the reported label"), ReportedLabel, ExpectedTitle);
			}

			bool bUndoAvailable = false;
			if (TXS_ReadBool(*Test, Rolled.Data, TEXT("undo_available"), bUndoAvailable))
			{
				const bool bTransactorAgrees = IsValid(TXS_Buffer()) && TXS_Buffer()->CanUndo();
				Test->TestTrue(
					FString::Printf(TEXT("undo_available (%s) matches the transactor's own CanUndo() (%s)"),
						bUndoAvailable ? TEXT("true") : TEXT("false"),
						bTransactorAgrees ? TEXT("true") : TEXT("false")),
					bUndoAvailable == bTransactorAgrees);
			}

			Test->TestFalse(TEXT("no group remains open after a successful rollback"),
				ClaireonTransactionGroupState::bGroupActive);
			TXS_ExpectShape(*Test, TEXT("the graph after the rollback"), TXS_Capture(&Graph), Before);
			TXS_ExpectNodeFacts(*Test, TEXT("the restored node"),
				TXS_ReadNodeFacts(&Graph, Canonical.CallGuid), CallBefore);
		}
	};
}

bool FClaireonBPEditorRollbackGroupRestoresThePreEditState::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPEditorTxnShapesInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FTXS_RollbackSuccessCommand(this,
		ClaireonBPEditorFixtures::UniquePath(TEXT("TxsRbOk"))));
	return true;
}

// Force rollback undo failure, verify the closed group retains its edit, then execute the hinted retry.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorRollbackGroupReportsAFailedUndo,
	"Claireon.BPEditor.Transactions.RollbackGroupReportsAFailedUndo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorTxnShapesInternal
{
	class FTXS_RollbackFailureCommand : public FTXS_FixtureCommand
	{
	public:
		using FTXS_FixtureCommand::FTXS_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph) override
		{
			if (!TXS_RequireBuffer(*Test))
			{
				return;
			}

			const FString Label = TEXT("gate7-case6-rollback-undo-failure");
			const FString ExpectedTitle = ClaireonTransactionGroupState::MakeGroupTitle(Label);

			const FGraphShape Before = TXS_Capture(&Graph);
			const FNodeFacts CallBefore = TXS_ReadNodeFacts(&Graph, Canonical.CallGuid);

			if (TXS_BeginGroup(Label).bIsError)
			{
				Test->AddError(TEXT("transaction_begin_group failed, so there is no group to roll back."));
				return;
			}
			if (TXS_RemoveNode(AssetPath, GraphName, Canonical.CallGuid).bIsError)
			{
				Test->AddError(TEXT("bp_remove_node failed inside the group."));
				return;
			}
			const FGraphShape AfterEdit = TXS_Capture(&Graph);

			IClaireonTool::FToolResult Rolled;
			{
				// Inject failure into the real undo-failure branch.
				ClaireonBPFaultInjection::FScopedFault Fault(ClaireonTransactionFaultSeam::RollbackGroupUndo);
				Rolled = TXS_RollbackAcknowledged(*Test);
			}

			Test->TestTrue(TEXT("a rollback whose undo failed is an ERROR"), Rolled.bIsError);
			TXS_ExpectBoolField(*Test, Rolled.Data, TEXT("group_closed"), true);
			TXS_ExpectBoolField(*Test, Rolled.Data, TEXT("group_rolled_back"), false);
			FString ReportedLabel;
			if (TXS_ReadString(*Test, Rolled.Data, TEXT("label"), ReportedLabel))
			{
				TXS_ExpectString(*Test, TEXT("the reported label"), ReportedLabel, ExpectedTitle);
			}
			FString FailureReason;
			if (TXS_ReadString(*Test, Rolled.Data, TEXT("undo_failure_reason"), FailureReason))
			{
				TXS_ExpectString(*Test, TEXT("the failure reason"), FailureReason, TEXT("undo_failed"));
			}

			bool bAtHead = false;
			if (TXS_ReadBool(*Test, Rolled.Data, TEXT("transaction_at_undo_head"), bAtHead))
			{
				const FString ObservedHead = TXS_UndoHeadTitle();
				const bool bReallyAtHead = ObservedHead.Equals(ExpectedTitle, ESearchCase::CaseSensitive);
				Test->TestTrue(
					FString::Printf(
						TEXT("transaction_at_undo_head (%s) matches the buffer, whose head is '%s'"),
						bAtHead ? TEXT("true") : TEXT("false"), *ObservedHead),
					bAtHead == bReallyAtHead);
			}
			TXS_ExpectString(*Test, TEXT("the hint names the retry tool"),
				TXS_HintTool(Rolled), TEXT("transaction_undo"));

			Test->TestFalse(TEXT("the group state is cleared even though the undo failed"),
				ClaireonTransactionGroupState::bGroupActive);
			TXS_ExpectShape(*Test, TEXT("the graph after a FAILED rollback still shows the edit"),
				TXS_Capture(&Graph), AfterEdit);

			const IClaireonTool::FToolResult Retry = TXS_Undo(1);
			Test->TestFalse(
				FString::Printf(TEXT("the recommended retry, transaction_undo, succeeds: %s"),
					*Retry.ErrorMessage),
				Retry.bIsError);
			TXS_ExpectShape(*Test, TEXT("the graph after the retry"), TXS_Capture(&Graph), Before);
			TXS_ExpectNodeFacts(*Test, TEXT("the node the retry restored"),
				TXS_ReadNodeFacts(&Graph, Canonical.CallGuid), CallBefore);
		}
	};
}

bool FClaireonBPEditorRollbackGroupReportsAFailedUndo::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPEditorTxnShapesInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FTXS_RollbackFailureCommand(this,
		ClaireonBPEditorFixtures::UniquePath(TEXT("TxsRbFail"))));
	return true;
}

// Ending an empty group removes its record; rollback must not undo the preceding edit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorEmptyGroupRollbackLeavesPriorEditAlone,
	"Claireon.BPEditor.Transactions.EmptyGroupRollbackLeavesPriorEditAlone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorTxnShapesInternal
{
	class FTXS_EmptyGroupRollbackCommand : public FTXS_FixtureCommand
	{
	public:
		using FTXS_FixtureCommand::FTXS_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph) override
		{
			if (!TXS_RequireBuffer(*Test))
			{
				return;
			}

			const FGraphShape Before = TXS_Capture(&Graph);
			if (TXS_RemoveNode(AssetPath, GraphName, Canonical.CallGuid).bIsError)
			{
				Test->AddError(TEXT("the sentinel bp_remove_node failed, so the scenario cannot be built."));
				return;
			}
			const FGraphShape AfterSentinel = TXS_Capture(&Graph);
			Test->TestTrue(TEXT("the sentinel edit landed"),
				AfterSentinel.NodeCount == Before.NodeCount - 1);

			if (TXS_BeginGroup(TEXT("gate7-case5b-empty-group")).bIsError)
			{
				Test->AddError(TEXT("transaction_begin_group failed, so there is no group to roll back."));
				return;
			}

			const IClaireonTool::FToolResult Rolled = TXS_RollbackAcknowledged(*Test);

			Test->TestTrue(TEXT("declining to undo an unrelated head is reported as an error"),
				Rolled.bIsError);
			TXS_ExpectBoolField(*Test, Rolled.Data, TEXT("group_closed"), true);
			TXS_ExpectBoolField(*Test, Rolled.Data, TEXT("group_rolled_back"), false);
			TXS_ExpectBoolField(*Test, Rolled.Data, TEXT("transaction_at_undo_head"), false);
			FString Reason;
			if (TXS_ReadString(*Test, Rolled.Data, TEXT("undo_failure_reason"), Reason))
			{
				TXS_ExpectString(*Test, TEXT("the decline reason"), Reason,
					TEXT("group_not_at_undo_head"));
			}

			TXS_ExpectShape(*Test, TEXT("the graph after the declined rollback"),
				TXS_Capture(&Graph), AfterSentinel);
			Test->TestFalse(TEXT("no group remains open after the declined rollback"),
				ClaireonTransactionGroupState::bGroupActive);
		}
	};
}

bool FClaireonBPEditorEmptyGroupRollbackLeavesPriorEditAlone::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPEditorTxnShapesInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FTXS_EmptyGroupRollbackCommand(this,
		ClaireonBPEditorFixtures::UniquePath(TEXT("TxsRbEmpty"))));
	return true;
}

// Reuse a group title to require transaction-ID matching after an empty group is discarded.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorSameLabelEmptyGroupRollback,
	"Claireon.BPEditor.Transactions.SameLabelEmptyGroupRollbackLeavesPredecessorAlone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorTxnShapesInternal
{
	class FTXS_SameLabelEmptyGroupCommand : public FTXS_FixtureCommand
	{
	public:
		using FTXS_FixtureCommand::FTXS_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph) override
		{
			if (!TXS_RequireBuffer(*Test))
			{
				return;
			}

			const FString Label = TEXT("gate7-case5c-repeated-label");

			const FGraphShape Before = TXS_Capture(&Graph);
			const FNodeFacts CallBefore = TXS_ReadNodeFacts(&Graph, Canonical.CallGuid);
			if (TXS_BeginGroup(Label).bIsError)
			{
				Test->AddError(TEXT("the predecessor group could not be opened."));
				return;
			}
			if (TXS_RemoveNode(AssetPath, GraphName, Canonical.CallGuid).bIsError)
			{
				Test->AddError(TEXT("the predecessor group's edit failed."));
				return;
			}
			TXS_EndGroup();
			const FGraphShape AfterPredecessor = TXS_Capture(&Graph);
			Test->TestTrue(TEXT("the predecessor group's edit landed"),
				AfterPredecessor.NodeCount == Before.NodeCount - 1);

			if (TXS_BeginGroup(Label).bIsError)
			{
				Test->AddError(TEXT("the repeated-label group could not be opened."));
				return;
			}

			const IClaireonTool::FToolResult Rolled = TXS_RollbackAcknowledged(*Test);

			Test->TestTrue(TEXT("the same-label empty rollback declines as an error"),
				Rolled.bIsError);
			TXS_ExpectBoolField(*Test, Rolled.Data, TEXT("group_closed"), true);
			TXS_ExpectBoolField(*Test, Rolled.Data, TEXT("group_rolled_back"), false);
			TXS_ExpectBoolField(*Test, Rolled.Data, TEXT("transaction_at_undo_head"), false);
			FString Reason;
			if (TXS_ReadString(*Test, Rolled.Data, TEXT("undo_failure_reason"), Reason))
			{
				TXS_ExpectString(*Test, TEXT("the decline reason"), Reason,
					TEXT("group_not_at_undo_head"));
			}

			TXS_ExpectShape(*Test, TEXT("the graph after the declined rollback"),
				TXS_Capture(&Graph), AfterPredecessor);

			// One ordinary undo must still reverse the predecessor group.
			const IClaireonTool::FToolResult Undone = TXS_Undo(1);
			Test->TestFalse(FString::Printf(
				TEXT("the predecessor's undo entry is still at the head (%s)"),
				*Undone.ErrorMessage), Undone.bIsError);
			TXS_ExpectShape(*Test, TEXT("the graph after undoing the predecessor"),
				TXS_Capture(&Graph), Before);
			TXS_ExpectNodeFacts(*Test, TEXT("the node the undo restored"),
				TXS_ReadNodeFacts(&Graph, Canonical.CallGuid), CallBefore);
		}
	};
}

bool FClaireonBPEditorSameLabelEmptyGroupRollback::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPEditorTxnShapesInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FTXS_SameLabelEmptyGroupCommand(this,
		ClaireonBPEditorFixtures::UniquePath(TEXT("TxsRbSameLbl"))));
	return true;
}

// Default rollback refusal retains the group because pending BA work cannot be ruled out.
// Absence of a BA transaction is not evidence of settlement.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorRollbackGroupRefusesAndRetainsTheGroup,
	"Claireon.BPEditor.Transactions.RollbackGroupRefusesAndRetainsTheGroup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorTxnShapesInternal
{
	class FTXS_RollbackRefusalCommand : public FTXS_FixtureCommand
	{
	public:
		using FTXS_FixtureCommand::FTXS_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph) override
		{
			if (!TXS_RequireBuffer(*Test))
			{
				return;
			}

			{
				const IClaireonTool::FToolResult NoGroup = TXS_RollbackDefault();
				Test->TestTrue(TEXT("rolling back with no group open is an error"), NoGroup.bIsError);
			}

			const FString Label = TEXT("gate7-case7-default-refusal");
			const FString ExpectedTitle = ClaireonTransactionGroupState::MakeGroupTitle(Label);

			if (TXS_BeginGroup(Label).bIsError)
			{
				Test->AddError(TEXT("transaction_begin_group failed, so there is no group to refuse over."));
				return;
			}
			if (TXS_RemoveNode(AssetPath, GraphName, Canonical.CallGuid).bIsError)
			{
				Test->AddError(TEXT("bp_remove_node failed inside the group."));
				return;
			}
			const FGraphShape AfterEdit = TXS_Capture(&Graph);

			const IClaireonTool::FToolResult Refused = TXS_RollbackDefault();

			Test->TestTrue(TEXT("the default rollback REFUSES"), Refused.bIsError);
			TXS_ExpectBoolField(*Test, Refused.Data, TEXT("group_closed"), false);
			TXS_ExpectBoolField(*Test, Refused.Data, TEXT("group_rolled_back"), false);
			TXS_ExpectBoolField(*Test, Refused.Data, TEXT("group_retained"), true);
			TXS_ExpectBoolField(*Test, Refused.Data, TEXT("rollback_group_safe"), false);
			FString RefusalReason;
			if (TXS_ReadString(*Test, Refused.Data, TEXT("refusal_reason"), RefusalReason))
			{
				TXS_ExpectString(*Test, TEXT("the refusal reason"),
					RefusalReason, RefusalReasonUnprovable);
			}
			FString ReportedLabel;
			if (TXS_ReadString(*Test, Refused.Data, TEXT("label"), ReportedLabel))
			{
				TXS_ExpectString(*Test, TEXT("the reported label"), ReportedLabel, ExpectedTitle);
			}
			TXS_ExpectString(*Test, TEXT("the hint names the safe alternative"),
				TXS_HintTool(Refused), TEXT("transaction_end_group"));

			const TSharedPtr<FJsonObject>* Evidence = nullptr;
			if (Refused.Data.IsValid()
				&& Refused.Data->TryGetObjectField(TEXT("refusal_evidence"), Evidence)
				&& Evidence != nullptr && Evidence->IsValid())
			{
				FString NotEvidence;
				const bool bHasDisclaimer = (*Evidence)->TryGetStringField(TEXT("not_evidence"), NotEvidence)
					&& !NotEvidence.IsEmpty();
				Test->TestTrue(
					TEXT("the refusal disclaims the absent 'Format Node Added' transaction as evidence"),
					bHasDisclaimer);
				FString Detection;
				FString Population;
				Test->TestTrue(
					TEXT("the refusal cites the two properties of the code it actually rests on: "
						 "undetectable pending formatting, and a group with no graph population"),
					(*Evidence)->TryGetStringField(TEXT("detection"), Detection) && !Detection.IsEmpty()
						&& (*Evidence)->TryGetStringField(TEXT("population"), Population) && !Population.IsEmpty());
			}
			else
			{
				Test->AddError(TEXT("the refusal carried no refusal_evidence object, so it offers no "
					"account of what it is and is not reasoning from."));
			}

			// Check CanUndo, not queue length: Begin adds an in-flight record before undo is available.
			bool bUndoAvailable = true;
			if (TXS_ReadBool(*Test, Refused.Data, TEXT("undo_available"), bUndoAvailable))
			{
				const bool bTransactorAgrees = IsValid(TXS_Buffer()) && TXS_Buffer()->CanUndo();
				Test->TestTrue(
					FString::Printf(
						TEXT("undo_available (%s) matches CanUndo() (%s) while the group is open, even "
							 "though the queue is not empty (%d undoable by length)"),
						bUndoAvailable ? TEXT("true") : TEXT("false"),
						bTransactorAgrees ? TEXT("true") : TEXT("false"), TXS_UndoableCount()),
					bUndoAvailable == bTransactorAgrees);
			}

			Test->TestTrue(TEXT("the group is still open in Claireon's own state"),
				ClaireonTransactionGroupState::bGroupActive);
			TXS_ExpectString(*Test, TEXT("the retained group keeps its label"),
				ClaireonTransactionGroupState::ActiveGroupLabel, Label);
			Test->TestFalse(TEXT("the transactor still has the group's transaction in flight"),
				IsValid(TXS_Buffer()) && TXS_Buffer()->CanUndo());
			TXS_ExpectShape(*Test, TEXT("nothing was undone by the refusal"),
				TXS_Capture(&Graph), AfterEdit);

			// Execute the suggested close and verify a second close refuses.
			const IClaireonTool::FToolResult Ended = TXS_EndGroup();
			Test->TestFalse(
				FString::Printf(TEXT("the retained group can still be closed: %s"), *Ended.ErrorMessage),
				Ended.bIsError);
			Test->TestFalse(TEXT("no group remains after the close"),
				ClaireonTransactionGroupState::bGroupActive);
			Test->TestTrue(TEXT("a second close refuses rather than closing something else"),
				TXS_EndGroup().bIsError);
		}
	};
}

bool FClaireonBPEditorRollbackGroupRefusesAndRetainsTheGroup::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPEditorTxnShapesInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FTXS_RollbackRefusalCommand(this,
		ClaireonBPEditorFixtures::UniquePath(TEXT("TxsRbRefuse"))));
	return true;
}

// Zero-progress undo is an error with a reason matching actual transactor state.
// Use a temporary swap for absent and empty buffers.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorUndoWithNoProgressIsAnError,
	"Claireon.BPEditor.Transactions.UndoWithNoProgressIsAnError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorTxnShapesInternal
{
	/** Facts every zero-progress result must carry, whatever the reason. */
	static void TXS_ExpectZeroProgress(FAutomationTestBase& Test,
		const IClaireonTool::FToolResult& Result, const TCHAR* What,
		const TCHAR* CountField, const TCHAR* ReasonField, const FString& ExpectedReason)
	{
		Test.TestTrue(FString::Printf(TEXT("%s: zero progress is an error"), What), Result.bIsError);
		TXS_ExpectIntField(Test, Result.Data, TEXT("requested_count"), 1);
		TXS_ExpectIntField(Test, Result.Data, CountField, 0);
		TXS_ExpectIntField(Test, Result.Data, TEXT("failed_attempt_index"), 0);
		FString Reason;
		if (TXS_ReadString(Test, Result.Data, ReasonField, Reason))
		{
			TXS_ExpectString(Test, What, Reason, ExpectedReason);
		}
		TArray<FString> Titles;
		if (TXS_ReadStringArray(Test, Result.Data, TEXT("transactions"), Titles))
		{
			Test.TestEqual(FString::Printf(TEXT("%s: nothing is listed as reversed"), What),
				Titles.Num(), 0);
		}
		Test.TestTrue(FString::Printf(TEXT("%s: no success summary is emitted"), What),
			Result.Summary.IsEmpty());
	}

	class FTXS_ZeroProgressCommand : public FTXS_FixtureCommand
	{
	public:
		using FTXS_FixtureCommand::FTXS_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& /*Blueprint*/, UEdGraph& /*Graph*/) override
		{
			if (!TXS_RequireBuffer(*Test))
			{
				return;
			}

			// An open group blocks undo despite a nonempty queue.
			{
				if (TXS_BeginGroup(TEXT("gate7-case8-blocker")).bIsError)
				{
					Test->AddError(TEXT("transaction_begin_group failed, so the blocked-transactor "
						"shape cannot be reached."));
					return;
				}

				const int32 UndoableByLength = TXS_UndoableCount();
				const IClaireonTool::FToolResult Blocked = TXS_Undo(1);
				TXS_ExpectZeroProgress(*Test, Blocked, TEXT("blocked transactor"),
					TEXT("undone_count"), TEXT("undo_failure_reason"), TEXT("undo_failed"));

				Test->TestTrue(
					FString::Printf(
						TEXT("undo_failed rather than empty_buffer is the truthful reason: the queue "
							 "still holds %d undoable entr(y/ies) by length"), UndoableByLength),
					UndoableByLength > 0);
				FString TransactorMessage;
				Test->TestTrue(
					TEXT("the transactor's own words are carried verbatim rather than flattened into "
						 "the enum"),
					Blocked.Data.IsValid()
						&& Blocked.Data->TryGetStringField(TEXT("transactor_message"), TransactorMessage)
						&& !TransactorMessage.IsEmpty());

				// Beginning a group purges redo history, so this case must report empty_redo_stack.
				const int32 RedoableByCount = IsValid(TXS_Buffer()) ? TXS_Buffer()->GetUndoCount() : -1;
				const IClaireonTool::FToolResult BlockedRedo = TXS_Redo(1);
				TXS_ExpectZeroProgress(*Test, BlockedRedo, TEXT("blocked transactor, redo"),
					TEXT("redone_count"), TEXT("redo_failure_reason"), TEXT("empty_redo_stack"));
				Test->TestEqual(
					TEXT("empty_redo_stack is the truthful reason here: opening the group purged the "
						 "undone region, so there is genuinely nothing to reapply"),
					RedoableByCount, 0);

				const IClaireonTool::FToolResult Ended = TXS_EndGroup();
				if (Ended.bIsError)
				{
					Test->AddError(TEXT("the blocker group could not be closed."));
					return;
				}
			}

			// ---- empty_buffer / empty_redo_stack: a transactor with nothing in it --------
			{
				UTransBuffer* Empty = NewObject<UTransBuffer>();
				FScopedTransactorSwap Swap(Empty);

				Test->TestEqual(TEXT("the substituted transactor really is empty"),
					IsValid(TXS_Buffer()) ? TXS_Buffer()->GetQueueLength() : -1, 0);

				TXS_ExpectZeroProgress(*Test, TXS_Undo(1), TEXT("empty buffer"),
					TEXT("undone_count"), TEXT("undo_failure_reason"), TEXT("empty_buffer"));
				TXS_ExpectZeroProgress(*Test, TXS_Redo(1), TEXT("empty redo stack"),
					TEXT("redone_count"), TEXT("redo_failure_reason"), TEXT("empty_redo_stack"));
			}

			// ---- no_transaction_buffer: no transactor at all ----------------------------
			{
				FScopedTransactorSwap Swap(nullptr);

				Test->TestFalse(TEXT("there really is no transactor for this shape"),
					IsValid(GEditor) && IsValid(GEditor->Trans));

				TXS_ExpectZeroProgress(*Test, TXS_Undo(1), TEXT("no transaction buffer"),
					TEXT("undone_count"), TEXT("undo_failure_reason"), TEXT("no_transaction_buffer"));
				TXS_ExpectZeroProgress(*Test, TXS_Redo(1), TEXT("no transaction buffer, redo"),
					TEXT("redone_count"), TEXT("redo_failure_reason"), TEXT("no_transaction_buffer"));
			}

			Test->TestTrue(TEXT("the editor's own transactor is back in place"),
				IsValid(TXS_Buffer()));
		}
	};
}

bool FClaireonBPEditorUndoWithNoProgressIsAnError::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPEditorTxnShapesInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FTXS_ZeroProgressCommand(this,
		ClaireonBPEditorFixtures::UniquePath(TEXT("TxsZero"))));
	return true;
}

// Fail the second of three requested undos; report the one reversed transaction and retained graph state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorUndoWithPartialProgressIsAnError,
	"Claireon.BPEditor.Transactions.UndoWithPartialProgressIsAnError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorTxnShapesInternal
{
	class FTXS_PartialProgressCommand : public FTXS_FixtureCommand
	{
	public:
		using FTXS_FixtureCommand::FTXS_FixtureCommand;

	protected:
		virtual void RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph) override
		{
			if (!TXS_RequireBuffer(*Test))
			{
				return;
			}

			int32 StartLength = 0;
			if (!TXS_DriveThreeMoves(*Test, AssetPath, GraphName, Canonical.CallGuid, StartLength))
			{
				return;
			}

			const TArray<FString> ExpectedIds = TXS_UndoableIdsMostRecentFirst(3);
			const int32 UndoableBefore = TXS_UndoableCount();

			IClaireonTool::FToolResult Partial;
			{
				// Fail only the second attempt.
				ClaireonBPFaultInjection::FScopedFault Fault(ClaireonTransactionFaultSeam::UndoAttempt(1));
				Partial = TXS_Undo(3);
			}

			Test->TestTrue(TEXT("partial progress is an ERROR, not a success with a short count"),
				Partial.bIsError);
			TXS_ExpectIntField(*Test, Partial.Data, TEXT("requested_count"), 3);
			TXS_ExpectIntField(*Test, Partial.Data, TEXT("undone_count"), 1);
			TXS_ExpectIntField(*Test, Partial.Data, TEXT("failed_attempt_index"), 1);
			FString Reason;
			if (TXS_ReadString(*Test, Partial.Data, TEXT("undo_failure_reason"), Reason))
			{
				TXS_ExpectString(*Test, TEXT("the failure reason"), Reason, TEXT("undo_failed"));
			}
			Test->TestTrue(TEXT("no success summary is emitted for a partial undo"),
				Partial.Summary.IsEmpty());

			TArray<FString> Titles;
			if (TXS_ReadStringArray(*Test, Partial.Data, TEXT("transactions"), Titles))
			{
				Test->TestEqual(TEXT("exactly one reversal is listed"), Titles.Num(), 1);
			}
			TArray<FString> ReportedIds;
			if (TXS_ReadStringArray(*Test, Partial.Data, TEXT("undone_transaction_ids"), ReportedIds)
				&& ExpectedIds.Num() >= 1)
			{
				Test->TestEqual(TEXT("exactly one identity is listed"), ReportedIds.Num(), 1);
				if (ReportedIds.Num() == 1)
				{
					TXS_ExpectString(*Test,
						TEXT("the identity reported is the transaction that really was reversed"),
						ReportedIds[0], ExpectedIds[0]);
				}
			}

			Test->TestEqual(TEXT("the transactor advanced by exactly one undo"),
				TXS_UndoableCount(), UndoableBefore - 1);
			Test->TestEqual(
				TEXT("the graph shows exactly one reversal: the second move's position"),
				TXS_ReadNodeFacts(&Graph, Canonical.CallGuid).PosX, 200);
		}
	};
}

bool FClaireonBPEditorUndoWithPartialProgressIsAnError::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPEditorTxnShapesInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FTXS_PartialProgressCommand(this,
		ClaireonBPEditorFixtures::UniquePath(TEXT("TxsPartial"))));
	return true;
}

#endif // WITH_CLAIREON_TESTS && WITH_EDITOR
