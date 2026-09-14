// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Verify extraction through gateway pins, preserved value links, and PrunedExecInUse diagnostics.
// This is structural evidence, not runtime Blueprint evaluation; warning suppression can
// hide the diagnostic, so gateway pin checks remain necessary.
// Derive selections from each clone because duplication regenerates GUIDs.
// Separate fixture creation, editor opening, and teardown across latent updates.

#include "Misc/AutomationTest.h"

#if WITH_CLAIREON_TESTS && WITH_EDITOR

#include "Tests/ClaireonBPEditorFixtures.h"
#include "Tools/ClaireonBPMutationResult.h"
#include "Tools/ClaireonBlueprintGraphTool_Extract.h"
#include "Tools/ClaireonTool_BlueprintDuplicate.h"
#include "Tools/ClaireonTool_Lint.h"
#include "Tools/IClaireonTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetArrayLibrary.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonGraphIslands.h"
#include "ClaireonSessionManager.h"
#include "HAL/PlatformMisc.h"
#include "K2Node_EditablePinBase.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/Script.h"
#include "UObject/SoftObjectPath.h"

namespace ClaireonBPExtractSemanticsInternal
{
	// Prefix helpers to avoid unity-build collisions.

	/**
	 * Read the optional source object path from CLAIREON_TEST_BLUEPRINT.
	 * Tests modify only unique clones under /Game/_ClaireonBPEditor/.
	 */
	static FString EXS_SourceAsset()
	{
		return FPlatformMisc::GetEnvironmentVariable(TEXT("CLAIREON_TEST_BLUEPRINT"));
	}

	/** Optional source function graph from CLAIREON_TEST_BLUEPRINT_FUNCTION. */
	static FString EXS_WarpGraphName()
	{
		return FPlatformMisc::GetEnvironmentVariable(TEXT("CLAIREON_TEST_BLUEPRINT_FUNCTION"));
	}

	/** Missing project content produces a warning-carrying pass because RunTest has no skipped state. */
	static bool EXS_RealAssetConfigured(FAutomationTestBase& Test, bool bNeedsFunctionGraph)
	{
		if (EXS_SourceAsset().IsEmpty())
		{
			Test.AddWarning(TEXT("skipped: set CLAIREON_TEST_BLUEPRINT to the object path of a "
				"real project Blueprint to run the real-asset extraction tests."));
			return false;
		}
		if (bNeedsFunctionGraph && EXS_WarpGraphName().IsEmpty())
		{
			Test.AddWarning(TEXT("skipped: set CLAIREON_TEST_BLUEPRINT_FUNCTION to a function "
				"graph of CLAIREON_TEST_BLUEPRINT whose island reads enclosing-graph locals."));
			return false;
		}
		return true;
	}

	/** The engine's own pruning diagnostic, by FName identity rather than by wording. */
	static const TCHAR* EXS_PrunedExecInUse() { return TEXT("PrunedExecInUse"); }

	/** The FScopedTransaction title ApplyExtract opens on the function path. */
	static const TCHAR* EXS_FunctionTransactionTitle() { return TEXT("Extract function"); }

	// Envelope readers reject absent fields.

	static bool EXS_ReadString(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field, FString& Out)
	{
		if (!Data.IsValid() || !Data->TryGetStringField(Field, Out))
		{
			Test.AddError(FString::Printf(TEXT("the envelope omitted the string field '%s'."), Field));
			return false;
		}
		return true;
	}

	static void EXS_ExpectString(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field, const TCHAR* Expected, const TCHAR* What)
	{
		FString Observed;
		if (EXS_ReadString(Test, Data, Field, Observed))
		{
			Test.TestEqual(FString::Printf(TEXT("%s: %s"), What, Field), Observed, FString(Expected));
		}
	}

	static void EXS_ExpectBool(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field, bool bExpected, const TCHAR* What)
	{
		bool bObserved = !bExpected;
		if (!Data.IsValid() || !Data->TryGetBoolField(Field, bObserved))
		{
			Test.AddError(FString::Printf(TEXT("%s: the envelope omitted the bool field '%s'."),
				What, Field));
			return;
		}
		Test.TestTrue(FString::Printf(TEXT("%s: %s is %s"), What, Field,
			bExpected ? TEXT("true") : TEXT("false")), bObserved == bExpected);
	}

	static void EXS_ExpectAbsent(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		const TCHAR* Field, const TCHAR* Why)
	{
		if (Data.IsValid() && Data->HasField(Field))
		{
			Test.AddError(FString::Printf(TEXT("'%s' is present, and it must not be: %s"), Field, Why));
		}
	}

	static int32 EXS_ReadInt(const TSharedPtr<FJsonObject>& Data, const TCHAR* Field, int32 Fallback)
	{
		double Value = 0.0;
		if (Data.IsValid() && Data->TryGetNumberField(Field, Value))
		{
			return static_cast<int32>(Value);
		}
		return Fallback;
	}

	/** True when any returned diagnostic carries this FName identity. */
	static bool EXS_HasDiagnosticIdentity(const TSharedPtr<FJsonObject>& Data, const TCHAR* Identity,
		FString& OutMessage)
	{
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(TEXT("compiler_diagnostics"), Entries)
			|| Entries == nullptr)
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
			FString Identifier;
			(*Entry)->TryGetStringField(TEXT("identifier"), Identifier);
			if (Identifier.Equals(Identity))
			{
				(*Entry)->TryGetStringField(TEXT("message"), OutMessage);
				return true;
			}
		}
		return false;
	}

	// Observe transaction titles; undo purging and transient removal make queue lengths insufficient.

	struct FEXSTxnWindow
	{
		int32 StartLength = INDEX_NONE;
		int32 StartUndoCount = INDEX_NONE;
		bool bValid = false;
	};

	static UTransBuffer* EXS_Buffer()
	{
		return IsValid(GEditor) ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
	}

	static FEXSTxnWindow EXS_OpenWindow(FAutomationTestBase& Test)
	{
		FEXSTxnWindow Window;
		UTransBuffer* Buffer = EXS_Buffer();
		if (!IsValid(Buffer))
		{
			Test.AddError(TEXT("GEditor->Trans is not a UTransBuffer, so 'no transaction was opened' "
				"cannot be checked against the editor. This suite is EditorContext-only precisely "
				"so that cannot happen."));
			return Window;
		}
		Window.StartLength = Buffer->GetQueueLength();
		Window.StartUndoCount = Buffer->GetUndoCount();
		Window.bValid = true;
		return Window;
	}

	/** Report an unreadable window through bOutMeasured, not as an absent transaction. */
	static bool EXS_WindowHasTitle(const FEXSTxnWindow& Window, const FString& Title,
		bool& bOutMeasured)
	{
		bOutMeasured = false;
		UTransBuffer* Buffer = EXS_Buffer();
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

	/** Verify the refusal opened no transaction. */
	static void EXS_ExpectNoTransactionOpened(FAutomationTestBase& Test, const FEXSTxnWindow& Window,
		const TCHAR* What)
	{
		bool bMeasured = false;
		const bool bPresent = EXS_WindowHasTitle(Window, EXS_FunctionTransactionTitle(), bMeasured);
		if (!bMeasured)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: the transaction window could not be read, so DEC-33's 'no transaction was "
				     "opened' could not be corroborated. Reported as a measurement failure rather "
				     "than as agreement."), What));
			return;
		}
		Test.TestFalse(FString::Printf(
			TEXT("%s: no '%s' record reached the editor's transaction buffer -- the refusal runs "
			     "before the first mutating call, so quiescence is proven BY CONSTRUCTION"),
			What, EXS_FunctionTransactionTitle()), bPresent);
	}


	static IClaireonTool::FToolResult EXS_ExtractFunction(const FString& AssetPath,
		const FString& GraphName, const TArray<FGuid>& NodeGuids, const FString& NewName)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("graph_name"), GraphName);
		Args->SetStringField(TEXT("response_mode"), TEXT("status"));
		if (!NewName.IsEmpty())
		{
			Args->SetStringField(TEXT("new_name"), NewName);
		}

		TArray<TSharedPtr<FJsonValue>> Guids;
		for (const FGuid& Guid : NodeGuids)
		{
			Guids.Add(MakeShared<FJsonValueString>(Guid.ToString(EGuidFormats::DigitsWithHyphens)));
		}
		Args->SetArrayField(TEXT("node_guids"), Guids);

		ClaireonBlueprintGraphTool_ExtractFunction Tool;
		return Tool.Execute(Args);
	}

	static bool EXS_Duplicate(const FString& SourcePath, const FString& DestPath, FString& OutError)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("source_path"), SourcePath);
		Args->SetStringField(TEXT("dest_path"), DestPath);
		ClaireonTool_BlueprintDuplicate Tool;
		const IClaireonTool::FToolResult Result = Tool.Execute(Args);
		if (Result.bIsError)
		{
			OutError = Result.ErrorMessage;
			return false;
		}
		return true;
	}

	/** One `pure-data-island` finding, reduced to what a selection needs. */
	struct FEXSIsland
	{
		TArray<FGuid> Members;
		FGuid Consumer;
		FString ConsumerTitle;
	};

	/** Derive selections from lint on each clone because duplication regenerates node GUIDs. */
	static bool EXS_LintIslands(FAutomationTestBase& Test, const FString& AssetPath,
		const FString& GraphName, TArray<FEXSIsland>& OutIslands)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("graph_name"), GraphName);
		Args->SetStringField(TEXT("rules"), TEXT("pure-data-island"));

		ClaireonTool_Lint Tool;
		const IClaireonTool::FToolResult Result = Tool.Execute(Args);
		if (Result.bIsError || !Result.Data.IsValid())
		{
			Test.AddError(FString::Printf(TEXT("bp_lint failed on '%s' of %s: %s"),
				*GraphName, *AssetPath, *Result.ErrorMessage));
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Findings = nullptr;
		if (!Result.Data->TryGetArrayField(TEXT("findings"), Findings) || Findings == nullptr)
		{
			Test.AddError(TEXT("bp_lint returned no findings array at all."));
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Findings)
		{
			const TSharedPtr<FJsonObject>* Finding = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Finding) || Finding == nullptr)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* Evidence = nullptr;
			if (!(*Finding)->TryGetObjectField(TEXT("evidence"), Evidence) || Evidence == nullptr)
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* Members = nullptr;
			if (!(*Evidence)->TryGetArrayField(TEXT("members"), Members) || Members == nullptr)
			{
				continue;
			}

			FEXSIsland Island;
			for (const TSharedPtr<FJsonValue>& MemberValue : *Members)
			{
				FString Text;
				FGuid Parsed;
				if (MemberValue.IsValid() && MemberValue->TryGetString(Text)
					&& FGuid::Parse(Text, Parsed))
				{
					Island.Members.Add(Parsed);
				}
			}
			FString ConsumerText;
			(*Evidence)->TryGetStringField(TEXT("consumer"), ConsumerText);
			FGuid::Parse(ConsumerText, Island.Consumer);
			(*Evidence)->TryGetStringField(TEXT("consumer_title"), Island.ConsumerTitle);
			OutIslands.Add(MoveTemp(Island));
		}

		OutIslands.Sort([](const FEXSIsland& A, const FEXSIsland& B)
		{
			return A.Members.Num() > B.Members.Num();
		});
		return true;
	}


	static UEdGraph* EXS_FindGraph(UBlueprint* Blueprint, const FString& GraphName)
	{
		if (!IsValid(Blueprint))
		{
			return nullptr;
		}
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		for (UEdGraph* Graph : Graphs)
		{
			if (IsValid(Graph) && Graph->GetName().Equals(GraphName))
			{
				return Graph;
			}
		}
		return nullptr;
	}

	static UEdGraphNode* EXS_FindNode(const UEdGraph* Graph, const FGuid& Guid)
	{
		if (!IsValid(Graph))
		{
			return nullptr;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (IsValid(Node) && Node->NodeGuid == Guid)
			{
				return Node;
			}
		}
		return nullptr;
	}

	/** Inspect all gateway exec pins, including orphans. Function entries always have Then pins, even when pure. */
	static TArray<FString> EXS_ExecPinNames(const UEdGraphNode* Node)
	{
		TArray<FString> Names;
		if (!IsValid(Node))
		{
			return Names;
		}
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin != nullptr && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				Names.Add(FString::Printf(TEXT("%s%s"), *Pin->PinName.ToString(),
					Pin->bOrphanedPin ? TEXT(" (orphaned)") : TEXT("")));
			}
		}
		Names.Sort();
		return Names;
	}

	static FString EXS_Join(const TArray<FString>& Values)
	{
		return Values.Num() == 0 ? FString(TEXT("none")) : FString::Join(Values, TEXT(", "));
	}

	/** True when any output pin of Gateway links to any input pin of Consumer. */
	static bool EXS_GatewayFeedsConsumer(const UEdGraphNode* Gateway, const UEdGraphNode* Consumer,
		FString& OutEdge)
	{
		if (!IsValid(Gateway) || !IsValid(Consumer))
		{
			return false;
		}
		for (const UEdGraphPin* Pin : Gateway->Pins)
		{
			if (Pin == nullptr || Pin->Direction != EGPD_Output)
			{
				continue;
			}
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (Linked != nullptr && Linked->GetOwningNodeUnchecked() == Consumer)
				{
					OutEdge = FString::Printf(TEXT("%s -> %s"),
						*Pin->PinName.ToString(), *Linked->PinName.ToString());
					return true;
				}
			}
		}
		return false;
	}

	static UK2Node_FunctionEntry* EXS_FindEntry(const UEdGraph* Graph)
	{
		if (!IsValid(Graph))
		{
			return nullptr;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node); IsValid(Entry))
			{
				return Entry;
			}
		}
		return nullptr;
	}


	class FEXS_FixtureCommand : public IAutomationLatentCommand
	{
	public:
		FEXS_FixtureCommand(FAutomationTestBase* InTest, const FString& InAssetPath)
			: Test(InTest)
			, AssetPath(InAssetPath)
		{
		}

		virtual bool Update() override
		{
			switch (Phase)
			{
			case EPhase::Build:
			{
				FString BuildError;
				if (!BuildFixture(BuildError))
				{
					Test->AddError(FString::Printf(TEXT("fixture setup failed at %s: %s"),
						*AssetPath, *BuildError));
					Phase = EPhase::Teardown;
					return false;
				}
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
				// Resolve the resident fixture without loading; opening the editor in the same stack as loading risks reentry.
				UBlueprint* Blueprint = Cast<UBlueprint>(ClaireonBPEditorFixtures::Resolve(AssetPath));
				if (!IsValid(Blueprint))
				{
					Test->AddError(TEXT("the fixture is not resident, so the body cannot run "
						"without a load it is not allowed to perform here."));
					Phase = EPhase::Teardown;
					return false;
				}
				RunBody(*Blueprint);

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
		/** Produce the asset at AssetPath. Runs before any editor is opened. */
		virtual bool BuildFixture(FString& OutError) = 0;

		/** The measurement. Runs with the fixture resident and no create/load on the stack. */
		virtual void RunBody(UBlueprint& Blueprint) = 0;

		FAutomationTestBase* Test = nullptr;
		FString AssetPath;

	private:
		enum class EPhase : uint8
		{
			Build,
			Settle,
			Body,
			Teardown,
		};

		EPhase Phase = EPhase::Build;
		int32 SettleUpdates = 0;
	};

	/** Clone the source without modifying it. */
	class FEXS_CloneCommand : public FEXS_FixtureCommand
	{
	public:
		using FEXS_FixtureCommand::FEXS_FixtureCommand;

	protected:
		virtual bool BuildFixture(FString& OutError) override
		{
			return EXS_Duplicate(EXS_SourceAsset(), AssetPath, OutError);
		}
	};
}

// Extract a lint-selected pure-data island and verify its gateway remains pure and connected.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorPureIslandExtractsPure,
	"Claireon.BPEditor.ExtractionSemantics.PureIslandOnTheRealAssetExtractsPure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPExtractSemanticsInternal
{
	class FEXS_PureIslandCommand : public FEXS_CloneCommand
	{
	public:
		using FEXS_CloneCommand::FEXS_CloneCommand;

	protected:
		virtual void RunBody(UBlueprint& Blueprint) override
		{
			UEdGraph* Graph = EXS_FindGraph(&Blueprint, TEXT("EventGraph"));
			if (!IsValid(Graph))
			{
				Test->AddError(TEXT("the clone has no EventGraph, so item 2's measured island "
					"cannot be located."));
				return;
			}

			TArray<FEXSIsland> Islands;
			if (!EXS_LintIslands(*Test, AssetPath, Graph->GetName(), Islands))
			{
				return;
			}
			if (Islands.Num() == 0)
			{
				Test->AddError(TEXT("bp_lint reported no pure-data-island in the clone's "
					"EventGraph. The rule that RECOMMENDS this extraction is the same rule item 2 "
					"exists for, so with no finding there is nothing to prove -- a failure, not a "
					"skip."));
				return;
			}

			const FEXSIsland& Island = Islands[0];
			Test->TestTrue(FString::Printf(
				TEXT("the island has more than one member (%d), so the extraction is not trivial"),
				Island.Members.Num()), Island.Members.Num() > 1);

			TSet<UEdGraphNode*> Selection;
			for (const FGuid& Guid : Island.Members)
			{
				if (UEdGraphNode* Node = EXS_FindNode(Graph, Guid); IsValid(Node))
				{
					Selection.Add(Node);
				}
			}
			if (Selection.Num() != Island.Members.Num())
			{
				Test->AddError(FString::Printf(
					TEXT("only %d of the island's %d member guids resolved in the EventGraph."),
					Selection.Num(), Island.Members.Num()));
				return;
			}

			FString WhyNot;
			if (!ClaireonExtractSemantics::IsSelectionExecFree(Selection, WhyNot))
			{
				Test->AddError(FString::Printf(
					TEXT("the lint island is not exec-free (%s), so DEC-1's gate does not apply to "
					     "it and this test would prove nothing."), *WhyNot));
				return;
			}

			UEdGraphNode* Consumer = EXS_FindNode(Graph, Island.Consumer);
			if (!IsValid(Consumer))
			{
				Test->AddError(TEXT("the island's external consumer did not resolve, so 'the "
					"consumer reads the computed value' cannot be checked."));
				return;
			}

			const FEXSTxnWindow Window = EXS_OpenWindow(*Test);
			const FString NewName = TEXT("EXS_ComputedIslandValue");
			const IClaireonTool::FToolResult Result =
				EXS_ExtractFunction(AssetPath, Graph->GetName(), Island.Members, NewName);

			if (Result.bIsError || !Result.Data.IsValid())
			{
				Test->AddError(FString::Printf(TEXT("the extraction failed: %s"), *Result.ErrorMessage));
				return;
			}

			EXS_ExpectString(*Test, Result.Data, TEXT("mutation_state"),
				TEXT("applied_clean"), TEXT("happy path"));
			EXS_ExpectString(*Test, Result.Data, TEXT("extract_kind"),
				TEXT("function"), TEXT("happy path"));
			EXS_ExpectString(*Test, Result.Data, TEXT("engine_compile_status"),
				TEXT("succeeded"), TEXT("happy path"));
			EXS_ExpectBool(*Test, Result.Data, TEXT("undo_record_available"), true,
				TEXT("happy path"));

			EXS_ExpectBool(*Test, Result.Data, TEXT("extracted_function_is_pure"), true,
				TEXT("happy path"));

			FString GatewayText;
			if (!EXS_ReadString(*Test, Result.Data, TEXT("gateway_node"), GatewayText))
			{
				return;
			}
			FGuid GatewayGuid;
			if (!FGuid::Parse(GatewayText, GatewayGuid))
			{
				Test->AddError(FString::Printf(TEXT("gateway_node '%s' is not a guid."), *GatewayText));
				return;
			}
			UEdGraphNode* Gateway = EXS_FindNode(Graph, GatewayGuid);
			if (!IsValid(Gateway))
			{
				Test->AddError(TEXT("the reported gateway node is not in the target graph."));
				return;
			}

			const TArray<FString> GatewayExecPins = EXS_ExecPinNames(Gateway);
			Test->TestTrue(FString::Printf(
				TEXT("THE ACCEPTANCE ASSERTION -- the pure extraction's gateway call node carries NO "
				     "exec pins after the refresh (observed: %s). A pure island has no exec edge to "
				     "attach, so a surviving exec pin is what gets the call pruned and every "
				     "consumer reading a type default."), *EXS_Join(GatewayExecPins)),
				GatewayExecPins.Num() == 0);

			FString Edge;
			const bool bFeedsConsumer = EXS_GatewayFeedsConsumer(Gateway, Consumer, Edge);
			Test->TestTrue(FString::Printf(
				TEXT("the consumer '%s' reads a gateway OUTPUT pin (%s), so the computed value "
				     "reaches it. On its own this proves nothing -- the pruned-call defect leaves "
				     "this very link in place -- which is why it is asserted alongside the "
				     "no-exec-pins assertion above."),
				*Island.ConsumerTitle, Edge.IsEmpty() ? TEXT("no edge") : *Edge),
				bFeedsConsumer);

			FString PruneMessage;
			const bool bPruned =
				EXS_HasDiagnosticIdentity(Result.Data, EXS_PrunedExecInUse(), PruneMessage);
			Test->TestFalse(FString::Printf(
				TEXT("no compiler diagnostic carries the identity '%s' (%s)"),
				EXS_PrunedExecInUse(), PruneMessage.IsEmpty() ? TEXT("none present") : *PruneMessage),
				bPruned);

			// Track diagnostics without graph tokens as an engine-behavior check.
			const int32 Total = EXS_ReadInt(Result.Data, TEXT("compiler_diagnostics_total"), -1);
			const int32 Tokenless = EXS_ReadInt(Result.Data, TEXT("compiler_diagnostics_tokenless"), -1);
			Test->TestTrue(TEXT("the diagnostic counts are reported at all"),
				Total >= 0 && Tokenless >= 0);
			Test->TestEqual(FString::Printf(
				TEXT("no diagnostic arrived without an FEdGraphToken (total %d)"), Total),
				Tokenless, 0);

			UEdGraph* Extracted = EXS_FindGraph(&Blueprint, NewName);
			if (!IsValid(Extracted))
			{
				Test->AddError(FString::Printf(TEXT("no extracted graph named '%s' exists."), *NewName));
				return;
			}
			UK2Node_FunctionEntry* Entry = EXS_FindEntry(Extracted);
			if (!IsValid(Entry))
			{
				Test->AddError(TEXT("the extracted graph has no UK2Node_FunctionEntry."));
				return;
			}
			UFunction* Function = IsValid(Blueprint.SkeletonGeneratedClass)
				? Blueprint.SkeletonGeneratedClass->FindFunctionByName(Extracted->GetFName())
				: nullptr;
			if (Function == nullptr)
			{
				Test->AddError(TEXT("the extracted function is not on the skeleton class."));
				return;
			}
			Test->TestTrue(TEXT("the entry node's extra flags carry FUNC_BlueprintPure"),
				Entry->HasAnyExtraFlags(FUNC_BlueprintPure));
			Test->TestTrue(TEXT("the UFunction's flags carry FUNC_BlueprintPure"),
				Function->HasAnyFunctionFlags(FUNC_BlueprintPure));

			// Changing flags without reconstructing pins demonstrates why gateway pins need independent assertions.
			{
				const int32 EntryFlagsBefore = Entry->GetExtraFlags();
				const EFunctionFlags FunctionFlagsBefore = Function->FunctionFlags;

				Entry->ClearExtraFlags(FUNC_BlueprintPure);
				Function->FunctionFlags &= ~FUNC_BlueprintPure;

				const bool bFlagsNowSayImpure = !Entry->HasAnyExtraFlags(FUNC_BlueprintPure)
					&& !Function->HasAnyFunctionFlags(FUNC_BlueprintPure);
				const TArray<FString> ExecPinsAfterFlagFlip = EXS_ExecPinNames(Gateway);

				Test->TestTrue(TEXT("the control actually flipped both flag sites to impure"),
					bFlagsNowSayImpure);
				Test->TestEqual(FString::Printf(
					TEXT("PINS NOT FLAGS: both flag sites now read IMPURE while the gateway's exec "
					     "pin set is unchanged (%s before, %s after). The two are independent "
					     "observations, so a flags-only assertion would have passed on a broken "
					     "refresh."),
					*EXS_Join(GatewayExecPins), *EXS_Join(ExecPinsAfterFlagFlip)),
					ExecPinsAfterFlagFlip.Num(), GatewayExecPins.Num());

				Entry->SetExtraFlags(EntryFlagsBefore);
				Function->FunctionFlags = FunctionFlagsBefore;
			}

			// Observe a committed record as a positive control for refusal-window checks.
			bool bMeasured = false;
			const bool bPresent = EXS_WindowHasTitle(Window, EXS_FunctionTransactionTitle(), bMeasured);
			Test->TestTrue(TEXT("the transaction window is readable"), bMeasured);
			Test->TestTrue(FString::Printf(
				TEXT("the committed extraction DID leave an '%s' record in the editor's buffer -- "
				     "the opposite direction of the refusal tests' assertion, so neither passes on "
				     "a broken observation"), EXS_FunctionTransactionTitle()), bPresent);
		}
	};
}

bool FClaireonBPEditorPureIslandExtractsPure::RunTest(const FString& Parameters)
{
	using namespace ClaireonBPExtractSemanticsInternal;
	if (!EXS_RealAssetConfigured(*this, /*bNeedsFunctionGraph=*/false))
	{
		return true;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FEXS_PureIslandCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("RealAssetPureIsland"))));
	return true;
}

// Without promotion, refuse enclosing-local reads that collapse cannot turn into boundary parameters.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorEnclosingLocalsRefuse,
	"Claireon.BPEditor.ExtractionSemantics.EnclosingLocalsRefuseOnTheRealAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPExtractSemanticsInternal
{
	class FEXS_EnclosingLocalsCommand : public FEXS_CloneCommand
	{
	public:
		using FEXS_CloneCommand::FEXS_CloneCommand;

	protected:
		virtual void RunBody(UBlueprint& Blueprint) override
		{
			UEdGraph* Graph = EXS_FindGraph(&Blueprint, EXS_WarpGraphName());
			if (!IsValid(Graph))
			{
				Test->AddError(FString::Printf(
					TEXT("the clone has no '%s' function graph, so item 3's measured island cannot "
					     "be located."), *EXS_WarpGraphName()));
				return;
			}

			TArray<FEXSIsland> Islands;
			if (!EXS_LintIslands(*Test, AssetPath, Graph->GetName(), Islands))
			{
				return;
			}
			if (Islands.Num() == 0)
			{
				Test->AddError(FString::Printf(
					TEXT("bp_lint reported no pure-data-island in '%s'."), *EXS_WarpGraphName()));
				return;
			}

			const FEXSIsland& Island = Islands[0];
			TSet<UEdGraphNode*> Selection;
			for (const FGuid& Guid : Island.Members)
			{
				if (UEdGraphNode* Node = EXS_FindNode(Graph, Guid); IsValid(Node))
				{
					Selection.Add(Node);
				}
			}
			if (Selection.Num() != Island.Members.Num())
			{
				Test->AddError(FString::Printf(
					TEXT("only %d of the island's %d member guids resolved in '%s'."),
					Selection.Num(), Island.Members.Num(), *EXS_WarpGraphName()));
				return;
			}

			TArray<ClaireonExtractSemantics::FEnclosingLocalFinding> Findings;
			ClaireonExtractSemantics::FindEnclosingLocalViolations(Selection, Findings);
			Test->TestTrue(FString::Printf(
				TEXT("the selection reads at least one enclosing local (found %d)"), Findings.Num()),
				Findings.Num() > 0);
			for (const ClaireonExtractSemantics::FEnclosingLocalFinding& Finding : Findings)
			{
				Test->TestEqual(FString::Printf(
					TEXT("'%s' is a PLAIN READ-ONLY get -- the reason a two-condition predicate "
					     "would have accepted the measured defect"), *Finding.VariableName),
					FString(ClaireonExtractSemantics::ToWireString(Finding.Reason)),
					FString(TEXT("read_only_reference")));
			}

			const int32 NodesBefore = Graph->Nodes.Num();
			const FString NewName = TEXT("EXS_RefusedWarpTransform");
			const FEXSTxnWindow Window = EXS_OpenWindow(*Test);

			const IClaireonTool::FToolResult Result =
				EXS_ExtractFunction(AssetPath, Graph->GetName(), Island.Members, NewName);

			Test->TestTrue(TEXT("the refusal is reported as an error result"), Result.bIsError);
			if (!Result.Data.IsValid())
			{
				Test->AddError(TEXT("the refusal returned no structured data, so it reported no "
					"mutation state -- which is the defect gate 3c closed."));
				return;
			}

			EXS_ExpectString(*Test, Result.Data, TEXT("mutation_state"),
				TEXT("refused"), TEXT("enclosing-local refusal"));
			EXS_ExpectBool(*Test, Result.Data, TEXT("mutation_retained"), false,
				TEXT("enclosing-local refusal"));
			EXS_ExpectBool(*Test, Result.Data, TEXT("undo_record_available"), false,
				TEXT("enclosing-local refusal"));
			EXS_ExpectString(*Test, Result.Data, TEXT("last_completed_phase"),
				kClaireonBPPhaseNone, TEXT("enclosing-local refusal"));
			EXS_ExpectString(*Test, Result.Data, TEXT("quiescence_proof"),
				TEXT("by_construction"), TEXT("enclosing-local refusal"));
			EXS_ExpectString(*Test, Result.Data, TEXT("target_graph"),
				*EXS_WarpGraphName(), TEXT("enclosing-local refusal"));

			// A pre-mutation refusal must not report operation_delta.
			EXS_ExpectAbsent(*Test, Result.Data, TEXT("operation_delta"),
				TEXT("a refusal proven BY CONSTRUCTION captures no snapshot, so there is no delta "
				     "to publish -- one here would mean a walk was paid for nothing and would blur "
				     "the two quiescence proofs together"));
			EXS_ExpectAbsent(*Test, Result.Data, TEXT("failed_phase"),
				TEXT("nothing ran, so no phase failed"));

			EXS_ExpectNoTransactionOpened(*Test, Window, TEXT("enclosing-local refusal"));

			const TArray<TSharedPtr<FJsonValue>>* Locals = nullptr;
			if (!Result.Data->TryGetArrayField(TEXT("enclosing_locals"), Locals) || Locals == nullptr)
			{
				Test->AddError(TEXT("the refusal carried no enclosing_locals evidence, so a caller "
					"cannot tell which local to repair."));
				return;
			}
			Test->TestEqual(TEXT("one evidence entry per refused reference"),
				Locals->Num(), Findings.Num());

			for (const TSharedPtr<FJsonValue>& Value : *Locals)
			{
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Entry) || Entry == nullptr)
				{
					continue;
				}
				FString Variable;
				FString Reason;
				FString NodeGuid;
				(*Entry)->TryGetStringField(TEXT("variable"), Variable);
				(*Entry)->TryGetStringField(TEXT("reason"), Reason);
				(*Entry)->TryGetStringField(TEXT("node_guid"), NodeGuid);

				Test->TestFalse(TEXT("the evidence entry names a variable"), Variable.IsEmpty());
				Test->TestFalse(TEXT("the evidence entry names the referencing node"),
					NodeGuid.IsEmpty());
				Test->TestEqual(FString::Printf(TEXT("'%s' is refused as read_only_reference"),
					*Variable), Reason, FString(TEXT("read_only_reference")));

				Test->TestTrue(FString::Printf(TEXT("the refusal message names '%s'"), *Variable),
					Result.ErrorMessage.Contains(Variable));
			}

			Test->TestTrue(TEXT("the refusal separates the read-only group from the rest"),
				Result.ErrorMessage.Contains(TEXT("Read only:")));

			Test->TestEqual(TEXT("the target graph still holds every node it held"),
				Graph->Nodes.Num(), NodesBefore);
			for (const FGuid& Guid : Island.Members)
			{
				Test->TestTrue(FString::Printf(TEXT("selected node %s is still in '%s'"),
					*Guid.ToString(EGuidFormats::DigitsWithHyphens), *EXS_WarpGraphName()),
					IsValid(EXS_FindNode(Graph, Guid)));
			}
			Test->TestFalse(FString::Printf(TEXT("no graph named '%s' was created"), *NewName),
				IsValid(EXS_FindGraph(&Blueprint, NewName)));
		}
	};
}

bool FClaireonBPEditorEnclosingLocalsRefuse::RunTest(const FString& Parameters)
{
	using namespace ClaireonBPExtractSemanticsInternal;
	if (!EXS_RealAssetConfigured(*this, /*bNeedsFunctionGraph=*/true))
	{
		return true;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FEXS_EnclosingLocalsCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("RealAssetEnclosingLocals"))));
	return true;
}

// Cover reads, writes, and mutable array arguments; ArrayParm metadata must work independently of bIsConst.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorEveryRefusalReasonNamesTheLocal,
	"Claireon.BPEditor.ExtractionSemantics.EveryRefusalReasonNamesTheLocal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPExtractSemanticsInternal
{
	static const TCHAR* EXS_HostGraphName()  { return TEXT("EXS_Host"); }
	static const TCHAR* EXS_ReadOnlyLocal()  { return TEXT("EXSReadOnlyLocal"); }
	static const TCHAR* EXS_WrittenLocal()   { return TEXT("EXSWrittenLocal"); }
	static const TCHAR* EXS_ReadWriteLocal() { return TEXT("EXSReadWriteLocal"); }
	static const TCHAR* EXS_ArrayLocal()     { return TEXT("EXSMutatedNumbers"); }

	static FEdGraphPinType EXS_DoubleType()
	{
		FEdGraphPinType Type;
		Type.PinCategory = UEdGraphSchema_K2::PC_Real;
		Type.PinSubCategory = UEdGraphSchema_K2::PC_Double;
		return Type;
	}

	static FEdGraphPinType EXS_IntArrayType()
	{
		FEdGraphPinType Type;
		Type.PinCategory = UEdGraphSchema_K2::PC_Int;
		Type.ContainerType = EPinContainerType::Array;
		return Type;
	}

	static FEdGraphPinType EXS_VectorType()
	{
		FEdGraphPinType Type;
		Type.PinCategory = UEdGraphSchema_K2::PC_Struct;
		Type.PinSubCategoryObject = TBaseStructure<FVector>::Get();
		return Type;
	}

	/** A local-scope Get or Set of one of the fixture's locals. */
	template <typename TVariableNode>
	static TVariableNode* EXS_AddLocalVariableNode(UBlueprint& Blueprint, UEdGraph& Graph,
		const TCHAR* VariableName, int32 X, int32 Y)
	{
		TVariableNode* Node = NewObject<TVariableNode>(&Graph);
		Graph.AddNode(Node, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		const FGuid VarGuid = FBlueprintEditorUtils::FindLocalVariableGuidByName(
			&Blueprint, &Graph, FName(VariableName));
		Node->VariableReference.SetLocalMember(FName(VariableName), Graph.GetName(), VarGuid);
		Node->CreateNewGuid();
		Node->NodePosX = X;
		Node->NodePosY = Y;
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		return Node;
	}

	static UEdGraphPin* EXS_FirstDataPin(UEdGraphNode* Node, EEdGraphPinDirection Direction,
		const FName NameOrNone)
	{
		if (!IsValid(Node))
		{
			return nullptr;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin == nullptr || Pin->Direction != Direction
				|| Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			if (!NameOrNone.IsNone() && Pin->PinName != NameOrNone)
			{
				continue;
			}
			return Pin;
		}
		return nullptr;
	}

	static UK2Node_CallFunction* EXS_AddCall(UEdGraph& Graph, UFunction& Function,
		bool bArrayFunction, int32 X, int32 Y)
	{
		UK2Node_CallFunction* Call = bArrayFunction
			? NewObject<UK2Node_CallArrayFunction>(&Graph)
			: NewObject<UK2Node_CallFunction>(&Graph);
		Graph.AddNode(Call, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Call->CreateNewGuid();
		Call->SetFromFunction(&Function);
		Call->NodePosX = X;
		Call->NodePosY = Y;
		Call->AllocateDefaultPins();
		return Call;
	}

	/** One refusal case: the selection, and what it must be refused for. */
	struct FEXSRefusalCase
	{
		FString Label;
		TArray<FGuid> Selection;

		/** "<variable>/<wire reason>" pairs the evidence must contain. Duplicated keys allowed. */
		TArray<FString> ExpectedPairs;

		/** Non-empty for the by-reference case: a suffix the reached-pin evidence must carry. */
		FString ExpectedReachedPinSuffix;
	};

	class FEXS_RefusalReasonsCommand : public FEXS_FixtureCommand
	{
	public:
		using FEXS_FixtureCommand::FEXS_FixtureCommand;

	protected:
		virtual bool BuildFixture(FString& OutError) override
		{
			UBlueprint* Blueprint = ClaireonBPEditorFixtures::Create(AssetPath, OutError);
			if (!IsValid(Blueprint))
			{
				return false;
			}

			UEdGraph* Host = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint, FName(EXS_HostGraphName()), UEdGraph::StaticClass(),
				UEdGraphSchema_K2::StaticClass());
			if (!IsValid(Host))
			{
				OutError = TEXT("the host function graph could not be created.");
				return false;
			}
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(
				Blueprint, Host, /*bIsUserCreated=*/true, static_cast<UClass*>(nullptr));

			const bool bLocals =
				FBlueprintEditorUtils::AddLocalVariable(Blueprint, Host,
					FName(EXS_ReadOnlyLocal()), EXS_DoubleType())
				&& FBlueprintEditorUtils::AddLocalVariable(Blueprint, Host,
					FName(EXS_WrittenLocal()), EXS_DoubleType())
				&& FBlueprintEditorUtils::AddLocalVariable(Blueprint, Host,
					FName(EXS_ReadWriteLocal()), EXS_DoubleType())
				&& FBlueprintEditorUtils::AddLocalVariable(Blueprint, Host,
					FName(EXS_ArrayLocal()), EXS_IntArrayType());
			if (!bLocals)
			{
				OutError = TEXT("one of the four function-local variables could not be declared.");
				return false;
			}

			// Compile first so local getters can resolve typed pins through the skeleton UFunction.
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			return true;
		}

		virtual void RunBody(UBlueprint& Blueprint) override
		{
			UEdGraph* Host = EXS_FindGraph(&Blueprint, EXS_HostGraphName());
			if (!IsValid(Host))
			{
				Test->AddError(TEXT("the host function graph did not survive fixture setup."));
				return;
			}

			UFunction* Multiply = UKismetMathLibrary::StaticClass()->FindFunctionByName(
				TEXT("Multiply_DoubleDouble"));
			UFunction* ArrayAdd = UKismetArrayLibrary::StaticClass()->FindFunctionByName(
				TEXT("Array_Add"));
			if (Multiply == nullptr || ArrayAdd == nullptr)
			{
				Test->AddError(TEXT("UKismetMathLibrary::Multiply_DoubleDouble or "
					"UKismetArrayLibrary::Array_Add could not be resolved. That is an engine-side "
					"change, not a fixture problem."));
				return;
			}

			// ---- The read-only case: a plain Get feeding a pure node.
			UK2Node_VariableGet* ReadOnlyGet = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, *Host, EXS_ReadOnlyLocal(), 300, 0);
			UK2Node_CallFunction* MultiplyCall = EXS_AddCall(*Host, *Multiply, false, 600, 0);
			UEdGraphPin* ReadOnlyOut = EXS_FirstDataPin(ReadOnlyGet, EGPD_Output, NAME_None);
			UEdGraphPin* MultiplyA = EXS_FirstDataPin(MultiplyCall, EGPD_Input, FName(TEXT("A")));
			if (ReadOnlyOut == nullptr || MultiplyA == nullptr)
			{
				Test->AddError(TEXT("the read-only fixture could not be wired: a data pin is "
					"missing, which means the local's type did not resolve."));
				return;
			}
			ReadOnlyOut->MakeLinkTo(MultiplyA);

			// ---- The explicit-write case: a Set, on its own.
			UK2Node_VariableSet* WrittenSet = EXS_AddLocalVariableNode<UK2Node_VariableSet>(
				Blueprint, *Host, EXS_WrittenLocal(), 300, 300);

			// Read and write the same local to require both refusal reasons.
			UK2Node_VariableGet* ReadWriteGet = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, *Host, EXS_ReadWriteLocal(), 300, 600);
			UK2Node_VariableSet* ReadWriteSet = EXS_AddLocalVariableNode<UK2Node_VariableSet>(
				Blueprint, *Host, EXS_ReadWriteLocal(), 600, 600);

			// Mutable array argument: local getter feeding Array Add.
			UK2Node_VariableGet* ArrayGet = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, *Host, EXS_ArrayLocal(), 300, 900);
			UK2Node_CallFunction* AddCall = EXS_AddCall(*Host, *ArrayAdd, true, 600, 900);
			UEdGraphPin* ArrayOut = EXS_FirstDataPin(ArrayGet, EGPD_Output, NAME_None);
			UEdGraphPin* TargetArray = AddCall->FindPin(FName(TEXT("TargetArray")), EGPD_Input);
			if (ArrayOut == nullptr || TargetArray == nullptr)
			{
				Test->AddError(TEXT("the by-reference fixture could not be wired: the array Get "
					"output or Array Add's TargetArray pin is missing."));
				return;
			}
			// Through the schema, so the wildcard array pin resolves its type exactly as it
			// would from the graph editor.
			if (!GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(ArrayOut, TargetArray))
			{
				Test->AddError(TEXT("the schema refused Get <local array> -> Array Add.TargetArray, "
					"so DEC-15's canonical case was never built and the assertions below would "
					"pass vacuously."));
				return;
			}
			TargetArray = AddCall->FindPin(FName(TEXT("TargetArray")), EGPD_Input);
			ArrayOut = EXS_FirstDataPin(ArrayGet, EGPD_Output, NAME_None);
			if (TargetArray == nullptr || ArrayOut == nullptr)
			{
				Test->AddError(TEXT("a pin disappeared when the connection resolved the wildcard."));
				return;
			}

			Host->NotifyGraphChanged();

			Test->TestTrue(TEXT("Array Add's TargetArray pin is by-reference"),
				TargetArray->PinType.bIsReference);
			// TargetArray constness varies by engine version; ArrayParm detection must not depend on it.
			Test->AddInfo(FString::Printf(TEXT("engine %s: Array Add TargetArray bIsConst=%s, so "
				"bIsReference && !bIsConst is %s for DEC-15's canonical case"),
				FApp::GetBuildVersion(),
				TargetArray->PinType.bIsConst ? TEXT("true") : TEXT("false"),
				(TargetArray->PinType.bIsReference && !TargetArray->PinType.bIsConst)
					? TEXT("TRUE") : TEXT("FALSE")));
			Test->TestTrue(TEXT("the ArrayParm branch is what answers correctly: TargetArray is "
				"named by its call's MD_ArrayParam metadata"),
				ClaireonExtractSemantics::IsArrayParmPin(TargetArray));

			TArray<FString> Reached;
			Test->TestTrue(TEXT("the by-reference walk reaches TargetArray from the array Get"),
				ClaireonExtractSemantics::ReachesMutableReferencePin(ArrayOut, Reached));
			bool bNamesTargetArray = false;
			for (const FString& PinId : Reached)
			{
				bNamesTargetArray = bNamesTargetArray || PinId.EndsWith(TEXT(".TargetArray"));
			}
			Test->TestTrue(FString::Printf(TEXT("the walk's evidence names the reached pin (%s)"),
				*EXS_Join(Reached)), bNamesTargetArray);

			TArray<FEXSRefusalCase> Cases;
			{
				FEXSRefusalCase Case;
				Case.Label = TEXT("read-only");
				Case.Selection = { ReadOnlyGet->NodeGuid, MultiplyCall->NodeGuid };
				Case.ExpectedPairs = { FString(EXS_ReadOnlyLocal()) + TEXT("/read_only_reference") };
				Cases.Add(MoveTemp(Case));
			}
			{
				FEXSRefusalCase Case;
				Case.Label = TEXT("write-only");
				Case.Selection = { WrittenSet->NodeGuid };
				Case.ExpectedPairs = { FString(EXS_WrittenLocal()) + TEXT("/explicit_write") };
				Cases.Add(MoveTemp(Case));
			}
			{
				FEXSRefusalCase Case;
				Case.Label = TEXT("read/write");
				Case.Selection = { ReadWriteGet->NodeGuid, ReadWriteSet->NodeGuid };
				// One local, two reasons: the Get and the Set are refused independently.
				Case.ExpectedPairs = {
					FString(EXS_ReadWriteLocal()) + TEXT("/explicit_write"),
					FString(EXS_ReadWriteLocal()) + TEXT("/read_only_reference") };
				Cases.Add(MoveTemp(Case));
			}
			{
				FEXSRefusalCase Case;
				Case.Label = TEXT("get reaching a mutable reference pin");
				Case.Selection = { ArrayGet->NodeGuid, AddCall->NodeGuid };
				Case.ExpectedPairs = {
					FString(EXS_ArrayLocal()) + TEXT("/reaches_mutable_reference_pin") };
				Case.ExpectedReachedPinSuffix = TEXT(".TargetArray");
				Cases.Add(MoveTemp(Case));
			}

			for (const FEXSRefusalCase& Case : Cases)
			{
				RunCase(*Host, Case);
			}
		}

	private:
		void RunCase(UEdGraph& Host, const FEXSRefusalCase& Case)
		{
			const TCHAR* Label = *Case.Label;
			const int32 NodesBefore = Host.Nodes.Num();
			const FEXSTxnWindow Window = EXS_OpenWindow(*Test);

			const IClaireonTool::FToolResult Result =
				EXS_ExtractFunction(AssetPath, Host.GetName(), Case.Selection, FString());

			Test->TestTrue(FString::Printf(TEXT("%s: refused as an error result"), Label),
				Result.bIsError);
			if (!Result.Data.IsValid())
			{
				Test->AddError(FString::Printf(
					TEXT("%s: the refusal returned no structured data, so it reported no mutation "
					     "state at all."), Label));
				return;
			}

			// Refusals must open no transaction and capture no snapshot.
			EXS_ExpectString(*Test, Result.Data, TEXT("mutation_state"), TEXT("refused"), Label);
			EXS_ExpectBool(*Test, Result.Data, TEXT("mutation_retained"), false, Label);
			EXS_ExpectBool(*Test, Result.Data, TEXT("undo_record_available"), false, Label);
			EXS_ExpectString(*Test, Result.Data, TEXT("quiescence_proof"),
				TEXT("by_construction"), Label);
			EXS_ExpectAbsent(*Test, Result.Data, TEXT("operation_delta"),
				TEXT("a refusal proven by construction captures no snapshot"));
			EXS_ExpectNoTransactionOpened(*Test, Window, Label);
			Test->TestEqual(FString::Printf(TEXT("%s: the host graph is untouched"), Label),
				Host.Nodes.Num(), NodesBefore);

			const TArray<TSharedPtr<FJsonValue>>* Locals = nullptr;
			if (!Result.Data->TryGetArrayField(TEXT("enclosing_locals"), Locals) || Locals == nullptr)
			{
				Test->AddError(FString::Printf(
					TEXT("%s: the refusal carried no enclosing_locals evidence."), Label));
				return;
			}

			TSet<FString> ObservedPairs;
			for (const TSharedPtr<FJsonValue>& Value : *Locals)
			{
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Entry) || Entry == nullptr)
				{
					continue;
				}
				FString Variable;
				FString Reason;
				(*Entry)->TryGetStringField(TEXT("variable"), Variable);
				(*Entry)->TryGetStringField(TEXT("reason"), Reason);
				ObservedPairs.Add(Variable + TEXT("/") + Reason);

				Test->TestTrue(FString::Printf(
					TEXT("%s: the refusal message names the local '%s'"), Label, *Variable),
					Result.ErrorMessage.Contains(Variable));

				if (!Case.ExpectedReachedPinSuffix.IsEmpty()
					&& Reason.Equals(TEXT("reaches_mutable_reference_pin")))
				{
					const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
					bool bFound = false;
					if ((*Entry)->TryGetArrayField(TEXT("reached_reference_pins"), Pins)
						&& Pins != nullptr)
					{
						for (const TSharedPtr<FJsonValue>& PinValue : *Pins)
						{
							FString PinId;
							if (PinValue.IsValid() && PinValue->TryGetString(PinId)
								&& PinId.EndsWith(Case.ExpectedReachedPinSuffix))
							{
								bFound = true;
							}
						}
					}
					Test->TestTrue(FString::Printf(
						TEXT("%s: the evidence names the reached pin ending '%s' -- a caller "
						     "repairing a fan-out needs every offending path, not a count"),
						Label, *Case.ExpectedReachedPinSuffix), bFound);
				}
			}

			for (const FString& Expected : Case.ExpectedPairs)
			{
				Test->TestTrue(FString::Printf(TEXT("%s: the evidence carries '%s'"),
					Label, *Expected), ObservedPairs.Contains(Expected));
			}
		}
	};
}

bool FClaireonBPEditorEveryRefusalReasonNamesTheLocal::RunTest(const FString& Parameters)
{
	using namespace ClaireonBPExtractSemanticsInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FEXS_RefusalReasonsCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("RefusalReasons"))));
	return true;
}

// Promotion tests compare full pin types, including bIsUObjectWrapper separately from operator==.
// Build each case immediately before extraction to keep fixture construction outside the operation delta.

namespace ClaireonBPExtractSemanticsInternal
{
	// ---------------------------------------------------------------- promotion helpers

	/** Case-sensitive equality; FString::operator== ignores case. */
	static bool EXS_BytesEqual(const FString& A, const FString& B)
	{
		return A.Len() == B.Len() && A.Equals(B, ESearchCase::CaseSensitive);
	}

	static FEdGraphPinType EXS_NameToLinearColorMapType()
	{
		// Use a map to exercise PinValueType.
		FEdGraphPinType Type;
		Type.PinCategory = UEdGraphSchema_K2::PC_Name;
		Type.ContainerType = EPinContainerType::Map;
		Type.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Struct;
		Type.PinValueType.TerminalSubCategoryObject = TBaseStructure<FLinearColor>::Get();
		return Type;
	}

	/** A user-created function graph on the fixture, with its own local scope. */
	static UEdGraph* EXS_MakeFunctionGraph(UBlueprint& Blueprint, const TCHAR* GraphName)
	{
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
			&Blueprint, FName(GraphName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!IsValid(Graph))
		{
			return nullptr;
		}
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(
			&Blueprint, Graph, /*bIsUserCreated=*/true, static_cast<UClass*>(nullptr));
		return Graph;
	}

	/** A Get or Set of a MEMBER variable -- resolves on self, so it is never an enclosing local. */
	template <typename TVariableNode>
	static TVariableNode* EXS_AddMemberVariableNode(UEdGraph& Graph, const TCHAR* VariableName,
		int32 X, int32 Y)
	{
		TVariableNode* Node = NewObject<TVariableNode>(&Graph);
		Graph.AddNode(Node, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Node->VariableReference.SetSelfMember(FName(VariableName));
		Node->CreateNewGuid();
		Node->NodePosX = X;
		Node->NodePosY = Y;
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		return Node;
	}

	/** The function graph's own entry exec output, so an impure selection has a reachable path. */
	static UEdGraphPin* EXS_HostThenPin(UEdGraph* Graph)
	{
		UK2Node_FunctionEntry* Entry = EXS_FindEntry(Graph);
		return IsValid(Entry) ? Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output) : nullptr;
	}

	/** One entry in the result's `promoted_locals` array. */
	struct FEXSPromotedLocal
	{
		FString Variable;
		FString Parameter;
		FString ParameterType;
		FString EarliestGetNode;
		FString CallSiteReadNode;
		int32 GetNodesDeleted = 0;
		int32 ConsumerLinksRewired = 0;
		int32 OrderY = 0;
		int32 OrderX = 0;
		int32 OrderPinIndex = 0;
	};

	static bool EXS_ReadPromotedLocals(FAutomationTestBase& Test, const TSharedPtr<FJsonObject>& Data,
		TArray<FEXSPromotedLocal>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(TEXT("promoted_locals"), Entries)
			|| Entries == nullptr)
		{
			Test.AddError(TEXT("the result carried no promoted_locals array, so the signature it "
				"synthesized cannot be read back from the envelope."));
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Entries)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Entry) || Entry == nullptr)
			{
				continue;
			}
			FEXSPromotedLocal Record;
			(*Entry)->TryGetStringField(TEXT("variable"), Record.Variable);
			(*Entry)->TryGetStringField(TEXT("parameter"), Record.Parameter);
			(*Entry)->TryGetStringField(TEXT("parameter_type"), Record.ParameterType);
			(*Entry)->TryGetStringField(TEXT("earliest_get_node"), Record.EarliestGetNode);
			(*Entry)->TryGetStringField(TEXT("callsite_read_node"), Record.CallSiteReadNode);
			Record.GetNodesDeleted = EXS_ReadInt(*Entry, TEXT("get_nodes_deleted"), -1);
			Record.ConsumerLinksRewired = EXS_ReadInt(*Entry, TEXT("consumer_links_rewired"), -1);
			Record.OrderY = EXS_ReadInt(*Entry, TEXT("order_node_pos_y"), MIN_int32);
			Record.OrderX = EXS_ReadInt(*Entry, TEXT("order_node_pos_x"), MIN_int32);
			Record.OrderPinIndex = EXS_ReadInt(*Entry, TEXT("order_pin_index"), MIN_int32);
			Out.Add(MoveTemp(Record));
		}
		return true;
	}

	/** Everything the envelope said about the synthesized signature, as one comparable string. */
	static FString EXS_CanonicalPromotion(const TArray<FEXSPromotedLocal>& Locals)
	{
		TArray<FString> Parts;
		for (const FEXSPromotedLocal& Local : Locals)
		{
			Parts.Add(FString::Printf(TEXT("%s|%s|%s|%d|%d|%d|%d|%d"),
				*Local.Variable, *Local.Parameter, *Local.ParameterType,
				Local.GetNodesDeleted, Local.ConsumerLinksRewired,
				Local.OrderY, Local.OrderX, Local.OrderPinIndex));
		}
		return FString::Join(Parts, TEXT(" ;; "));
	}

	/** The entry node's user-defined pins, in order, as one comparable string. */
	static FString EXS_EntrySignatureText(const UK2Node_EditablePinBase* Entry)
	{
		if (!IsValid(Entry))
		{
			return FString(TEXT("<no entry node>"));
		}
		TArray<FString> Parts;
		for (const TSharedPtr<FUserPinInfo>& UserPin : Entry->UserDefinedPins)
		{
			if (UserPin.IsValid())
			{
				Parts.Add(FString::Printf(TEXT("%s:%s"), *UserPin->PinName.ToString(),
					*UEdGraphSchema_K2::TypeToText(UserPin->PinType).ToString()));
			}
		}
		return FString::Join(Parts, TEXT(","));
	}

	/** The user-defined pin names on an entry node, in declaration order. */
	static TArray<FString> EXS_EntryPinNames(const UK2Node_EditablePinBase* Entry)
	{
		TArray<FString> Names;
		if (IsValid(Entry))
		{
			for (const TSharedPtr<FUserPinInfo>& UserPin : Entry->UserDefinedPins)
			{
				if (UserPin.IsValid())
				{
					Names.Add(UserPin->PinName.ToString());
				}
			}
		}
		return Names;
	}

	/** Field-for-field type equality, reported so a failure names the field that died. */
	static void EXS_ExpectPinTypeEquals(FAutomationTestBase& Test, const TCHAR* What,
		const FEdGraphPinType& Observed, const FEdGraphPinType& Expected)
	{
		Test.TestTrue(FString::Printf(
			TEXT("%s: the promoted parameter's FEdGraphPinType equals the source local's value-pin "
			     "type (expected '%s', observed '%s')"),
			What, *UEdGraphSchema_K2::TypeToText(Expected).ToString(),
			*UEdGraphSchema_K2::TypeToText(Observed).ToString()),
			Observed == Expected);

		// operator== does NOT compare bIsUObjectWrapper, so it is asserted on its own rather
		// than assumed to be covered.
		Test.TestTrue(FString::Printf(TEXT("%s: bIsUObjectWrapper survived (operator== does not "
			"compare it, so it needs its own assertion)"), What),
			Observed.bIsUObjectWrapper == Expected.bIsUObjectWrapper);

		Test.TestTrue(FString::Printf(TEXT("%s: PinCategory is '%s'"), What,
			*Expected.PinCategory.ToString()), Observed.PinCategory == Expected.PinCategory);
		Test.TestTrue(FString::Printf(TEXT("%s: ContainerType is %d"), What,
			static_cast<int32>(Expected.ContainerType)),
			Observed.ContainerType == Expected.ContainerType);
		Test.TestTrue(FString::Printf(TEXT("%s: PinSubCategoryObject is '%s'"), What,
			Expected.PinSubCategoryObject.IsValid()
				? *Expected.PinSubCategoryObject->GetName() : TEXT("none")),
			Observed.PinSubCategoryObject == Expected.PinSubCategoryObject);
		Test.TestTrue(FString::Printf(TEXT("%s: PinValueType (the map value terminal) survived"), What),
			Observed.PinValueType == Expected.PinValueType);

		Test.TestFalse(FString::Printf(TEXT("%s: the promoted parameter is an input COPY, so "
			"bIsReference is cleared and never copied"), What), Observed.bIsReference);
	}

	/** The UK2Node_VariableGet feeding one input pin of the gateway, or null. */
	static UK2Node_VariableGet* EXS_CallSiteRead(UEdGraphNode* Gateway, const FName ParameterName)
	{
		if (!IsValid(Gateway))
		{
			return nullptr;
		}
		const UEdGraphPin* Pin = Gateway->FindPin(ParameterName, EGPD_Input);
		if (Pin == nullptr)
		{
			return nullptr;
		}
		for (UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (Linked == nullptr)
			{
				continue;
			}
			if (UK2Node_VariableGet* Read = Cast<UK2Node_VariableGet>(Linked->GetOwningNodeUnchecked()); IsValid(Read))
			{
				return Read;
			}
		}
		return nullptr;
	}

	/** Every UK2Node_VariableGet in a graph that names this variable. */
	static int32 EXS_CountVariableGets(const UEdGraph* Graph, const FString& VariableName)
	{
		int32 Count = 0;
		if (IsValid(Graph))
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				const UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Node);
				if (IsValid(Get) && Get->VariableReference.GetMemberName().ToString().Equals(VariableName))
				{
					++Count;
				}
			}
		}
		return Count;
	}

	/** Whatever a failed extraction said was wrong with the result, so a run can be diagnosed. */
	static FString EXS_ValidationFailureText(const TSharedPtr<FJsonObject>& Data)
	{
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(TEXT("validation_failures"), Entries)
			|| Entries == nullptr)
		{
			return FString(TEXT("none reported"));
		}
		TArray<FString> Parts;
		for (const TSharedPtr<FJsonValue>& Value : *Entries)
		{
			FString Text;
			if (Value.IsValid() && Value->TryGetString(Text))
			{
				Parts.Add(Text);
			}
		}
		return Parts.Num() == 0 ? FString(TEXT("none reported")) : FString::Join(Parts, TEXT(" | "));
	}

	/** Which of the two extract tools that can be asked for promotion to invoke. */
	enum class EEXSExtractTool : uint8
	{
		Function,
		Event,
	};

	static IClaireonTool::FToolResult EXS_ExtractWith(EEXSExtractTool Which, const FString& AssetPath,
		const FString& GraphName, const TArray<FGuid>& NodeGuids, const FString& NewName,
		bool bPromoteEnclosingLocals)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("graph_name"), GraphName);
		Args->SetStringField(TEXT("response_mode"), TEXT("status"));
		if (!NewName.IsEmpty())
		{
			Args->SetStringField(TEXT("new_name"), NewName);
		}
		if (bPromoteEnclosingLocals)
		{
			Args->SetBoolField(TEXT("promote_enclosing_locals"), true);
		}

		TArray<TSharedPtr<FJsonValue>> Guids;
		for (const FGuid& Guid : NodeGuids)
		{
			Guids.Add(MakeShared<FJsonValueString>(Guid.ToString(EGuidFormats::DigitsWithHyphens)));
		}
		Args->SetArrayField(TEXT("node_guids"), Guids);

		if (Which == EEXSExtractTool::Event)
		{
			ClaireonBlueprintGraphTool_ExtractEvent Tool;
			return Tool.Execute(Args);
		}
		ClaireonBlueprintGraphTool_ExtractFunction Tool;
		return Tool.Execute(Args);
	}

	/** One accepted promotion, resolved back to the live graph it produced. */
	struct FEXSPromotionRun
	{
		bool bOk = false;
		FString ExtractedGraphName;
		UEdGraph* Extracted = nullptr;
		UK2Node_FunctionEntry* Entry = nullptr;
		UEdGraphNode* Gateway = nullptr;
		TArray<FEXSPromotedLocal> Promoted;
		FString SignatureBefore;
		FString SignatureAfter;
	};

	/** Extract with promotion enabled and resolve assertion inputs, reporting the tool diagnosis on failure. */
	static FEXSPromotionRun EXS_RunPromotion(FAutomationTestBase& Test, UBlueprint& Blueprint,
		const FString& AssetPath, const FString& GraphName, const TArray<FGuid>& Selection,
		const FString& NewName, const TCHAR* Label)
	{
		FEXSPromotionRun Run;
		const IClaireonTool::FToolResult Result = EXS_ExtractWith(
			EEXSExtractTool::Function, AssetPath, GraphName, Selection, NewName,
			/*bPromoteEnclosingLocals=*/true);

		if (Result.bIsError || !Result.Data.IsValid())
		{
			Test.AddError(FString::Printf(
				TEXT("%s: the promoting extraction failed: %s (validation_failures: %s)"),
				Label, *Result.ErrorMessage, *EXS_ValidationFailureText(Result.Data)));
			return Run;
		}

		EXS_ExpectString(Test, Result.Data, TEXT("mutation_state"), TEXT("applied_clean"), Label);
		EXS_ExpectString(Test, Result.Data, TEXT("engine_compile_status"), TEXT("succeeded"), Label);
		EXS_ExpectBool(Test, Result.Data, TEXT("enclosing_locals_promoted"), true, Label);

		if (!EXS_ReadPromotedLocals(Test, Result.Data, Run.Promoted))
		{
			return Run;
		}
		EXS_ReadString(Test, Result.Data, TEXT("signature_before"), Run.SignatureBefore);
		EXS_ReadString(Test, Result.Data, TEXT("signature_after"), Run.SignatureAfter);

		if (!EXS_ReadString(Test, Result.Data, TEXT("extracted_graph"), Run.ExtractedGraphName))
		{
			return Run;
		}
		Run.Extracted = EXS_FindGraph(&Blueprint, Run.ExtractedGraphName);
		if (!IsValid(Run.Extracted))
		{
			Test.AddError(FString::Printf(TEXT("%s: no graph named '%s' exists after the extraction."),
				Label, *Run.ExtractedGraphName));
			return Run;
		}
		Run.Entry = EXS_FindEntry(Run.Extracted);
		if (!IsValid(Run.Entry))
		{
			Test.AddError(FString::Printf(
				TEXT("%s: the extracted graph has no UK2Node_FunctionEntry to carry a signature."),
				Label));
			return Run;
		}

		FString GatewayText;
		FGuid GatewayGuid;
		UEdGraph* SourceGraph = EXS_FindGraph(&Blueprint, GraphName);
		if (EXS_ReadString(Test, Result.Data, TEXT("gateway_node"), GatewayText)
			&& FGuid::Parse(GatewayText, GatewayGuid))
		{
			Run.Gateway = EXS_FindNode(SourceGraph, GatewayGuid);
		}
		if (!IsValid(Run.Gateway))
		{
			Test.AddError(FString::Printf(
				TEXT("%s: the reported gateway node is not in the source graph, so the call-site "
				     "wiring cannot be checked."), Label));
			return Run;
		}

		Run.bOk = true;
		return Run;
	}
}

// Cover read-only locals, duplicate reads, containers, name collisions, and ordering.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorPromotionMatrix,
	"Claireon.BPEditor.Promotion.AcceptedShapesPromoteFaithfully",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPExtractSemanticsInternal
{
	static const TCHAR* EXS_PM_ReadOnlyGraph()  { return TEXT("EXSPromoReadOnly"); }
	static const TCHAR* EXS_PM_DuplicateGraph() { return TEXT("EXSPromoDuplicate"); }
	static const TCHAR* EXS_PM_ContainerGraph() { return TEXT("EXSPromoContainer"); }
	static const TCHAR* EXS_PM_CollisionGraph() { return TEXT("EXSPromoCollision"); }
	static const TCHAR* EXS_PM_OrderGraph()     { return TEXT("EXSPromoOrder"); }

	static const TCHAR* EXS_PM_ReadAlpha()    { return TEXT("PReadAlpha"); }
	static const TCHAR* EXS_PM_DupBeta()      { return TEXT("PDupBeta"); }
	static const TCHAR* EXS_PM_MapLocal()     { return TEXT("PMapLocal"); }
	static const TCHAR* EXS_PM_MapMember()    { return TEXT("EXSPromoMapMember"); }
	/** Deliberately one character: it is the name the engine's own boundary pin takes. */
	static const TCHAR* EXS_PM_CollidingLocal() { return TEXT("A"); }
	static const TCHAR* EXS_PM_CollisionMember() { return TEXT("PCollisionSource"); }
	static const TCHAR* EXS_PM_OrderZulu()    { return TEXT("POrderZulu"); }
	static const TCHAR* EXS_PM_OrderAlpha()   { return TEXT("POrderAlpha"); }
	static const TCHAR* EXS_PM_OrderMike()    { return TEXT("POrderMike"); }

	class FEXS_PromotionMatrixCommand : public FEXS_FixtureCommand
	{
	public:
		using FEXS_FixtureCommand::FEXS_FixtureCommand;

	protected:
		virtual bool BuildFixture(FString& OutError) override
		{
			UBlueprint* Blueprint = ClaireonBPEditorFixtures::Create(AssetPath, OutError);
			if (!IsValid(Blueprint))
			{
				return false;
			}

			UEdGraph* ReadOnly  = EXS_MakeFunctionGraph(*Blueprint, EXS_PM_ReadOnlyGraph());
			UEdGraph* Duplicate = EXS_MakeFunctionGraph(*Blueprint, EXS_PM_DuplicateGraph());
			UEdGraph* Container = EXS_MakeFunctionGraph(*Blueprint, EXS_PM_ContainerGraph());
			UEdGraph* Collision = EXS_MakeFunctionGraph(*Blueprint, EXS_PM_CollisionGraph());
			UEdGraph* Order     = EXS_MakeFunctionGraph(*Blueprint, EXS_PM_OrderGraph());
			if (!IsValid(ReadOnly) || !IsValid(Duplicate) || !IsValid(Container)
				|| !IsValid(Collision) || !IsValid(Order))
			{
				OutError = TEXT("one of the five case graphs could not be created.");
				return false;
			}

			const FEdGraphPinType MapType = EXS_NameToLinearColorMapType();
			const bool bVars =
				FBlueprintEditorUtils::AddLocalVariable(Blueprint, ReadOnly,
					FName(EXS_PM_ReadAlpha()), EXS_DoubleType())
				&& FBlueprintEditorUtils::AddLocalVariable(Blueprint, Duplicate,
					FName(EXS_PM_DupBeta()), EXS_DoubleType())
				&& FBlueprintEditorUtils::AddLocalVariable(Blueprint, Container,
					FName(EXS_PM_MapLocal()), MapType)
				&& FBlueprintEditorUtils::AddMemberVariable(Blueprint,
					FName(EXS_PM_MapMember()), MapType)
				&& FBlueprintEditorUtils::AddLocalVariable(Blueprint, Collision,
					FName(EXS_PM_CollidingLocal()), EXS_DoubleType())
				&& FBlueprintEditorUtils::AddMemberVariable(Blueprint,
					FName(EXS_PM_CollisionMember()), EXS_DoubleType())
				// Declared Zulu, Alpha, Mike -- so declaration order is neither the
				// alphabetical order nor the graph-position order the comparator must produce.
				&& FBlueprintEditorUtils::AddLocalVariable(Blueprint, Order,
					FName(EXS_PM_OrderZulu()), EXS_DoubleType())
				&& FBlueprintEditorUtils::AddLocalVariable(Blueprint, Order,
					FName(EXS_PM_OrderAlpha()), EXS_DoubleType())
				&& FBlueprintEditorUtils::AddLocalVariable(Blueprint, Order,
					FName(EXS_PM_OrderMike()), EXS_DoubleType());
			if (!bVars)
			{
				OutError = TEXT("one of the fixture's local or member variables could not be declared.");
				return false;
			}

			// Compile first so local getters resolve typed pins through the skeleton UFunction.
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			return true;
		}

		virtual void RunBody(UBlueprint& Blueprint) override
		{
			UFunction* Multiply = UKismetMathLibrary::StaticClass()->FindFunctionByName(
				TEXT("Multiply_DoubleDouble"));
			if (Multiply == nullptr)
			{
				Test->AddError(TEXT("UKismetMathLibrary::Multiply_DoubleDouble could not be resolved. "
					"That is an engine-side change, not a fixture problem."));
				return;
			}

			RunReadOnly(Blueprint, *Multiply);
			RunDuplicateReads(Blueprint, *Multiply);
			RunContainer(Blueprint);
			RunNameCollision(Blueprint, *Multiply);
			RunMultiLocalOrdering(Blueprint, *Multiply);
		}

	private:
		/** A promoted local becomes one input; replace its internal getter with one beside the gateway. */
		void RunReadOnly(UBlueprint& Blueprint, UFunction& Multiply)
		{
			const TCHAR* Label = TEXT("read-only");
			UEdGraph* Graph = EXS_FindGraph(&Blueprint, EXS_PM_ReadOnlyGraph());
			if (!IsValid(Graph))
			{
				Test->AddError(TEXT("read-only: the case graph did not survive fixture setup."));
				return;
			}

			UK2Node_VariableGet* Get = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, *Graph, EXS_PM_ReadAlpha(), 300, 0);
			UK2Node_CallFunction* Inside = EXS_AddCall(*Graph, Multiply, false, 600, 0);
			UK2Node_CallFunction* Outside = EXS_AddCall(*Graph, Multiply, false, 900, 0);

			UEdGraphPin* GetValue = EXS_FirstDataPin(Get, EGPD_Output, NAME_None);
			UEdGraphPin* InsideA = EXS_FirstDataPin(Inside, EGPD_Input, FName(TEXT("A")));
			UEdGraphPin* InsideRet = Inside->GetReturnValuePin();
			UEdGraphPin* OutsideA = EXS_FirstDataPin(Outside, EGPD_Input, FName(TEXT("A")));
			if (GetValue == nullptr || InsideA == nullptr || InsideRet == nullptr || OutsideA == nullptr)
			{
				Test->AddError(TEXT("read-only: the fixture could not be wired -- a data pin is "
					"missing, which means the local's type did not resolve."));
				return;
			}
			const FEdGraphPinType SourceType = GetValue->PinType;
			GetValue->MakeLinkTo(InsideA);
			InsideRet->MakeLinkTo(OutsideA);
			Graph->NotifyGraphChanged();

			const FGuid InsideGuid = Inside->NodeGuid;
			const FEXSPromotionRun Run = EXS_RunPromotion(*Test, Blueprint, AssetPath,
				Graph->GetName(), { Get->NodeGuid, Inside->NodeGuid },
				TEXT("EXSPromotedReadOnly"), Label);
			if (!Run.bOk)
			{
				return;
			}

			if (Run.Promoted.Num() != 1)
			{
				Test->AddError(FString::Printf(
					TEXT("read-only: expected exactly one promoted local, got %d."), Run.Promoted.Num()));
				return;
			}
			const FEXSPromotedLocal& Promoted = Run.Promoted[0];
			Test->TestEqual(TEXT("read-only: the promoted local is named"),
				Promoted.Variable, FString(EXS_PM_ReadAlpha()));
			Test->TestEqual(TEXT("read-only: the parameter took the local's own name, uncontested"),
				Promoted.Parameter, FString(EXS_PM_ReadAlpha()));
			Test->TestEqual(TEXT("read-only: one Get was deleted"), Promoted.GetNodesDeleted, 1);
			Test->TestEqual(TEXT("read-only: one consumer link was rewired to the entry pin"),
				Promoted.ConsumerLinksRewired, 1);

			Test->TestTrue(FString::Printf(
				TEXT("read-only: signature_after names the parameter (before '%s', after '%s')"),
				*Run.SignatureBefore, *Run.SignatureAfter),
				Run.SignatureAfter.Contains(EXS_PM_ReadAlpha()));
			Test->TestFalse(TEXT("read-only: signature_before did not"),
				Run.SignatureBefore.Contains(EXS_PM_ReadAlpha()));

			UEdGraphPin* EntryPin = Run.Entry->FindPin(FName(*Promoted.Parameter), EGPD_Output);
			if (EntryPin == nullptr)
			{
				Test->AddError(FString::Printf(
					TEXT("read-only: the entry node grew no output pin named '%s'; it carries %s."),
					*Promoted.Parameter, *EXS_Join(EXS_EntryPinNames(Run.Entry))));
				return;
			}
			EXS_ExpectPinTypeEquals(*Test, Label, EntryPin->PinType, SourceType);

			Test->TestEqual(FString::Printf(
				TEXT("read-only: DISPOSAL (a) -- no VariableGet of '%s' survives inside the extracted "
				     "function"), EXS_PM_ReadAlpha()),
				EXS_CountVariableGets(Run.Extracted, EXS_PM_ReadAlpha()), 0);

			UEdGraphNode* MovedInside = EXS_FindNode(Run.Extracted, InsideGuid);
			if (!IsValid(MovedInside))
			{
				Test->AddError(TEXT("read-only: the consumer did not move into the extracted graph."));
				return;
			}
			UEdGraphPin* MovedA = MovedInside->FindPin(FName(TEXT("A")), EGPD_Input);
			const bool bReadsEntry = MovedA != nullptr && MovedA->LinkedTo.Num() == 1
				&& MovedA->LinkedTo[0] == EntryPin;
			Test->TestTrue(TEXT("read-only: the consumer's input pin now reads the ENTRY node's new "
				"parameter pin, which is the whole of disposal (a)"), bReadsEntry);

			UEdGraph* Source = EXS_FindGraph(&Blueprint, EXS_PM_ReadOnlyGraph());
			Test->TestEqual(FString::Printf(
				TEXT("read-only: exactly one VariableGet of '%s' now stands in the source graph -- "
				     "the read the selection used to perform, re-established next to the gateway"),
				EXS_PM_ReadAlpha()),
				EXS_CountVariableGets(Source, EXS_PM_ReadAlpha()), 1);

			UK2Node_VariableGet* CallSiteRead = EXS_CallSiteRead(Run.Gateway, FName(*Promoted.Parameter));
			if (!IsValid(CallSiteRead))
			{
				Test->AddError(FString::Printf(
					TEXT("read-only: the gateway's '%s' input pin is not fed by a VariableGet, so the "
					     "promoted parameter has no value at the call site."), *Promoted.Parameter));
				return;
			}
			Test->TestEqual(TEXT("read-only: the call-site read names the same local"),
				CallSiteRead->VariableReference.GetMemberName().ToString(),
				FString(EXS_PM_ReadAlpha()));
			Test->TestTrue(TEXT("read-only: the call-site read is LOCAL scope -- its MemberScope "
				"still names the enclosing function, where the local genuinely lives"),
				CallSiteRead->VariableReference.IsLocalScope());
			Test->TestEqual(TEXT("read-only: and that MemberScope is the source graph"),
				CallSiteRead->VariableReference.GetMemberScopeName(), FString(EXS_PM_ReadOnlyGraph()));
			Test->TestEqual(TEXT("read-only: the result named that node"),
				Promoted.CallSiteReadNode,
				CallSiteRead->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		}

		/** CASE 2: N reads of one local produce ONE parameter and ONE call-site read. */
		void RunDuplicateReads(UBlueprint& Blueprint, UFunction& Multiply)
		{
			const TCHAR* Label = TEXT("duplicate reads");
			UEdGraph* Graph = EXS_FindGraph(&Blueprint, EXS_PM_DuplicateGraph());
			if (!IsValid(Graph))
			{
				Test->AddError(TEXT("duplicate reads: the case graph did not survive fixture setup."));
				return;
			}

			UK2Node_VariableGet* First = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, *Graph, EXS_PM_DupBeta(), 300, 0);
			UK2Node_VariableGet* Second = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, *Graph, EXS_PM_DupBeta(), 300, 200);
			UK2Node_CallFunction* Inside = EXS_AddCall(*Graph, Multiply, false, 600, 0);
			UK2Node_CallFunction* Outside = EXS_AddCall(*Graph, Multiply, false, 900, 0);

			UEdGraphPin* FirstValue = EXS_FirstDataPin(First, EGPD_Output, NAME_None);
			UEdGraphPin* SecondValue = EXS_FirstDataPin(Second, EGPD_Output, NAME_None);
			UEdGraphPin* InsideA = EXS_FirstDataPin(Inside, EGPD_Input, FName(TEXT("A")));
			UEdGraphPin* InsideB = EXS_FirstDataPin(Inside, EGPD_Input, FName(TEXT("B")));
			UEdGraphPin* InsideRet = Inside->GetReturnValuePin();
			UEdGraphPin* OutsideA = EXS_FirstDataPin(Outside, EGPD_Input, FName(TEXT("A")));
			if (FirstValue == nullptr || SecondValue == nullptr || InsideA == nullptr
				|| InsideB == nullptr || InsideRet == nullptr || OutsideA == nullptr)
			{
				Test->AddError(TEXT("duplicate reads: the fixture could not be wired."));
				return;
			}
			FirstValue->MakeLinkTo(InsideA);
			SecondValue->MakeLinkTo(InsideB);
			InsideRet->MakeLinkTo(OutsideA);
			Graph->NotifyGraphChanged();

			const FGuid InsideGuid = Inside->NodeGuid;
			const FEXSPromotionRun Run = EXS_RunPromotion(*Test, Blueprint, AssetPath,
				Graph->GetName(), { First->NodeGuid, Second->NodeGuid, Inside->NodeGuid },
				TEXT("EXSPromotedDuplicate"), Label);
			if (!Run.bOk)
			{
				return;
			}

			Test->TestEqual(TEXT("duplicate reads: TWO Gets of one local produce ONE parameter, not "
				"one per access"), Run.Promoted.Num(), 1);
			if (Run.Promoted.Num() != 1)
			{
				return;
			}
			const FEXSPromotedLocal& Promoted = Run.Promoted[0];
			Test->TestEqual(TEXT("duplicate reads: both Gets were deleted"),
				Promoted.GetNodesDeleted, 2);
			Test->TestEqual(TEXT("duplicate reads: both consumer links were rewired"),
				Promoted.ConsumerLinksRewired, 2);
			Test->TestEqual(TEXT("duplicate reads: the entry node carries exactly one user-defined "
				"pin, so the deduplication is visible on the signature and not only in the report"),
				EXS_EntryPinNames(Run.Entry).Num(), 1);

			UEdGraphPin* EntryPin = Run.Entry->FindPin(FName(*Promoted.Parameter), EGPD_Output);
			UEdGraphNode* MovedInside = EXS_FindNode(Run.Extracted, InsideGuid);
			UEdGraphPin* MovedA = IsValid(MovedInside)
				? MovedInside->FindPin(FName(TEXT("A")), EGPD_Input) : nullptr;
			UEdGraphPin* MovedB = IsValid(MovedInside)
				? MovedInside->FindPin(FName(TEXT("B")), EGPD_Input) : nullptr;
			const bool bBothReadOnePin = EntryPin != nullptr && MovedA != nullptr && MovedB != nullptr
				&& MovedA->LinkedTo.Num() == 1 && MovedA->LinkedTo[0] == EntryPin
				&& MovedB->LinkedTo.Num() == 1 && MovedB->LinkedTo[0] == EntryPin;
			Test->TestTrue(TEXT("duplicate reads: BOTH consumer pins read the SAME entry pin"),
				bBothReadOnePin);

			Test->TestEqual(TEXT("duplicate reads: no Get of the local survives inside the function"),
				EXS_CountVariableGets(Run.Extracted, EXS_PM_DupBeta()), 0);
			Test->TestEqual(TEXT("duplicate reads: exactly ONE call-site read was created, not two"),
				EXS_CountVariableGets(EXS_FindGraph(&Blueprint, EXS_PM_DuplicateGraph()),
					EXS_PM_DupBeta()), 1);
		}

		/** Preserve the map value terminal during promotion. */
		void RunContainer(UBlueprint& Blueprint)
		{
			const TCHAR* Label = TEXT("container");
			UEdGraph* Graph = EXS_FindGraph(&Blueprint, EXS_PM_ContainerGraph());
			if (!IsValid(Graph))
			{
				Test->AddError(TEXT("container: the case graph did not survive fixture setup."));
				return;
			}

			UK2Node_VariableGet* Get = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, *Graph, EXS_PM_MapLocal(), 300, 0);
			UK2Node_VariableSet* SetMember = EXS_AddMemberVariableNode<UK2Node_VariableSet>(
				*Graph, EXS_PM_MapMember(), 600, 0);

			UEdGraphPin* GetValue = EXS_FirstDataPin(Get, EGPD_Output, NAME_None);
			UEdGraphPin* SetValue = SetMember->FindPin(FName(EXS_PM_MapMember()), EGPD_Input);
			UEdGraphPin* SetExec = SetMember->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
			UEdGraphPin* HostThen = EXS_HostThenPin(Graph);
			if (GetValue == nullptr || SetValue == nullptr || SetExec == nullptr || HostThen == nullptr)
			{
				Test->AddError(TEXT("container: the fixture could not be wired -- the map Get's value "
					"pin, the member Set's value or exec pin, or the host entry's then pin is "
					"missing."));
				return;
			}
			// Connect exec from outside the selection to avoid an unrelated unreachable-node diagnostic.
			HostThen->MakeLinkTo(SetExec);
			if (!GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(GetValue, SetValue))
			{
				Test->AddError(TEXT("container: the schema refused Get <local map> -> Set <member "
					"map>, so the container case was never built."));
				return;
			}
			GetValue = EXS_FirstDataPin(Get, EGPD_Output, NAME_None);
			if (GetValue == nullptr)
			{
				Test->AddError(TEXT("container: the map Get's value pin disappeared on connect."));
				return;
			}
			const FEdGraphPinType SourceType = GetValue->PinType;
			Graph->NotifyGraphChanged();

			Test->TestTrue(TEXT("container: the source local really is a Map"),
				SourceType.ContainerType == EPinContainerType::Map);
			Test->TestTrue(TEXT("container: with a struct value terminal"),
				SourceType.PinValueType.TerminalCategory == UEdGraphSchema_K2::PC_Struct
				&& SourceType.PinValueType.TerminalSubCategoryObject
					== TBaseStructure<FLinearColor>::Get());

			const FEXSPromotionRun Run = EXS_RunPromotion(*Test, Blueprint, AssetPath,
				Graph->GetName(), { Get->NodeGuid, SetMember->NodeGuid },
				TEXT("EXSPromotedContainer"), Label);
			if (!Run.bOk || Run.Promoted.Num() != 1)
			{
				Test->TestEqual(TEXT("container: exactly one promoted local"), Run.Promoted.Num(), 1);
				return;
			}

			UEdGraphPin* EntryPin = Run.Entry->FindPin(FName(*Run.Promoted[0].Parameter), EGPD_Output);
			if (EntryPin == nullptr)
			{
				Test->AddError(FString::Printf(
					TEXT("container: the entry node grew no pin named '%s'; it carries %s."),
					*Run.Promoted[0].Parameter, *EXS_Join(EXS_EntryPinNames(Run.Entry))));
				return;
			}

			EXS_ExpectPinTypeEquals(*Test, Label, EntryPin->PinType, SourceType);

			// Record the string-grammar round trip alongside the full pin-type comparison.
			const FString Rendered = ClaireonBlueprintHelpers::FormatVariableTypeString(SourceType);
			const FEdGraphPinType Reparsed = ClaireonBlueprintHelpers::ParseVariableType(Rendered);
			Test->AddInfo(FString::Printf(
				TEXT("container: the string-grammar round trip of this type renders as '%s' and "
				     "re-parses to '%s'; it %s the source type field for field. The promoted "
				     "parameter did not go through it."),
				*Rendered, *UEdGraphSchema_K2::TypeToText(Reparsed).ToString(),
				(Reparsed == SourceType) ? TEXT("MATCHES") : TEXT("DOES NOT MATCH")));

			Test->TestEqual(TEXT("container: no Get of the map local survives inside the function"),
				EXS_CountVariableGets(Run.Extracted, EXS_PM_MapLocal()), 0);
			UK2Node_VariableGet* CallSiteRead =
				EXS_CallSiteRead(Run.Gateway, FName(*Run.Promoted[0].Parameter));
			Test->TestTrue(TEXT("container: the gateway's map input pin is fed by a call-site read"),
				IsValid(CallSiteRead));
			if (IsValid(CallSiteRead))
			{
				UEdGraphPin* ReadValue = CallSiteRead->GetValuePin();
				Test->TestTrue(TEXT("container: and that read's value pin carries the same map type, "
					"so the wire is type-legal rather than merely present"),
					ReadValue != nullptr && ReadValue->PinType == SourceType);
			}
		}

		/** Resolve parameter-name collisions and wire the call site using the resulting name. */
		void RunNameCollision(UBlueprint& Blueprint, UFunction& Multiply)
		{
			const TCHAR* Label = TEXT("name collision");
			UEdGraph* Graph = EXS_FindGraph(&Blueprint, EXS_PM_CollisionGraph());
			if (!IsValid(Graph))
			{
				Test->AddError(TEXT("name collision: the case graph did not survive fixture setup."));
				return;
			}

			// The boundary parameter and enclosing local both request the name A.
			UK2Node_VariableGet* MemberRead = EXS_AddMemberVariableNode<UK2Node_VariableGet>(
				*Graph, EXS_PM_CollisionMember(), 0, 0);
			UK2Node_CallFunction* Inside = EXS_AddCall(*Graph, Multiply, false, 600, 0);
			UK2Node_VariableGet* LocalRead = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, *Graph, EXS_PM_CollidingLocal(), 300, 200);
			UK2Node_CallFunction* Outside = EXS_AddCall(*Graph, Multiply, false, 900, 0);

			UEdGraphPin* MemberValue = EXS_FirstDataPin(MemberRead, EGPD_Output, NAME_None);
			UEdGraphPin* LocalValue = EXS_FirstDataPin(LocalRead, EGPD_Output, NAME_None);
			UEdGraphPin* InsideA = EXS_FirstDataPin(Inside, EGPD_Input, FName(TEXT("A")));
			UEdGraphPin* InsideB = EXS_FirstDataPin(Inside, EGPD_Input, FName(TEXT("B")));
			UEdGraphPin* InsideRet = Inside->GetReturnValuePin();
			UEdGraphPin* OutsideA = EXS_FirstDataPin(Outside, EGPD_Input, FName(TEXT("A")));
			if (MemberValue == nullptr || LocalValue == nullptr || InsideA == nullptr
				|| InsideB == nullptr || InsideRet == nullptr || OutsideA == nullptr)
			{
				Test->AddError(TEXT("name collision: the fixture could not be wired."));
				return;
			}
			MemberValue->MakeLinkTo(InsideA);
			LocalValue->MakeLinkTo(InsideB);
			InsideRet->MakeLinkTo(OutsideA);
			Graph->NotifyGraphChanged();

			const FEXSPromotionRun Run = EXS_RunPromotion(*Test, Blueprint, AssetPath,
				Graph->GetName(), { Inside->NodeGuid, LocalRead->NodeGuid },
				TEXT("EXSPromotedCollision"), Label);
			if (!Run.bOk || Run.Promoted.Num() != 1)
			{
				Test->TestEqual(TEXT("name collision: exactly one promoted local"),
					Run.Promoted.Num(), 1);
				return;
			}
			const FEXSPromotedLocal& Promoted = Run.Promoted[0];

			const TArray<FString> EntryPins = EXS_EntryPinNames(Run.Entry);
			Test->TestEqual(FString::Printf(
				TEXT("name collision: the entry node carries TWO user-defined pins -- the engine's "
				     "boundary parameter and the promoted local (%s)"), *EXS_Join(EntryPins)),
				EntryPins.Num(), 2);
			Test->TestEqual(TEXT("name collision: the local kept its own name in the evidence"),
				Promoted.Variable, FString(EXS_PM_CollidingLocal()));
			Test->TestFalse(TEXT("name collision: but the PARAMETER did not take it -- the "
				"boundary pin had it first, so CreateUniquePinName suffixed instead of failing"),
				Promoted.Parameter.Equals(EXS_PM_CollidingLocal(), ESearchCase::CaseSensitive));
			// UK2Node_FunctionTerminator numbers the first collision A1 and also checks UFunction property names.
			Test->TestTrue(FString::Printf(
				TEXT("name collision: and the suffixed name is the engine's numeric form for a "
				     "function terminator, '%s1' (observed '%s')"),
				EXS_PM_CollidingLocal(), *Promoted.Parameter),
				Promoted.Parameter.Equals(FString(EXS_PM_CollidingLocal()) + TEXT("1")));
			Test->TestTrue(FString::Printf(TEXT("name collision: the boundary pin '%s' is still there"),
				EXS_PM_CollidingLocal()), EntryPins.Contains(FString(EXS_PM_CollidingLocal())));
			Test->TestTrue(TEXT("name collision: and the promoted pin is on the entry node under the "
				"name the tool reported"), EntryPins.Contains(Promoted.Parameter));

			// Wire by the allocated name rather than the requested colliding name.
			UK2Node_VariableGet* CallSiteRead = EXS_CallSiteRead(Run.Gateway, FName(*Promoted.Parameter));
			Test->TestTrue(FString::Printf(
				TEXT("name collision: the gateway's '%s' pin is fed by the new call-site read"),
				*Promoted.Parameter), IsValid(CallSiteRead));
			if (IsValid(CallSiteRead))
			{
				Test->TestEqual(TEXT("name collision: and that read names the LOCAL, not the member"),
					CallSiteRead->VariableReference.GetMemberName().ToString(),
					FString(EXS_PM_CollidingLocal()));
				Test->TestTrue(TEXT("name collision: local scope, so it resolves in the source graph"),
					CallSiteRead->VariableReference.IsLocalScope());
			}
			UK2Node_VariableGet* BoundaryFeed =
				EXS_CallSiteRead(Run.Gateway, FName(EXS_PM_CollidingLocal()));
			Test->TestTrue(TEXT("name collision: the boundary pin is still fed by the MEMBER read it "
				"was created for, so the two did not get crossed"),
				IsValid(BoundaryFeed)
				&& BoundaryFeed->VariableReference.GetMemberName().ToString()
					.Equals(EXS_PM_CollisionMember()));
		}

		/** Order locals by their earliest getter position, independently of declaration and alphabetical order. */
		void RunMultiLocalOrdering(UBlueprint& Blueprint, UFunction& Multiply)
		{
			const TCHAR* Label = TEXT("multi-local ordering");
			UEdGraph* Graph = EXS_FindGraph(&Blueprint, EXS_PM_OrderGraph());
			if (!IsValid(Graph))
			{
				Test->AddError(TEXT("multi-local ordering: the case graph did not survive setup."));
				return;
			}

			// Y order is Mike, Alpha, Zulu; declaration and alphabetical orders differ.
			UK2Node_VariableGet* Mike = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, *Graph, EXS_PM_OrderMike(), 500, 100);
			UK2Node_VariableGet* Alpha = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, *Graph, EXS_PM_OrderAlpha(), 500, 300);
			UK2Node_VariableGet* Zulu = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, *Graph, EXS_PM_OrderZulu(), 500, 900);
			UK2Node_CallFunction* First = EXS_AddCall(*Graph, Multiply, false, 800, 100);
			UK2Node_CallFunction* Second = EXS_AddCall(*Graph, Multiply, false, 1100, 100);
			UK2Node_CallFunction* Outside = EXS_AddCall(*Graph, Multiply, false, 1400, 100);

			UEdGraphPin* MikeValue = EXS_FirstDataPin(Mike, EGPD_Output, NAME_None);
			UEdGraphPin* AlphaValue = EXS_FirstDataPin(Alpha, EGPD_Output, NAME_None);
			UEdGraphPin* ZuluValue = EXS_FirstDataPin(Zulu, EGPD_Output, NAME_None);
			UEdGraphPin* FirstA = EXS_FirstDataPin(First, EGPD_Input, FName(TEXT("A")));
			UEdGraphPin* FirstB = EXS_FirstDataPin(First, EGPD_Input, FName(TEXT("B")));
			UEdGraphPin* SecondA = EXS_FirstDataPin(Second, EGPD_Input, FName(TEXT("A")));
			UEdGraphPin* SecondB = EXS_FirstDataPin(Second, EGPD_Input, FName(TEXT("B")));
			UEdGraphPin* OutsideA = EXS_FirstDataPin(Outside, EGPD_Input, FName(TEXT("A")));
			if (MikeValue == nullptr || AlphaValue == nullptr || ZuluValue == nullptr
				|| FirstA == nullptr || FirstB == nullptr || SecondA == nullptr
				|| SecondB == nullptr || OutsideA == nullptr
				|| First->GetReturnValuePin() == nullptr || Second->GetReturnValuePin() == nullptr)
			{
				Test->AddError(TEXT("multi-local ordering: the fixture could not be wired."));
				return;
			}
			MikeValue->MakeLinkTo(FirstA);
			AlphaValue->MakeLinkTo(FirstB);
			First->GetReturnValuePin()->MakeLinkTo(SecondA);
			ZuluValue->MakeLinkTo(SecondB);
			Second->GetReturnValuePin()->MakeLinkTo(OutsideA);
			Graph->NotifyGraphChanged();

			const FEXSPromotionRun Run = EXS_RunPromotion(*Test, Blueprint, AssetPath,
				Graph->GetName(),
				{ Mike->NodeGuid, Alpha->NodeGuid, Zulu->NodeGuid, First->NodeGuid, Second->NodeGuid },
				TEXT("EXSPromotedOrder"), Label);
			if (!Run.bOk)
			{
				return;
			}

			TArray<FString> ObservedOrder;
			for (const FEXSPromotedLocal& Local : Run.Promoted)
			{
				ObservedOrder.Add(Local.Variable);
			}
			const TArray<FString> ExpectedOrder = {
				FString(EXS_PM_OrderMike()), FString(EXS_PM_OrderAlpha()), FString(EXS_PM_OrderZulu()) };
			Test->TestEqual(FString::Printf(
				TEXT("multi-local ordering: THE EXACT ORDER is the NodePosY order Mike(100), "
				     "Alpha(300), Zulu(900) -- not declaration order (Zulu, Alpha, Mike) and not "
				     "alphabetical (Alpha, Mike, Zulu). Observed: %s"), *EXS_Join(ObservedOrder)),
				EXS_Join(ObservedOrder), EXS_Join(ExpectedOrder));

			if (Run.Promoted.Num() == 3)
			{
				Test->TestEqual(TEXT("multi-local ordering: Mike's sort key is its NodePosY 100"),
					Run.Promoted[0].OrderY, 100);
				Test->TestEqual(TEXT("multi-local ordering: Alpha's is 300"),
					Run.Promoted[1].OrderY, 300);
				Test->TestEqual(TEXT("multi-local ordering: Zulu's is 900"),
					Run.Promoted[2].OrderY, 900);
			}

			const TArray<FString> EntryPins = EXS_EntryPinNames(Run.Entry);
			Test->TestEqual(FString::Printf(
				TEXT("multi-local ordering: the entry node's user-defined pins are in that same "
				     "order (%s)"), *EXS_Join(EntryPins)),
				EXS_Join(EntryPins), EXS_Join(ExpectedOrder));
		}
	};
}

bool FClaireonBPEditorPromotionMatrix::RunTest(const FString& Parameters)
{
	using namespace ClaireonBPExtractSemanticsInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FEXS_PromotionMatrixCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("PromotionMatrix"))));
	return true;
}

// Build equivalent graphs in opposite node order with equal Y coordinates to exercise the X tie-breaker.
// Compare signatures case-sensitively.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorPromotionIsDeterministic,
	"Claireon.BPEditor.Promotion.IdenticalInputsProduceIdenticalSignatures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPExtractSemanticsInternal
{
	static const TCHAR* EXS_PD_GraphA()   { return TEXT("EXSPromoDetA"); }
	static const TCHAR* EXS_PD_GraphB()   { return TEXT("EXSPromoDetB"); }
	static const TCHAR* EXS_PD_Alpha()    { return TEXT("PDetAlpha"); }
	static const TCHAR* EXS_PD_Bravo()    { return TEXT("PDetBravo"); }
	static const TCHAR* EXS_PD_Charlie()  { return TEXT("PDetCharlie"); }

	/** All three Get nodes share this Y, so NodePosX is what the comparator must fall through to. */
	static constexpr int32 EXS_PD_SharedY = 500;

	class FEXS_PromotionDeterminismCommand : public FEXS_FixtureCommand
	{
	public:
		using FEXS_FixtureCommand::FEXS_FixtureCommand;

	protected:
		virtual bool BuildFixture(FString& OutError) override
		{
			UBlueprint* Blueprint = ClaireonBPEditorFixtures::Create(AssetPath, OutError);
			if (!IsValid(Blueprint))
			{
				return false;
			}

			UEdGraph* GraphA = EXS_MakeFunctionGraph(*Blueprint, EXS_PD_GraphA());
			UEdGraph* GraphB = EXS_MakeFunctionGraph(*Blueprint, EXS_PD_GraphB());
			if (!IsValid(GraphA) || !IsValid(GraphB))
			{
				OutError = TEXT("one of the two twin graphs could not be created.");
				return false;
			}

			bool bVars = true;
			for (UEdGraph* Graph : { GraphA, GraphB })
			{
				bVars = bVars
					&& FBlueprintEditorUtils::AddLocalVariable(Blueprint, Graph,
						FName(EXS_PD_Alpha()), EXS_DoubleType())
					&& FBlueprintEditorUtils::AddLocalVariable(Blueprint, Graph,
						FName(EXS_PD_Bravo()), EXS_DoubleType())
					&& FBlueprintEditorUtils::AddLocalVariable(Blueprint, Graph,
						FName(EXS_PD_Charlie()), EXS_DoubleType());
			}
			if (!bVars)
			{
				OutError = TEXT("the twin graphs' locals could not be declared.");
				return false;
			}

			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			return true;
		}

		virtual void RunBody(UBlueprint& Blueprint) override
		{
			UFunction* Multiply = UKismetMathLibrary::StaticClass()->FindFunctionByName(
				TEXT("Multiply_DoubleDouble"));
			if (Multiply == nullptr)
			{
				Test->AddError(TEXT("UKismetMathLibrary::Multiply_DoubleDouble could not be resolved."));
				return;
			}

			UEdGraph* GraphA = EXS_FindGraph(&Blueprint, EXS_PD_GraphA());
			UEdGraph* GraphB = EXS_FindGraph(&Blueprint, EXS_PD_GraphB());
			if (!IsValid(GraphA) || !IsValid(GraphB))
			{
				Test->AddError(TEXT("a twin graph did not survive fixture setup."));
				return;
			}

			int32 CharlieIndexA = INDEX_NONE;
			int32 CharlieIndexB = INDEX_NONE;
			TArray<FGuid> SelectionA;
			TArray<FGuid> SelectionB;
			if (!BuildTwin(Blueprint, *GraphA, *Multiply, /*bReversed=*/false, SelectionA, CharlieIndexA)
				|| !BuildTwin(Blueprint, *GraphB, *Multiply, /*bReversed=*/true, SelectionB, CharlieIndexB))
			{
				return;
			}

			// Verify node-list order differs as the control for the determinism test.
			Test->TestTrue(FString::Printf(
				TEXT("the twins really were built in OPPOSITE order -- Charlie's Get is at node index "
				     "%d in A and %d in B, so UEdGraph::Nodes order and the selection TSet's "
				     "pointer-hash order differ between the two runs"), CharlieIndexA, CharlieIndexB),
				CharlieIndexA != INDEX_NONE && CharlieIndexB != INDEX_NONE
				&& CharlieIndexA != CharlieIndexB);

			const FEXSPromotionRun RunA = EXS_RunPromotion(*Test, Blueprint, AssetPath,
				GraphA->GetName(), SelectionA, TEXT("EXSPromotedDetA"), TEXT("determinism A"));
			const FEXSPromotionRun RunB = EXS_RunPromotion(*Test, Blueprint, AssetPath,
				GraphB->GetName(), SelectionB, TEXT("EXSPromotedDetB"), TEXT("determinism B"));
			if (!RunA.bOk || !RunB.bOk)
			{
				return;
			}

			// The order both runs must have produced: NodePosX 100, 400, 700 at a SHARED
			// NodePosY 500.
			const TArray<FString> Expected = {
				FString(EXS_PD_Charlie()), FString(EXS_PD_Alpha()), FString(EXS_PD_Bravo()) };
			for (const TPair<const TCHAR*, const FEXSPromotionRun*>& Pair :
				{ TPair<const TCHAR*, const FEXSPromotionRun*>(TEXT("A"), &RunA),
				  TPair<const TCHAR*, const FEXSPromotionRun*>(TEXT("B"), &RunB) })
			{
				TArray<FString> Observed;
				for (const FEXSPromotedLocal& Local : Pair.Value->Promoted)
				{
					Observed.Add(Local.Variable);
					Test->TestEqual(FString::Printf(
						TEXT("run %s: '%s' was ordered on a SHARED NodePosY of %d, so NodePosX is what "
						     "separated it"), Pair.Key, *Local.Variable, EXS_PD_SharedY),
						Local.OrderY, EXS_PD_SharedY);
				}
				Test->TestEqual(FString::Printf(
					TEXT("run %s: the order is the NodePosX order Charlie(100), Alpha(400), "
					     "Bravo(700) (observed %s)"), Pair.Key, *EXS_Join(Observed)),
					EXS_Join(Observed), EXS_Join(Expected));
			}
			if (RunA.Promoted.Num() == 3)
			{
				Test->TestEqual(TEXT("run A: Charlie's NodePosX key is 100"), RunA.Promoted[0].OrderX, 100);
				Test->TestEqual(TEXT("run A: Alpha's is 400"), RunA.Promoted[1].OrderX, 400);
				Test->TestEqual(TEXT("run A: Bravo's is 700"), RunA.Promoted[2].OrderX, 700);
			}

			Test->TestTrue(FString::Printf(
				TEXT("BYTE-IDENTICAL signature_after across two runs on identical inputs built in "
				     "opposite order: A='%s' B='%s'"), *RunA.SignatureAfter, *RunB.SignatureAfter),
				EXS_BytesEqual(RunA.SignatureAfter, RunB.SignatureAfter));
			Test->TestFalse(TEXT("and the signature is not empty, so the comparison is not vacuous"),
				RunA.SignatureAfter.IsEmpty());

			const FString CanonicalA = EXS_CanonicalPromotion(RunA.Promoted);
			const FString CanonicalB = EXS_CanonicalPromotion(RunB.Promoted);
			Test->TestTrue(FString::Printf(
				TEXT("BYTE-IDENTICAL promoted_locals, including the ordering keys that decided the "
				     "signature: A='%s' B='%s'"), *CanonicalA, *CanonicalB),
				EXS_BytesEqual(CanonicalA, CanonicalB));

			const FString EntryA = EXS_EntrySignatureText(RunA.Entry);
			const FString EntryB = EXS_EntrySignatureText(RunB.Entry);
			Test->TestTrue(FString::Printf(
				TEXT("BYTE-IDENTICAL entry-node signatures read off the LIVE graphs, not the "
				     "envelope: A='%s' B='%s'"), *EntryA, *EntryB),
				EXS_BytesEqual(EntryA, EntryB));
		}

	private:
		/**
		 * The same six nodes, the same five wires, the same coordinates -- created forward or
		 * backward. Only creation order differs.
		 */
		bool BuildTwin(UBlueprint& Blueprint, UEdGraph& Graph, UFunction& Multiply, bool bReversed,
			TArray<FGuid>& OutSelection, int32& OutCharlieNodeIndex)
		{
			UK2Node_VariableGet* Charlie = nullptr;
			UK2Node_VariableGet* Alpha = nullptr;
			UK2Node_VariableGet* Bravo = nullptr;
			UK2Node_CallFunction* First = nullptr;
			UK2Node_CallFunction* Second = nullptr;
			UK2Node_CallFunction* Outside = nullptr;

			auto MakeCharlie = [&]() { Charlie = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, Graph, EXS_PD_Charlie(), 100, EXS_PD_SharedY); };
			auto MakeAlpha = [&]() { Alpha = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, Graph, EXS_PD_Alpha(), 400, EXS_PD_SharedY); };
			auto MakeBravo = [&]() { Bravo = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, Graph, EXS_PD_Bravo(), 700, EXS_PD_SharedY); };
			auto MakeFirst = [&]() { First = EXS_AddCall(Graph, Multiply, false, 1000, EXS_PD_SharedY); };
			auto MakeSecond = [&]() { Second = EXS_AddCall(Graph, Multiply, false, 1300, EXS_PD_SharedY); };
			auto MakeOutside = [&]() { Outside = EXS_AddCall(Graph, Multiply, false, 1600, EXS_PD_SharedY); };

			if (bReversed)
			{
				MakeOutside(); MakeSecond(); MakeFirst(); MakeBravo(); MakeAlpha(); MakeCharlie();
			}
			else
			{
				MakeCharlie(); MakeAlpha(); MakeBravo(); MakeFirst(); MakeSecond(); MakeOutside();
			}

			UEdGraphPin* CharlieValue = EXS_FirstDataPin(Charlie, EGPD_Output, NAME_None);
			UEdGraphPin* AlphaValue = EXS_FirstDataPin(Alpha, EGPD_Output, NAME_None);
			UEdGraphPin* BravoValue = EXS_FirstDataPin(Bravo, EGPD_Output, NAME_None);
			UEdGraphPin* FirstA = EXS_FirstDataPin(First, EGPD_Input, FName(TEXT("A")));
			UEdGraphPin* FirstB = EXS_FirstDataPin(First, EGPD_Input, FName(TEXT("B")));
			UEdGraphPin* SecondA = EXS_FirstDataPin(Second, EGPD_Input, FName(TEXT("A")));
			UEdGraphPin* SecondB = EXS_FirstDataPin(Second, EGPD_Input, FName(TEXT("B")));
			UEdGraphPin* OutsideA = EXS_FirstDataPin(Outside, EGPD_Input, FName(TEXT("A")));
			if (CharlieValue == nullptr || AlphaValue == nullptr || BravoValue == nullptr
				|| FirstA == nullptr || FirstB == nullptr || SecondA == nullptr || SecondB == nullptr
				|| OutsideA == nullptr || First->GetReturnValuePin() == nullptr
				|| Second->GetReturnValuePin() == nullptr)
			{
				Test->AddError(FString::Printf(TEXT("determinism: twin '%s' could not be wired."),
					*Graph.GetName()));
				return false;
			}
			CharlieValue->MakeLinkTo(FirstA);
			AlphaValue->MakeLinkTo(FirstB);
			First->GetReturnValuePin()->MakeLinkTo(SecondA);
			BravoValue->MakeLinkTo(SecondB);
			Second->GetReturnValuePin()->MakeLinkTo(OutsideA);
			Graph.NotifyGraphChanged();

			OutCharlieNodeIndex = Graph.Nodes.IndexOfByKey(Charlie);

			// The selection is handed over in creation order too, so the two runs differ in one
			// more place the tool could have leaked from.
			OutSelection.Reset();
			if (bReversed)
			{
				OutSelection = { Second->NodeGuid, First->NodeGuid, Bravo->NodeGuid,
					Alpha->NodeGuid, Charlie->NodeGuid };
			}
			else
			{
				OutSelection = { Charlie->NodeGuid, Alpha->NodeGuid, Bravo->NodeGuid,
					First->NodeGuid, Second->NodeGuid };
			}
			return true;
		}
	};
}

bool FClaireonBPEditorPromotionIsDeterministic::RunTest(const FString& Parameters)
{
	using namespace ClaireonBPExtractSemanticsInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FEXS_PromotionDeterminismCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("PromotionDeterminism"))));
	return true;
}

// Opt-in promotion still refuses writes, escaping reads, split pins, and non-function extraction.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorPromotionStillRefuses,
	"Claireon.BPEditor.Promotion.WritesAndReferencesStillRefuseWithTheFlagOn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPExtractSemanticsInternal
{
	static const TCHAR* EXS_PR_GraphName()  { return TEXT("EXSPromoRefuse"); }
	static const TCHAR* EXS_PR_Written()    { return TEXT("PRefWritten"); }
	static const TCHAR* EXS_PR_ReadWrite()  { return TEXT("PRefReadWrite"); }
	static const TCHAR* EXS_PR_Array()      { return TEXT("PRefArray"); }
	static const TCHAR* EXS_PR_Outside()    { return TEXT("PRefOutside"); }
	static const TCHAR* EXS_PR_EventRead()  { return TEXT("PRefEventRead"); }
	static const TCHAR* EXS_PR_Split()      { return TEXT("PRefSplit"); }

	/** The FScopedTransaction title ApplyExtract opens on the event path. */
	static const TCHAR* EXS_EventTransactionTitle() { return TEXT("Extract event"); }

	/** DEC-33's first half against an arbitrary transaction title. */
	static void EXS_ExpectNoTransactionTitled(FAutomationTestBase& Test, const FEXSTxnWindow& Window,
		const FString& Title, const TCHAR* What)
	{
		bool bMeasured = false;
		const bool bPresent = EXS_WindowHasTitle(Window, Title, bMeasured);
		if (!bMeasured)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: the transaction window could not be read, so DEC-33's 'no transaction was "
				     "opened' could not be corroborated."), What));
			return;
		}
		Test.TestFalse(FString::Printf(
			TEXT("%s: no '%s' record reached the editor's transaction buffer"), What, *Title),
			bPresent);
	}

	struct FEXSPromotionRefusalCase
	{
		FString Label;
		EEXSExtractTool Tool = EEXSExtractTool::Function;
		TArray<FGuid> Selection;

		/** Substrings the refusal message must carry. */
		TArray<FString> ExpectedMessageParts;

		/** "<variable>/<wire reason>" pairs the enclosing_locals evidence must carry. */
		TArray<FString> ExpectedPairs;
	};

	class FEXS_PromotionRefusalCommand : public FEXS_FixtureCommand
	{
	public:
		using FEXS_FixtureCommand::FEXS_FixtureCommand;

	protected:
		virtual bool BuildFixture(FString& OutError) override
		{
			UBlueprint* Blueprint = ClaireonBPEditorFixtures::Create(AssetPath, OutError);
			if (!IsValid(Blueprint))
			{
				return false;
			}
			UEdGraph* Host = EXS_MakeFunctionGraph(*Blueprint, EXS_PR_GraphName());
			if (!IsValid(Host))
			{
				OutError = TEXT("the refusal host graph could not be created.");
				return false;
			}
			const bool bLocals =
				FBlueprintEditorUtils::AddLocalVariable(Blueprint, Host,
					FName(EXS_PR_Written()), EXS_DoubleType())
				&& FBlueprintEditorUtils::AddLocalVariable(Blueprint, Host,
					FName(EXS_PR_ReadWrite()), EXS_DoubleType())
				&& FBlueprintEditorUtils::AddLocalVariable(Blueprint, Host,
					FName(EXS_PR_Array()), EXS_IntArrayType())
				&& FBlueprintEditorUtils::AddLocalVariable(Blueprint, Host,
					FName(EXS_PR_Outside()), EXS_DoubleType())
				&& FBlueprintEditorUtils::AddLocalVariable(Blueprint, Host,
					FName(EXS_PR_EventRead()), EXS_DoubleType())
				&& FBlueprintEditorUtils::AddLocalVariable(Blueprint, Host,
					FName(EXS_PR_Split()), EXS_VectorType());
			if (!bLocals)
			{
				OutError = TEXT("one of the six refusal locals could not be declared.");
				return false;
			}
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			return true;
		}

		virtual void RunBody(UBlueprint& Blueprint) override
		{
			UEdGraph* Host = EXS_FindGraph(&Blueprint, EXS_PR_GraphName());
			if (!IsValid(Host))
			{
				Test->AddError(TEXT("the refusal host graph did not survive fixture setup."));
				return;
			}

			UFunction* Multiply = UKismetMathLibrary::StaticClass()->FindFunctionByName(
				TEXT("Multiply_DoubleDouble"));
			UFunction* ArrayAdd = UKismetArrayLibrary::StaticClass()->FindFunctionByName(
				TEXT("Array_Add"));
			if (Multiply == nullptr || ArrayAdd == nullptr)
			{
				Test->AddError(TEXT("Multiply_DoubleDouble or Array_Add could not be resolved."));
				return;
			}

			// ---- explicit_write: a Set, on its own.
			UK2Node_VariableSet* WrittenSet = EXS_AddLocalVariableNode<UK2Node_VariableSet>(
				Blueprint, *Host, EXS_PR_Written(), 300, 300);

			// ---- read/write: a Get and a Set of the same local.
			UK2Node_VariableGet* ReadWriteGet = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, *Host, EXS_PR_ReadWrite(), 300, 600);
			UK2Node_VariableSet* ReadWriteSet = EXS_AddLocalVariableNode<UK2Node_VariableSet>(
				Blueprint, *Host, EXS_PR_ReadWrite(), 600, 600);

			// ---- reaches_mutable_reference_pin: DEC-15's canonical Get -> Array Add.
			UK2Node_VariableGet* ArrayGet = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, *Host, EXS_PR_Array(), 300, 900);
			UK2Node_CallFunction* AddCall = EXS_AddCall(*Host, *ArrayAdd, true, 600, 900);
			UEdGraphPin* ArrayOut = EXS_FirstDataPin(ArrayGet, EGPD_Output, NAME_None);
			UEdGraphPin* TargetArray = AddCall->FindPin(FName(TEXT("TargetArray")), EGPD_Input);
			if (ArrayOut == nullptr || TargetArray == nullptr
				|| !GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(ArrayOut, TargetArray))
			{
				Test->AddError(TEXT("the by-reference case could not be wired, so its assertions "
					"would pass vacuously."));
				return;
			}

			// A read-only getter feeds one selected and one unselected consumer.
			UK2Node_VariableGet* OutsideGet = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, *Host, EXS_PR_Outside(), 300, 1200);
			UK2Node_CallFunction* InsideConsumer = EXS_AddCall(*Host, *Multiply, false, 600, 1200);
			UK2Node_CallFunction* OutsideConsumer = EXS_AddCall(*Host, *Multiply, false, 900, 1200);
			UEdGraphPin* OutsideValue = EXS_FirstDataPin(OutsideGet, EGPD_Output, NAME_None);
			UEdGraphPin* InsideA = EXS_FirstDataPin(InsideConsumer, EGPD_Input, FName(TEXT("A")));
			UEdGraphPin* OutsideA = EXS_FirstDataPin(OutsideConsumer, EGPD_Input, FName(TEXT("A")));
			if (OutsideValue == nullptr || InsideA == nullptr || OutsideA == nullptr)
			{
				Test->AddError(TEXT("the read-reaches-outside case could not be wired."));
				return;
			}
			OutsideValue->MakeLinkTo(InsideA);
			OutsideValue->MakeLinkTo(OutsideA);

			Test->TestEqual(TEXT("read-reaches-outside: the Get's value pin really has TWO "
				"consumers, one of which will be left out of the selection"),
				OutsideValue->LinkedTo.Num(), 2);

			// ---- the kind gate: a plain read-only shape, driven through bp_extract_event.
			UK2Node_VariableGet* EventGet = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, *Host, EXS_PR_EventRead(), 300, 1500);
			UK2Node_CallFunction* EventConsumer = EXS_AddCall(*Host, *Multiply, false, 600, 1500);
			UEdGraphPin* EventValue = EXS_FirstDataPin(EventGet, EGPD_Output, NAME_None);
			UEdGraphPin* EventA = EXS_FirstDataPin(EventConsumer, EGPD_Input, FName(TEXT("A")));
			if (EventValue == nullptr || EventA == nullptr)
			{
				Test->AddError(TEXT("the kind-gate case could not be wired."));
				return;
			}
			EventValue->MakeLinkTo(EventA);

			// Split before wiring. Promotion must refuse subpin consumers because it rewires only the parent pin.
			UK2Node_VariableGet* SplitGet = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, *Host, EXS_PR_Split(), 300, 1800);
			UK2Node_CallFunction* SplitConsumer = EXS_AddCall(*Host, *Multiply, false, 600, 1800);
			UEdGraphPin* SplitValue = EXS_FirstDataPin(SplitGet, EGPD_Output, NAME_None);
			UEdGraphPin* SplitA = EXS_FirstDataPin(SplitConsumer, EGPD_Input, FName(TEXT("A")));
			if (SplitValue == nullptr || SplitA == nullptr)
			{
				Test->AddError(TEXT("the split-pin case could not be built."));
				return;
			}
			GetDefault<UEdGraphSchema_K2>()->SplitPin(SplitValue, /*bNotify=*/false);
			if (SplitValue->SubPins.Num() == 0)
			{
				Test->AddError(TEXT("SplitPin produced no subpins, so the split-pin case would "
					"assert against the wrong shape."));
				return;
			}
			SplitValue->SubPins[0]->MakeLinkTo(SplitA);
			Test->TestEqual(TEXT("split-pin: the parent value pin has no links of its own"),
				SplitValue->LinkedTo.Num(), 0);
			Test->TestEqual(TEXT("split-pin: the field subpin carries the consumer link"),
				SplitValue->SubPins[0]->LinkedTo.Num(), 1);

			Host->NotifyGraphChanged();

			TArray<FEXSPromotionRefusalCase> Cases;
			{
				FEXSPromotionRefusalCase Case;
				Case.Label = TEXT("write-only, promotion ON");
				Case.Selection = { WrittenSet->NodeGuid };
				Case.ExpectedMessageParts = { FString(TEXT("Written by a Set node")),
					FString(EXS_PR_Written()) };
				Case.ExpectedPairs = { FString(EXS_PR_Written()) + TEXT("/explicit_write") };
				Cases.Add(MoveTemp(Case));
			}
			{
				FEXSPromotionRefusalCase Case;
				Case.Label = TEXT("read/write, promotion ON");
				Case.Selection = { ReadWriteGet->NodeGuid, ReadWriteSet->NodeGuid };
				Case.ExpectedMessageParts = { FString(TEXT("Written by a Set node")),
					FString(EXS_PR_ReadWrite()) };
				// The Get and the Set are refused independently, exactly as with the flag off.
				Case.ExpectedPairs = {
					FString(EXS_PR_ReadWrite()) + TEXT("/explicit_write"),
					FString(EXS_PR_ReadWrite()) + TEXT("/read_only_reference") };
				Cases.Add(MoveTemp(Case));
			}
			{
				FEXSPromotionRefusalCase Case;
				Case.Label = TEXT("get reaching a mutable reference pin, promotion ON");
				Case.Selection = { ArrayGet->NodeGuid, AddCall->NodeGuid };
				Case.ExpectedMessageParts = { FString(TEXT("by-reference, non-const pin")),
					FString(EXS_PR_Array()) };
				Case.ExpectedPairs = {
					FString(EXS_PR_Array()) + TEXT("/reaches_mutable_reference_pin") };
				Cases.Add(MoveTemp(Case));
			}
			{
				FEXSPromotionRefusalCase Case;
				Case.Label = TEXT("read reaches outside the selection");
				Case.Selection = { OutsideGet->NodeGuid, InsideConsumer->NodeGuid };
				Case.ExpectedMessageParts = { FString(TEXT("read_reaches_outside_selection")),
					FString(EXS_PR_Outside()),
					// The evidence has to name the consumer that put it outside, or the caller
					// cannot tell which link to move.
					FString(OutsideConsumer->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)) };
				// The FINDING is still an ordinary read-only reference; the extra reason is
				// promotion's own and lives in the refusal text.
				Case.ExpectedPairs = { FString(EXS_PR_Outside()) + TEXT("/read_only_reference") };
				Cases.Add(MoveTemp(Case));
			}
			{
				FEXSPromotionRefusalCase Case;
				Case.Label = TEXT("split getter with connected field subpins");
				Case.Selection = { SplitGet->NodeGuid, SplitConsumer->NodeGuid };
				Case.ExpectedMessageParts = { FString(TEXT("split_pin_unsupported")),
					FString(EXS_PR_Split()) };
				Case.ExpectedPairs = { FString(EXS_PR_Split()) + TEXT("/read_only_reference") };
				Cases.Add(MoveTemp(Case));
			}
			{
				FEXSPromotionRefusalCase Case;
				Case.Label = TEXT("promotion asked for on a non-function kind");
				Case.Tool = EEXSExtractTool::Event;
				Case.Selection = { EventGet->NodeGuid, EventConsumer->NodeGuid };
				Case.ExpectedMessageParts = {
					FString(TEXT("honoured by bp_extract_function only")),
					FString(TEXT("bp_extract_event")) };
				Case.ExpectedPairs = { FString(EXS_PR_EventRead()) + TEXT("/read_only_reference") };
				Cases.Add(MoveTemp(Case));
			}

			for (const FEXSPromotionRefusalCase& Case : Cases)
			{
				RunCase(*Host, Case);
			}
		}

	private:
		void RunCase(UEdGraph& Host, const FEXSPromotionRefusalCase& Case)
		{
			const TCHAR* Label = *Case.Label;
			const int32 NodesBefore = Host.Nodes.Num();
			const FEXSTxnWindow Window = EXS_OpenWindow(*Test);

			const IClaireonTool::FToolResult Result = EXS_ExtractWith(
				Case.Tool, AssetPath, Host.GetName(), Case.Selection, FString(),
				/*bPromoteEnclosingLocals=*/true);

			Test->TestTrue(FString::Printf(
				TEXT("%s: refused as an error result even with promote_enclosing_locals=true"), Label),
				Result.bIsError);
			if (!Result.Data.IsValid())
			{
				Test->AddError(FString::Printf(
					TEXT("%s: the refusal returned no structured data."), Label));
				return;
			}

			EXS_ExpectString(*Test, Result.Data, TEXT("mutation_state"), TEXT("refused"), Label);
			EXS_ExpectBool(*Test, Result.Data, TEXT("mutation_retained"), false, Label);
			EXS_ExpectBool(*Test, Result.Data, TEXT("undo_record_available"), false, Label);
			EXS_ExpectString(*Test, Result.Data, TEXT("last_completed_phase"),
				kClaireonBPPhaseNone, Label);
			EXS_ExpectString(*Test, Result.Data, TEXT("quiescence_proof"),
				TEXT("by_construction"), Label);
			EXS_ExpectAbsent(*Test, Result.Data, TEXT("operation_delta"),
				TEXT("a refusal proven by construction captures no snapshot"));
			EXS_ExpectAbsent(*Test, Result.Data, TEXT("failed_phase"),
				TEXT("nothing ran, so no phase failed"));
			EXS_ExpectNoTransactionTitled(*Test, Window,
				Case.Tool == EEXSExtractTool::Event
					? FString(EXS_EventTransactionTitle()) : FString(EXS_FunctionTransactionTitle()),
				Label);
			Test->TestEqual(FString::Printf(TEXT("%s: the host graph is untouched"), Label),
				Host.Nodes.Num(), NodesBefore);

			// ASKED FOR AND NOT GRANTED is a different report from never asked. Both leave the
			// asset alone; only one means the caller has a decision to make.
			EXS_ExpectBool(*Test, Result.Data, TEXT("promotion_requested"), true, Label);
			FString Because;
			if (EXS_ReadString(*Test, Result.Data, TEXT("promotion_refused_because"), Because))
			{
				Test->TestFalse(FString::Printf(
					TEXT("%s: promotion_refused_because is not empty"), Label), Because.IsEmpty());
			}

			for (const FString& Part : Case.ExpectedMessageParts)
			{
				Test->TestTrue(FString::Printf(
					TEXT("%s: the refusal message carries '%s'"), Label, *Part),
					Result.ErrorMessage.Contains(Part));
			}

			const TArray<TSharedPtr<FJsonValue>>* Locals = nullptr;
			if (!Result.Data->TryGetArrayField(TEXT("enclosing_locals"), Locals) || Locals == nullptr)
			{
				Test->AddError(FString::Printf(
					TEXT("%s: the refusal carried no enclosing_locals evidence."), Label));
				return;
			}
			TSet<FString> ObservedPairs;
			for (const TSharedPtr<FJsonValue>& Value : *Locals)
			{
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Entry) || Entry == nullptr)
				{
					continue;
				}
				FString Variable;
				FString Reason;
				(*Entry)->TryGetStringField(TEXT("variable"), Variable);
				(*Entry)->TryGetStringField(TEXT("reason"), Reason);
				ObservedPairs.Add(Variable + TEXT("/") + Reason);
				Test->TestTrue(FString::Printf(
					TEXT("%s: the refusal message names the local '%s'"), Label, *Variable),
					Result.ErrorMessage.Contains(Variable));
			}
			for (const FString& Expected : Case.ExpectedPairs)
			{
				Test->TestTrue(FString::Printf(TEXT("%s: the evidence carries '%s'"),
					Label, *Expected), ObservedPairs.Contains(Expected));
			}
		}
	};
}

bool FClaireonBPEditorPromotionStillRefuses::RunTest(const FString& Parameters)
{
	using namespace ClaireonBPExtractSemanticsInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FEXS_PromotionRefusalCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("PromotionRefusals"))));
	return true;
}

// Save, release the session, unload, and reload to verify promoted types from disk.
// UnloadPackages resets the transaction buffer, so this test makes no undo assertions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorPromotionSurvivesReload,
	"Claireon.BPEditor.Promotion.SynthesizedSignatureSurvivesSaveAndReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPExtractSemanticsInternal
{
	static const TCHAR* EXS_RT_GraphName()  { return TEXT("EXSPromoTrip"); }
	static const TCHAR* EXS_RT_Scalar()     { return TEXT("PTripAlpha"); }
	static const TCHAR* EXS_RT_Map()        { return TEXT("PTripMap"); }
	static const TCHAR* EXS_RT_MapMember()  { return TEXT("EXSTripMapMember"); }

	/** What the pre-reload half measured, and the post-reload half must find again. */
	struct FEXSReloadExpectation
	{
		FString ParameterName;
		FString SourceVariableName;
		FEdGraphPinType ParameterType;
		FString CallSiteReadGuid;
	};

	class FEXS_PromotionReloadCommand : public IAutomationLatentCommand
	{
	public:
		FEXS_PromotionReloadCommand(FAutomationTestBase* InTest, const FString& InAssetPath)
			: Test(InTest)
			, AssetPath(InAssetPath)
		{
		}

		virtual bool Update() override
		{
			switch (Phase)
			{
			case EPhase::Build:
			{
				FString BuildError;
				if (!BuildFixture(BuildError))
				{
					Test->AddError(FString::Printf(TEXT("fixture setup failed at %s: %s"),
						*AssetPath, *BuildError));
					Phase = EPhase::Teardown;
					return false;
				}
				Phase = EPhase::Settle;
				return false;
			}

			case EPhase::Settle:
				if (++SettleUpdates < 3)
				{
					return false;
				}
				Phase = EPhase::Extract;
				return false;

			case EPhase::Extract:
			{
				UBlueprint* Blueprint = Cast<UBlueprint>(ClaireonBPEditorFixtures::Resolve(AssetPath));
				if (!IsValid(Blueprint))
				{
					Test->AddError(TEXT("the fixture is not resident, so the extraction cannot run "
						"without a load it is not allowed to perform here."));
					Phase = EPhase::Teardown;
					return false;
				}
				if (!ExtractAndSave(*Blueprint))
				{
					Phase = EPhase::Teardown;
					return false;
				}
				Phase = EPhase::Unload;
				return false;
			}

			case EPhase::Unload:
				if (!Unload())
				{
					Phase = EPhase::Teardown;
					return false;
				}
				Phase = EPhase::Verify;
				return false;

			case EPhase::Verify:
				Verify();
				Phase = EPhase::Teardown;
				return false;

			case EPhase::Teardown:
			default:
				ClaireonBPEditorFixtures::Teardown(AssetPath);
				return true;
			}
		}

	private:
		enum class EPhase : uint8
		{
			Build,
			Settle,
			Extract,
			Unload,
			Verify,
			Teardown,
		};

		bool BuildFixture(FString& OutError)
		{
			UBlueprint* Blueprint = ClaireonBPEditorFixtures::Create(AssetPath, OutError);
			if (!IsValid(Blueprint))
			{
				return false;
			}
			UEdGraph* Host = EXS_MakeFunctionGraph(*Blueprint, EXS_RT_GraphName());
			if (!IsValid(Host))
			{
				OutError = TEXT("the round-trip host graph could not be created.");
				return false;
			}
			const FEdGraphPinType MapType = EXS_NameToLinearColorMapType();
			const bool bVars =
				FBlueprintEditorUtils::AddLocalVariable(Blueprint, Host,
					FName(EXS_RT_Scalar()), EXS_DoubleType())
				&& FBlueprintEditorUtils::AddLocalVariable(Blueprint, Host,
					FName(EXS_RT_Map()), MapType)
				&& FBlueprintEditorUtils::AddMemberVariable(Blueprint,
					FName(EXS_RT_MapMember()), MapType);
			if (!bVars)
			{
				OutError = TEXT("the round-trip fixture's variables could not be declared.");
				return false;
			}
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			return true;
		}

		bool ExtractAndSave(UBlueprint& Blueprint)
		{
			UEdGraph* Host = EXS_FindGraph(&Blueprint, EXS_RT_GraphName());
			UFunction* Multiply = UKismetMathLibrary::StaticClass()->FindFunctionByName(
				TEXT("Multiply_DoubleDouble"));
			if (!IsValid(Host) || Multiply == nullptr)
			{
				Test->AddError(TEXT("round trip: the host graph or Multiply_DoubleDouble is missing."));
				return false;
			}

			UK2Node_VariableGet* ScalarGet = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, *Host, EXS_RT_Scalar(), 300, 0);
			UK2Node_CallFunction* Inside = EXS_AddCall(*Host, *Multiply, false, 600, 0);
			UK2Node_CallFunction* Outside = EXS_AddCall(*Host, *Multiply, false, 900, 0);
			UK2Node_VariableGet* MapGet = EXS_AddLocalVariableNode<UK2Node_VariableGet>(
				Blueprint, *Host, EXS_RT_Map(), 300, 300);
			UK2Node_VariableSet* SetMember = EXS_AddMemberVariableNode<UK2Node_VariableSet>(
				*Host, EXS_RT_MapMember(), 600, 300);

			UEdGraphPin* ScalarValue = EXS_FirstDataPin(ScalarGet, EGPD_Output, NAME_None);
			UEdGraphPin* InsideA = EXS_FirstDataPin(Inside, EGPD_Input, FName(TEXT("A")));
			UEdGraphPin* OutsideA = EXS_FirstDataPin(Outside, EGPD_Input, FName(TEXT("A")));
			UEdGraphPin* MapValue = EXS_FirstDataPin(MapGet, EGPD_Output, NAME_None);
			UEdGraphPin* SetValue = SetMember->FindPin(FName(EXS_RT_MapMember()), EGPD_Input);
			UEdGraphPin* SetExec = SetMember->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
			UEdGraphPin* HostThen = EXS_HostThenPin(Host);
			if (ScalarValue == nullptr || InsideA == nullptr || OutsideA == nullptr
				|| MapValue == nullptr || SetValue == nullptr || SetExec == nullptr
				|| HostThen == nullptr || Inside->GetReturnValuePin() == nullptr)
			{
				Test->AddError(TEXT("round trip: the fixture could not be wired."));
				return false;
			}
			ScalarValue->MakeLinkTo(InsideA);
			Inside->GetReturnValuePin()->MakeLinkTo(OutsideA);
			HostThen->MakeLinkTo(SetExec);
			if (!GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(MapValue, SetValue))
			{
				Test->AddError(TEXT("round trip: the schema refused the map wire."));
				return false;
			}
			MapValue = EXS_FirstDataPin(MapGet, EGPD_Output, NAME_None);
			ScalarValue = EXS_FirstDataPin(ScalarGet, EGPD_Output, NAME_None);
			if (MapValue == nullptr || ScalarValue == nullptr)
			{
				Test->AddError(TEXT("round trip: a Get's value pin disappeared on connect."));
				return false;
			}
			const FEdGraphPinType ScalarType = ScalarValue->PinType;
			const FEdGraphPinType MapPinType = MapValue->PinType;
			Host->NotifyGraphChanged();

			const FEXSPromotionRun Run = EXS_RunPromotion(*Test, Blueprint, AssetPath,
				Host->GetName(),
				{ ScalarGet->NodeGuid, Inside->NodeGuid, MapGet->NodeGuid, SetMember->NodeGuid },
				TEXT("EXSPromotedRoundTrip"), TEXT("round trip"));
			if (!Run.bOk)
			{
				return false;
			}
			if (Run.Promoted.Num() != 2)
			{
				Test->AddError(FString::Printf(
					TEXT("round trip: expected two promoted locals, got %d."), Run.Promoted.Num()));
				return false;
			}

			// Ordered by NodePosY: the scalar Get at 0, the map Get at 300.
			Test->TestEqual(TEXT("round trip: the scalar local is the first parameter"),
				Run.Promoted[0].Variable, FString(EXS_RT_Scalar()));
			Test->TestEqual(TEXT("round trip: the map local is the second"),
				Run.Promoted[1].Variable, FString(EXS_RT_Map()));

			ExtractedGraphName = Run.ExtractedGraphName;
			GatewayGuid = Run.Gateway->NodeGuid;
			SignatureAfter = Run.SignatureAfter;
			EntrySignatureBeforeReload = EXS_EntrySignatureText(Run.Entry);

			Expectations.Reset();
			for (int32 Index = 0; Index < Run.Promoted.Num(); ++Index)
			{
				FEXSReloadExpectation Expectation;
				Expectation.ParameterName = Run.Promoted[Index].Parameter;
				Expectation.SourceVariableName = Run.Promoted[Index].Variable;
				Expectation.ParameterType = (Index == 0) ? ScalarType : MapPinType;
				Expectation.CallSiteReadGuid = Run.Promoted[Index].CallSiteReadNode;
				Expectations.Add(MoveTemp(Expectation));
			}

			// Saved BEFORE the unload, or UnloadPackages refuses a dirty package and the reload
			// would read a file that never carried the signature.
			if (!ClaireonBPEditorFixtures::Save(&Blueprint))
			{
				Test->AddError(TEXT("round trip: the fixture package could not be saved, so there is "
					"nothing on disk to reload."));
				return false;
			}
			return true;
		}

		/** Drop the in-memory copy so the next read genuinely comes off disk. */
		bool Unload()
		{
			// Release the session's Blueprint reference before unloading; UnloadPackages closes editors.
			FClaireonSessionManager::Get().ReleaseByAssetPath(AssetPath);

			UPackage* Package = FindPackage(nullptr, *AssetPath);
			if (!IsValid(Package))
			{
				Test->AddError(TEXT("round trip: the fixture package is not resident before the "
					"unload, which means the extraction ran somewhere this test cannot see."));
				return false;
			}

			TArray<UPackage*> ToUnload;
			ToUnload.Add(Package);
			UPackageTools::FUnloadPackageParams UnloadParams(ToUnload);
			UnloadParams.bUnloadDirtyPackages = true;
			UPackageTools::UnloadPackages(UnloadParams);
			CollectGarbage(RF_NoFlags);

			// Require the package to be absent before reloading so assertions cannot reuse resident objects.
			const bool bGone = FindPackage(nullptr, *AssetPath) == nullptr;
			Test->TestTrue(FString::Printf(
				TEXT("round trip: the package is genuinely unloaded before anything reloads it (%s)"),
				UnloadParams.OutErrorMessage.IsEmpty()
					? TEXT("no error reported") : *UnloadParams.OutErrorMessage.ToString()),
				bGone);
			return bGone;
		}

		void Verify()
		{
			const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
			UBlueprint* Reloaded = LoadObject<UBlueprint>(nullptr, *ObjectPath);
			if (!IsValid(Reloaded))
			{
				Test->AddError(TEXT("round trip: the fixture did not reload from disk."));
				return;
			}

			UEdGraph* Extracted = EXS_FindGraph(Reloaded, ExtractedGraphName);
			if (!IsValid(Extracted))
			{
				Test->AddError(FString::Printf(
					TEXT("round trip: the reloaded Blueprint has no graph named '%s'."),
					*ExtractedGraphName));
				return;
			}
			UK2Node_FunctionEntry* Entry = EXS_FindEntry(Extracted);
			if (!IsValid(Entry))
			{
				Test->AddError(TEXT("round trip: the reloaded extracted graph has no function entry."));
				return;
			}

			const FString EntryAfterReload = EXS_EntrySignatureText(Entry);
			Test->TestTrue(FString::Printf(
				TEXT("round trip: the entry node's signature is BYTE-IDENTICAL after a save, an "
				     "unload and a reload from disk (before '%s', after '%s')"),
				*EntrySignatureBeforeReload, *EntryAfterReload),
				EXS_BytesEqual(EntrySignatureBeforeReload, EntryAfterReload));

			for (const FEXSReloadExpectation& Expectation : Expectations)
			{
				const FString What = FString::Printf(TEXT("round trip: parameter '%s'"),
					*Expectation.ParameterName);
				UEdGraphPin* EntryPin = Entry->FindPin(FName(*Expectation.ParameterName), EGPD_Output);
				if (EntryPin == nullptr)
				{
					Test->AddError(FString::Printf(
						TEXT("%s did not survive the reload; the entry node carries %s."),
						*What, *EXS_Join(EXS_EntryPinNames(Entry))));
					continue;
				}
				EXS_ExpectPinTypeEquals(*Test, *What, EntryPin->PinType, Expectation.ParameterType);

				Test->TestEqual(FString::Printf(
					TEXT("%s: no VariableGet of '%s' reappeared inside the extracted function"),
					*What, *Expectation.SourceVariableName),
					EXS_CountVariableGets(Extracted, Expectation.SourceVariableName), 0);
			}

			UEdGraph* Source = EXS_FindGraph(Reloaded, EXS_RT_GraphName());
			UEdGraphNode* Gateway = EXS_FindNode(Source, GatewayGuid);
			if (!IsValid(Gateway))
			{
				Test->AddError(TEXT("round trip: the gateway call node did not survive the reload."));
				return;
			}

			for (const FEXSReloadExpectation& Expectation : Expectations)
			{
				UK2Node_VariableGet* CallSiteRead =
					EXS_CallSiteRead(Gateway, FName(*Expectation.ParameterName));
				if (!IsValid(CallSiteRead))
				{
					Test->AddError(FString::Printf(
						TEXT("round trip: after the reload the gateway's '%s' input pin is fed by no "
						     "VariableGet, so the call-site wiring did not survive serialization."),
						*Expectation.ParameterName));
					continue;
				}
				Test->TestEqual(FString::Printf(
					TEXT("round trip: the reloaded call-site read for '%s' names the same local"),
					*Expectation.ParameterName),
					CallSiteRead->VariableReference.GetMemberName().ToString(),
					Expectation.SourceVariableName);
				Test->TestTrue(FString::Printf(
					TEXT("round trip: the reloaded read for '%s' is still LOCAL scope on the source "
					     "graph"), *Expectation.ParameterName),
					CallSiteRead->VariableReference.IsLocalScope()
					&& CallSiteRead->VariableReference.GetMemberScopeName()
						.Equals(EXS_RT_GraphName()));
				Test->TestEqual(FString::Printf(
					TEXT("round trip: the read for '%s' is the SAME node the extraction reported, "
					     "by guid"), *Expectation.ParameterName),
					CallSiteRead->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
					Expectation.CallSiteReadGuid);
			}
		}

		FAutomationTestBase* Test = nullptr;
		FString AssetPath;
		EPhase Phase = EPhase::Build;
		int32 SettleUpdates = 0;

		FString ExtractedGraphName;
		FGuid GatewayGuid;
		FString SignatureAfter;
		FString EntrySignatureBeforeReload;
		TArray<FEXSReloadExpectation> Expectations;
	};
}

bool FClaireonBPEditorPromotionSurvivesReload::RunTest(const FString& Parameters)
{
	using namespace ClaireonBPExtractSemanticsInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FEXS_PromotionReloadCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("PromotionRoundTrip"))));
	return true;
}

// Event extraction allocates an island slot, translates the body and internal reroutes rigidly,
// and leaves the gateway at the original selection. Derive expectations from the pre-call graph.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorEventPlacesTheThreeThings,
	"Claireon.BPEditor.ExtractionSemantics.EventExtractionPlacesTheThreeThingsApart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPExtractSemanticsInternal
{
	static const TCHAR* EXS_EP_EntryName() { return TEXT("EXSPlacementEntry"); }
	static const TCHAR* EXS_EP_EventName() { return TEXT("EXSPlacedEvent"); }

	/** Then -> Execute, for the fixture's exec chain. False when either pin is missing. */
	static bool EXS_LinkExec(UEdGraphNode* From, UEdGraphNode* To)
	{
		UEdGraphPin* Then = IsValid(From)
			? From->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output) : nullptr;
		UEdGraphPin* Exec = IsValid(To)
			? To->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input) : nullptr;
		if (Then == nullptr || Exec == nullptr)
		{
			return false;
		}
		Then->MakeLinkTo(Exec);
		return true;
	}

	/** Track positions by GUID because skeleton regeneration and compilation may replace nodes. */
	struct FEXSPlacedBodyNode
	{
		FGuid Guid;
		int32 BeforeX = 0;
		int32 BeforeY = 0;
		int32 AfterX = 0;
		int32 AfterY = 0;
	};

	class FEXS_EventPlacementCommand : public FEXS_FixtureCommand
	{
	public:
		using FEXS_FixtureCommand::FEXS_FixtureCommand;

	protected:
		virtual bool BuildFixture(FString& OutError) override
		{
			UBlueprint* Blueprint = ClaireonBPEditorFixtures::Create(AssetPath, OutError);
			if (!IsValid(Blueprint))
			{
				return false;
			}
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			return true;
		}

		virtual void RunBody(UBlueprint& Blueprint) override
		{
			UEdGraph* Graph = ClaireonBPEditorFixtures::FirstUbergraph(&Blueprint);
			if (!IsValid(Graph))
			{
				Test->AddError(TEXT("the fixture has no ubergraph page, so there is nowhere a "
					"Custom Event could be created."));
				return;
			}

			UFunction* PrintString = UKismetSystemLibrary::StaticClass()->FindFunctionByName(
				TEXT("PrintString"));
			if (PrintString == nullptr)
			{
				Test->AddError(TEXT("PrintString could not be resolved, so the fixture's exec chain "
					"cannot be built."));
				return;
			}

			// Use irregular body offsets so a per-node relayout cannot masquerade as rigid translation.
			UK2Node_CustomEvent* ChainEntry = NewObject<UK2Node_CustomEvent>(Graph);
			Graph->AddNode(ChainEntry, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			ChainEntry->CreateNewGuid();
			ChainEntry->CustomFunctionName = FName(EXS_EP_EntryName());
			ChainEntry->NodePosX = 0;
			ChainEntry->NodePosY = 0;
			ChainEntry->AllocateDefaultPins();

			UK2Node_CallFunction* Caller = EXS_AddCall(*Graph, *PrintString, false, 400, 0);
			const TArray<UK2Node_CallFunction*> Members = {
				EXS_AddCall(*Graph, *PrintString, false, 900, 100),
				EXS_AddCall(*Graph, *PrintString, false, 1300, 260),
				EXS_AddCall(*Graph, *PrintString, false, 1750, 60) };

			// Include an internal reroute in the request and verify it moves despite selection filtering.
			// Connect through the schema so wildcard exec pins resolve.
			UK2Node_Knot* Reroute = NewObject<UK2Node_Knot>(Graph);
			Graph->AddNode(Reroute, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			Reroute->CreateNewGuid();
			Reroute->NodePosX = 1120;
			Reroute->NodePosY = 180;
			Reroute->AllocateDefaultPins();

			const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
			UEdGraphPin* BodyOneThen = Members[0]->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			UEdGraphPin* BodyTwoExec = Members[1]->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
			const bool bRerouteWired = BodyOneThen != nullptr && BodyTwoExec != nullptr
				&& Reroute->GetInputPin() != nullptr && Reroute->GetOutputPin() != nullptr
				&& Schema->TryCreateConnection(BodyOneThen, Reroute->GetInputPin())
				&& Schema->TryCreateConnection(Reroute->GetOutputPin(), BodyTwoExec);

			if (!EXS_LinkExec(ChainEntry, Caller)
				|| !EXS_LinkExec(Caller, Members[0])
				|| !bRerouteWired
				|| !EXS_LinkExec(Members[1], Members[2]))
			{
				Test->AddError(TEXT("the fixture's exec chain could not be wired, so the selection "
					"would have no single entry edge and the extraction would refuse for a reason "
					"this test is not about."));
				return;
			}
			Graph->NotifyGraphChanged();

			const FString GraphName = Graph->GetName();
			const int32 PreRailX = FMath::RoundToInt32(ClaireonGraphIslands::ResolveRailX(Graph));
			int32 PreMaxY = MIN_int32;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (IsValid(Node))
				{
					PreMaxY = FMath::Max(PreMaxY, Node->NodePosY);
				}
			}

			TArray<FEXSPlacedBodyNode> Body;
			TArray<FGuid> SelectionGuids;
			int32 SelectionMinX = MAX_int32;
			int32 SelectionMinY = MAX_int32;
			for (UK2Node_CallFunction* Member : Members)
			{
				if (!IsValid(Member))
				{
					Test->AddError(TEXT("one of the three body nodes was not created."));
					return;
				}
				FEXSPlacedBodyNode Record;
				Record.Guid = Member->NodeGuid;
				Record.BeforeX = Member->NodePosX;
				Record.BeforeY = Member->NodePosY;
				Body.Add(Record);
				SelectionGuids.Add(Member->NodeGuid);
				SelectionMinX = FMath::Min(SelectionMinX, Member->NodePosX);
				SelectionMinY = FMath::Min(SelectionMinY, Member->NodePosY);
			}

			// Exclude the reroute from the expected selection bounds, matching the tool's filter.
			FEXSPlacedBodyNode RerouteRecord;
			RerouteRecord.Guid = Reroute->NodeGuid;
			RerouteRecord.BeforeX = Reroute->NodePosX;
			RerouteRecord.BeforeY = Reroute->NodePosY;
			Body.Add(RerouteRecord);
			SelectionGuids.Add(Reroute->NodeGuid);

			const IClaireonTool::FToolResult Result = EXS_ExtractWith(EEXSExtractTool::Event,
				AssetPath, GraphName, SelectionGuids, EXS_EP_EventName(),
				/*bPromoteEnclosingLocals=*/false);
			if (Result.bIsError)
			{
				Test->AddError(FString::Printf(
					TEXT("the event extraction failed, so nothing was placed to measure: %s "
					     "(validation_failures: %s)"),
					*Result.ErrorMessage, *EXS_ValidationFailureText(Result.Data)));
				return;
			}

			UEdGraph* After = EXS_FindGraph(&Blueprint, GraphName);
			if (!IsValid(After))
			{
				Test->AddError(TEXT("the ubergraph did not survive the extraction, so no position "
					"can be read."));
				return;
			}

			UK2Node_CustomEvent* Placed = nullptr;
			for (UEdGraphNode* Node : After->Nodes)
			{
				UK2Node_CustomEvent* Candidate = Cast<UK2Node_CustomEvent>(Node);
				if (IsValid(Candidate) && Candidate->CustomFunctionName == FName(EXS_EP_EventName()))
				{
					Placed = Candidate;
					break;
				}
			}
			if (!IsValid(Placed))
			{
				Test->AddError(FString::Printf(
					TEXT("no Custom Event named '%s' is in the graph after a successful extraction."),
					EXS_EP_EventName()));
				return;
			}

			// Require a slot strictly below existing nodes, measured before adding the event.
			Test->TestTrue(FString::Printf(
				TEXT("the Custom Event's Y (%d) is strictly below the pre-call max NodePosY (%d)"),
				Placed->NodePosY, PreMaxY), Placed->NodePosY > PreMaxY);

			Test->TestEqual(TEXT("the Custom Event's X is the pre-call rail"),
				Placed->NodePosX, PreRailX);

			FString GatewayText;
			FGuid GatewayGuid;
			if (!EXS_ReadString(*Test, Result.Data, TEXT("gateway_node"), GatewayText)
				|| !FGuid::Parse(GatewayText, GatewayGuid))
			{
				Test->AddError(TEXT("the result named no parseable gateway node, so its position "
					"cannot be checked."));
				return;
			}
			UEdGraphNode* Gateway = EXS_FindNode(After, GatewayGuid);
			if (!IsValid(Gateway))
			{
				Test->AddError(TEXT("the reported gateway node is not in the source graph."));
				return;
			}
			Test->TestEqual(TEXT("the gateway's X is the pre-call selection's left edge"),
				Gateway->NodePosX, SelectionMinX);
			Test->TestEqual(TEXT("the gateway's Y is the pre-call selection's top edge"),
				Gateway->NodePosY, SelectionMinY);

			// Compare every pairwise offset; bounding-box equality cannot prove rigid translation.
			for (FEXSPlacedBodyNode& Record : Body)
			{
				UEdGraphNode* Moved = EXS_FindNode(After, Record.Guid);
				if (!IsValid(Moved))
				{
					Test->AddError(FString::Printf(
						TEXT("body node %s is no longer in the graph, so the translation cannot be "
						     "measured."),
						*Record.Guid.ToString(EGuidFormats::DigitsWithHyphens)));
					return;
				}
				Record.AfterX = Moved->NodePosX;
				Record.AfterY = Moved->NodePosY;
			}
			for (int32 I = 0; I < Body.Num(); ++I)
			{
				for (int32 J = I + 1; J < Body.Num(); ++J)
				{
					Test->TestEqual(FString::Printf(
						TEXT("body pair %d/%d kept its x offset"), I, J),
						Body[J].AfterX - Body[I].AfterX, Body[J].BeforeX - Body[I].BeforeX);
					Test->TestEqual(FString::Printf(
						TEXT("body pair %d/%d kept its y offset"), I, J),
						Body[J].AfterY - Body[I].AfterY, Body[J].BeforeY - Body[I].BeforeY);
				}
			}

			// Assert alignment below the event without duplicating the tool's spacing constant.
			int32 BodyMinX = MAX_int32;
			int32 BodyMinY = MAX_int32;
			for (const FEXSPlacedBodyNode& Record : Body)
			{
				BodyMinX = FMath::Min(BodyMinX, Record.AfterX);
				BodyMinY = FMath::Min(BodyMinY, Record.AfterY);
			}
			Test->TestEqual(TEXT("the moved body's left edge is the event's own X"),
				BodyMinX, Placed->NodePosX);
			Test->TestTrue(FString::Printf(
				TEXT("the moved body's top (%d) is strictly below the event (%d)"),
				BodyMinY, Placed->NodePosY), BodyMinY > Placed->NodePosY);

			// Use MIN_int32 defaults so missing position fields fail the report comparison.
			Test->TestEqual(TEXT("the result's event_node_pos_x is the event's actual X"),
				EXS_ReadInt(Result.Data, TEXT("event_node_pos_x"), MIN_int32), Placed->NodePosX);
			Test->TestEqual(TEXT("the result's event_node_pos_y is the event's actual Y"),
				EXS_ReadInt(Result.Data, TEXT("event_node_pos_y"), MIN_int32), Placed->NodePosY);
			Test->TestEqual(TEXT("the result's gateway_pos_x is the gateway's actual X"),
				EXS_ReadInt(Result.Data, TEXT("gateway_pos_x"), MIN_int32), Gateway->NodePosX);
			Test->TestEqual(TEXT("the result's gateway_pos_y is the gateway's actual Y"),
				EXS_ReadInt(Result.Data, TEXT("gateway_pos_y"), MIN_int32), Gateway->NodePosY);
			Test->TestEqual(TEXT("the result counted every moved node, reroute included"),
				EXS_ReadInt(Result.Data, TEXT("body_nodes_moved"), MIN_int32), Body.Num());
			Test->TestEqual(TEXT("the reroute is counted separately as a carried knot"),
				EXS_ReadInt(Result.Data, TEXT("body_knots_moved"), MIN_int32), 1);
			Test->TestEqual(TEXT("the reroute was dropped from the selection, not extracted"),
				EXS_ReadInt(Result.Data, TEXT("reroutes_ignored"), MIN_int32), 1);
		}
	};
}

bool FClaireonBPEditorEventPlacesTheThreeThings::RunTest(const FString& Parameters)
{
	using namespace ClaireonBPExtractSemanticsInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FEXS_EventPlacementCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("EventPlacement"))));
	return true;
}

// A single-entry cyclic selection must retain its internal backedge when the external boundary is replaced.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorEventKeepsInternalLoopback,
	"Claireon.BPEditor.ExtractionSemantics.EventExtractionKeepsTheInternalLoopback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPExtractSemanticsInternal
{
	class FEXS_EventLoopbackCommand : public FEXS_FixtureCommand
	{
	public:
		using FEXS_FixtureCommand::FEXS_FixtureCommand;

	protected:
		virtual bool BuildFixture(FString& OutError) override
		{
			UBlueprint* Blueprint = ClaireonBPEditorFixtures::Create(AssetPath, OutError);
			if (!IsValid(Blueprint))
			{
				return false;
			}
			FKismetEditorUtilities::CompileBlueprint(Blueprint);
			return true;
		}

		virtual void RunBody(UBlueprint& Blueprint) override
		{
			UEdGraph* Graph = ClaireonBPEditorFixtures::FirstUbergraph(&Blueprint);
			if (!IsValid(Graph))
			{
				Test->AddError(TEXT("the fixture has no ubergraph page."));
				return;
			}

			UFunction* DelayFn = UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("Delay"));
			UFunction* PrintString = UKismetSystemLibrary::StaticClass()->FindFunctionByName(
				TEXT("PrintString"));
			if (DelayFn == nullptr || PrintString == nullptr)
			{
				Test->AddError(TEXT("Delay or PrintString could not be resolved."));
				return;
			}

			// entry -> Branch; Branch.Then -> Delay -> BACK into Branch; Branch.Else -> tail.
			// No crossing data edges (the condition stays on its default), one distinct
			// incoming exec target, everything else terminal inside the selection.
			UK2Node_CustomEvent* ChainEntry = NewObject<UK2Node_CustomEvent>(Graph);
			Graph->AddNode(ChainEntry, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			ChainEntry->CreateNewGuid();
			ChainEntry->CustomFunctionName = FName(TEXT("EXSLoopChainEntry"));
			ChainEntry->NodePosX = 0;
			ChainEntry->NodePosY = 0;
			ChainEntry->AllocateDefaultPins();

			UK2Node_IfThenElse* Branch = NewObject<UK2Node_IfThenElse>(Graph);
			Graph->AddNode(Branch, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			Branch->CreateNewGuid();
			Branch->NodePosX = 400;
			Branch->NodePosY = 0;
			Branch->AllocateDefaultPins();

			UK2Node_CallFunction* Delay = EXS_AddCall(*Graph, *DelayFn, false, 800, -100);
			UK2Node_CallFunction* Tail = EXS_AddCall(*Graph, *PrintString, false, 800, 200);

			UEdGraphPin* BranchExec = Branch->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
			UEdGraphPin* BranchThen = Branch->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			UEdGraphPin* BranchElse = Branch->FindPin(UEdGraphSchema_K2::PN_Else, EGPD_Output);
			UEdGraphPin* EntryThen = ChainEntry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			UEdGraphPin* DelayExec = Delay->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
			UEdGraphPin* DelayThen = Delay->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			UEdGraphPin* TailExec = Tail->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
			if (!BranchExec || !BranchThen || !BranchElse || !EntryThen
				|| !DelayExec || !DelayThen || !TailExec)
			{
				Test->AddError(TEXT("the loopback fixture's pins could not all be found."));
				return;
			}
			EntryThen->MakeLinkTo(BranchExec);
			BranchThen->MakeLinkTo(DelayExec);
			DelayThen->MakeLinkTo(BranchExec);   // THE BACKEDGE.
			BranchElse->MakeLinkTo(TailExec);
			Graph->NotifyGraphChanged();

			// PRECONDITION, asserted: the entry pin holds exactly the two inbound links the
			// scenario is about -- one external, one internal.
			Test->TestEqual(TEXT("the Branch input carries the external entry AND the backedge"),
				BranchExec->LinkedTo.Num(), 2);

			const FGuid BranchGuid = Branch->NodeGuid;
			const FGuid DelayGuid = Delay->NodeGuid;
			const IClaireonTool::FToolResult Result = EXS_ExtractWith(EEXSExtractTool::Event,
				AssetPath, Graph->GetName(), {BranchGuid, DelayGuid, Tail->NodeGuid},
				TEXT("EXSLoopedEvent"), /*bPromoteEnclosingLocals=*/false);

			Test->TestFalse(FString::Printf(
				TEXT("the single-entry cyclic body extracts without error (%s)"),
				*Result.ErrorMessage), Result.bIsError);
			if (Result.bIsError)
			{
				return;
			}

			// Re-resolved by GUID: the extraction force-regenerates the skeleton and compiles.
			UK2Node_IfThenElse* BranchAfter = nullptr;
			UK2Node_CallFunction* DelayAfter = nullptr;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!IsValid(Node))
				{
					continue;
				}
				if (Node->NodeGuid == BranchGuid)
				{
					BranchAfter = Cast<UK2Node_IfThenElse>(Node);
				}
				else if (Node->NodeGuid == DelayGuid)
				{
					DelayAfter = Cast<UK2Node_CallFunction>(Node);
				}
			}
			if (!IsValid(BranchAfter) || !IsValid(DelayAfter))
			{
				Test->AddError(TEXT("the Branch or Delay did not survive the extraction."));
				return;
			}

			UEdGraphPin* ExecAfter = BranchAfter->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
			UEdGraphPin* DelayThenAfter = DelayAfter->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			if (!ExecAfter || !DelayThenAfter)
			{
				Test->AddError(TEXT("the surviving nodes lost their exec pins."));
				return;
			}

			// THE POINT: the internal backedge survived the boundary rewiring.
			Test->TestTrue(TEXT("the Delay still loops back into the Branch input"),
				ExecAfter->LinkedTo.Contains(DelayThenAfter));

			// And the new event's Then coexists with it -- the entry pin was rewired, not
			// stripped.
			bool bEventFeedsBranch = false;
			for (const UEdGraphPin* Linked : ExecAfter->LinkedTo)
			{
				const UEdGraphNode* Owner = Linked ? Linked->GetOwningNodeUnchecked() : nullptr;
				if (Owner && Owner->IsA<UK2Node_CustomEvent>()
					&& CastChecked<UK2Node_CustomEvent>(Owner)->CustomFunctionName
						== FName(TEXT("EXSLoopedEvent")))
				{
					bEventFeedsBranch = true;
				}
			}
			Test->TestTrue(TEXT("the extracted event's Then reaches the Branch input alongside "
			                    "the surviving backedge"), bEventFeedsBranch);
			Test->TestEqual(TEXT("the Branch input holds exactly the event link and the backedge"),
				ExecAfter->LinkedTo.Num(), 2);
		}
	};
}

bool FClaireonBPEditorEventKeepsInternalLoopback::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPExtractSemanticsInternal;
	ADD_LATENT_AUTOMATION_COMMAND(FEXS_EventLoopbackCommand(
		this, ClaireonBPEditorFixtures::UniquePath(TEXT("EventLoopback"))));
	return true;
}

#endif // WITH_CLAIREON_TESTS && WITH_EDITOR
