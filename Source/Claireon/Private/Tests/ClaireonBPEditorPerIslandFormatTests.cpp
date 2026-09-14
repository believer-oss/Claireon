// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Interactive per-island formatting tests; BlueprintAssist measures live widgets.
// Share and reshape one fixture because BA ticks only its active graph handler.
// After reshaping, rebuild open editors and yield frames before formatting can paint them.
// Compare knot-transparent connectivity: successful formatting may add reroute nodes.

#include "Misc/AutomationTest.h"

#if WITH_CLAIREON_TESTS && WITH_EDITOR

#include "Tests/ClaireonBASettleHelper.h"
#include "Tests/ClaireonBPEditorFixtures.h"

#include "ClaireonBlueprintHelpers.h"
#include "ClaireonExecTopology.h"
#include "ClaireonGraphIslands.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "GraphEditor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Knot.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tools/ClaireonBPSnapshot.h"
#include "Tools/ClaireonBlueprintGraphTool_Format.h"
#include "Tools/ClaireonBlueprintGraphTool_Open.h"
#include "Tools/ClaireonBlueprintGraphTool_Selection.h"

namespace ClaireonBPEditorPerIslandFormatInternal
{
	// Prefix helpers to avoid unity-build collisions.

	static FString PIF_FixturePath()
	{
		return ClaireonBPEditorFixtures::SharedBlueprintAssistFixturePath();
	}

	static FString PIF_Guid(const FGuid& Value)
	{
		return Value.ToString(EGuidFormats::DigitsWithHyphens);
	}

	/** What one island of the fixture is made of. */
	struct FPIF_IslandSpec
	{
		/** 1 for a singleton, 2 for an event->call pair. */
		int32 NodeCount = 2;

		/** Insert a reroute between the event and the call. Needs NodeCount == 2. */
		bool bWithKnot = false;

		/**
		 * Add a second call after the first and feed both InString pins from one pure
		 * MakeLiteralString through a single data reroute: one source pin, one knot, two
		 * consumers. Needs NodeCount == 2 and !bWithKnot.
		 */
		bool bWithDataKnotFanOut = false;
	};

	/** The GUIDs one built island ended up with. */
	struct FPIF_Island
	{
		TArray<FGuid> Members;
		FGuid EventGuid;
		FGuid CallGuid;
		FGuid KnotGuid;
		FGuid SecondCallGuid;
		FGuid LiteralGuid;

		/** The plan-time representative: the lexicographically lowest member GUID. */
		FString Representative() const
		{
			TArray<FString> Ids;
			for (const FGuid& Member : Members)
			{
				Ids.Add(PIF_Guid(Member));
			}
			Ids.Sort();
			return Ids.Num() > 0 ? Ids[0] : FString();
		}
	};

	struct FPIF_Shape
	{
		TArray<FPIF_Island> Islands;
		FGuid CommentGuid;

		/** Islands in the order bp_format will execute them: ascending representative. */
		TArray<int32> PlanOrder() const
		{
			TArray<int32> Order;
			for (int32 I = 0; I < Islands.Num(); ++I)
			{
				Order.Add(I);
			}
			Order.Sort([this](int32 A, int32 B)
			{
				return Islands[A].Representative() < Islands[B].Representative();
			});
			return Order;
		}
	};

	/**
	 * Set RF_Transactional explicitly so undo records node changes.
	 * Space islands 600 units apart so a comment can cover exactly two rows.
	 */
	static bool PIF_BuildShape(UBlueprint& Blueprint, UEdGraph& Graph,
		const TArray<FPIF_IslandSpec>& Specs, bool bSpanningComment,
		FPIF_Shape& Out, FString& OutError)
	{
		Out = FPIF_Shape();

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

		UFunction* PrintString =
			UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString"));
		if (PrintString == nullptr)
		{
			OutError = TEXT("UKismetSystemLibrary::PrintString could not be resolved, so no fixture "
				"island can be built. That is an engine-side change, not a fixture problem.");
			return false;
		}

		for (int32 Index = 0; Index < Specs.Num(); ++Index)
		{
			const FPIF_IslandSpec& Spec = Specs[Index];
			const int32 Row = Index * 600;
			FPIF_Island Island;

			if (Spec.NodeCount == 1)
			{
				// A lone call: one island, one member, and the singleton policy's target.
				UK2Node_CallFunction* Lone =
					NewObject<UK2Node_CallFunction>(&Graph, NAME_None, RF_Transactional);
				Graph.AddNode(Lone, /*bFromUI=*/false, /*bSelectNewNode=*/false);
				Lone->CreateNewGuid();
				Lone->SetFromFunction(PrintString);
				Lone->NodePosX = 0;
				Lone->NodePosY = Row;
				Lone->AllocateDefaultPins();
				Island.CallGuid = Lone->NodeGuid;
				Island.Members.Add(Lone->NodeGuid);
				Out.Islands.Add(MoveTemp(Island));
				continue;
			}

			UK2Node_CustomEvent* Event =
				NewObject<UK2Node_CustomEvent>(&Graph, NAME_None, RF_Transactional);
			Graph.AddNode(Event, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			Event->CreateNewGuid();
			Event->CustomFunctionName = *FString::Printf(TEXT("PIF_Entry_%d"), Index);
			Event->NodePosX = 0;
			Event->NodePosY = Row;
			Event->AllocateDefaultPins();

			// Set the member reference before allocating call pins.
			UK2Node_CallFunction* Call =
				NewObject<UK2Node_CallFunction>(&Graph, NAME_None, RF_Transactional);
			Graph.AddNode(Call, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			Call->CreateNewGuid();
			Call->SetFromFunction(PrintString);
			Call->NodePosX = 400;
			Call->NodePosY = Row;
			Call->AllocateDefaultPins();

			UEdGraphPin* Then = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			UEdGraphPin* Exec = Call->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
			if (Then == nullptr || Exec == nullptr)
			{
				OutError = FString::Printf(TEXT("island %d could not be wired: %s pin missing."),
					Index, Then == nullptr ? TEXT("event Then") : TEXT("call Execute"));
				return false;
			}

			Island.EventGuid = Event->NodeGuid;
			Island.CallGuid = Call->NodeGuid;
			Island.Members.Add(Event->NodeGuid);
			Island.Members.Add(Call->NodeGuid);

			if (Spec.bWithKnot)
			{
				// Connect the incoming knot pin first to resolve its wildcard type.
				UK2Node_Knot* Knot = NewObject<UK2Node_Knot>(&Graph, NAME_None, RF_Transactional);
				Graph.AddNode(Knot, /*bFromUI=*/false, /*bSelectNewNode=*/false);
				Knot->CreateNewGuid();
				Knot->NodePosX = 200;
				Knot->NodePosY = Row;
				Knot->AllocateDefaultPins();

				UEdGraphPin* KnotIn = Knot->GetInputPin();
				UEdGraphPin* KnotOut = Knot->GetOutputPin();
				if (KnotIn == nullptr || KnotOut == nullptr)
				{
					OutError = FString::Printf(TEXT("island %d's reroute has no pins."), Index);
					return false;
				}
				Then->MakeLinkTo(KnotIn);
				KnotOut->MakeLinkTo(Exec);

				Island.KnotGuid = Knot->NodeGuid;
				Island.Members.Add(Knot->NodeGuid);
			}
			else
			{
				Then->MakeLinkTo(Exec);
			}

			if (Spec.bWithDataKnotFanOut)
			{
				UFunction* MakeLiteralString = UKismetSystemLibrary::StaticClass()
					->FindFunctionByName(TEXT("MakeLiteralString"));
				if (MakeLiteralString == nullptr)
				{
					OutError = TEXT("UKismetSystemLibrary::MakeLiteralString could not be resolved.");
					return false;
				}

				UK2Node_CallFunction* SecondCall =
					NewObject<UK2Node_CallFunction>(&Graph, NAME_None, RF_Transactional);
				Graph.AddNode(SecondCall, /*bFromUI=*/false, /*bSelectNewNode=*/false);
				SecondCall->CreateNewGuid();
				SecondCall->SetFromFunction(PrintString);
				SecondCall->NodePosX = 800;
				SecondCall->NodePosY = Row;
				SecondCall->AllocateDefaultPins();

				UK2Node_CallFunction* Literal =
					NewObject<UK2Node_CallFunction>(&Graph, NAME_None, RF_Transactional);
				Graph.AddNode(Literal, /*bFromUI=*/false, /*bSelectNewNode=*/false);
				Literal->CreateNewGuid();
				Literal->SetFromFunction(MakeLiteralString);
				Literal->NodePosX = 0;
				Literal->NodePosY = Row + 200;
				Literal->AllocateDefaultPins();

				UK2Node_Knot* DataKnot = NewObject<UK2Node_Knot>(&Graph, NAME_None, RF_Transactional);
				Graph.AddNode(DataKnot, /*bFromUI=*/false, /*bSelectNewNode=*/false);
				DataKnot->CreateNewGuid();
				DataKnot->NodePosX = 200;
				DataKnot->NodePosY = Row + 200;
				DataKnot->AllocateDefaultPins();

				UEdGraphPin* FirstThen = Call->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
				UEdGraphPin* SecondExec = SecondCall->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
				UEdGraphPin* FirstInString = Call->FindPin(TEXT("InString"), EGPD_Input);
				UEdGraphPin* SecondInString = SecondCall->FindPin(TEXT("InString"), EGPD_Input);
				UEdGraphPin* LiteralOut = Literal->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Output);
				UEdGraphPin* DataKnotIn = DataKnot->GetInputPin();
				UEdGraphPin* DataKnotOut = DataKnot->GetOutputPin();
				if (!FirstThen || !SecondExec || !FirstInString || !SecondInString || !LiteralOut
					|| !DataKnotIn || !DataKnotOut)
				{
					OutError = FString::Printf(TEXT("island %d's data fan-out could not be wired: a pin is missing."), Index);
					return false;
				}

				FirstThen->MakeLinkTo(SecondExec);

				// Resolve the wildcard reroute from its source before fanning out.
				LiteralOut->MakeLinkTo(DataKnotIn);
				DataKnot->NotifyPinConnectionListChanged(DataKnotIn);
				DataKnotOut->MakeLinkTo(FirstInString);
				DataKnotOut->MakeLinkTo(SecondInString);
				DataKnot->NotifyPinConnectionListChanged(DataKnotOut);

				Island.SecondCallGuid = SecondCall->NodeGuid;
				Island.LiteralGuid = Literal->NodeGuid;
				Island.KnotGuid = DataKnot->NodeGuid;
				Island.Members.Add(SecondCall->NodeGuid);
				Island.Members.Add(Literal->NodeGuid);
				Island.Members.Add(DataKnot->NodeGuid);
			}

			Out.Islands.Add(MoveTemp(Island));
		}

		if (bSpanningComment)
		{
			if (Out.Islands.Num() < 2)
			{
				OutError = TEXT("a spanning comment needs at least two islands to span.");
				return false;
			}

			// Sized to cover rows 0 and 1 and nothing else, so exactly two islands are
			// spanned however many the shape has.
			UEdGraphNode_Comment* Comment =
				NewObject<UEdGraphNode_Comment>(&Graph, NAME_None, RF_Transactional);
			Graph.AddNode(Comment, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			Comment->CreateNewGuid();
			Comment->NodePosX = -100;
			Comment->NodePosY = -100;
			Comment->NodeWidth = 900;
			Comment->NodeHeight = 900;
			Comment->NodeComment = TEXT("PIF spanning comment");

			for (int32 IslandIndex = 0; IslandIndex < 2; ++IslandIndex)
			{
				for (const FGuid& Member : Out.Islands[IslandIndex].Members)
				{
					for (UEdGraphNode* Node : Graph.Nodes)
					{
						if (IsValid(Node) && Node->NodeGuid == Member)
						{
							Comment->AddNodeUnderComment(Node);
						}
					}
				}
			}
			Out.CommentGuid = Comment->NodeGuid;
		}

		return true;
	}


	static FString PIF_Str(const IClaireonTool::FToolResult& R, const TCHAR* Field)
	{
		FString Value;
		if (R.Data.IsValid())
		{
			R.Data->TryGetStringField(Field, Value);
		}
		return Value;
	}

	static bool PIF_Bool(const IClaireonTool::FToolResult& R, const TCHAR* Field, bool bDefault = false)
	{
		bool Value = bDefault;
		if (R.Data.IsValid())
		{
			R.Data->TryGetBoolField(Field, Value);
		}
		return Value;
	}

	static bool PIF_HasField(const IClaireonTool::FToolResult& R, const TCHAR* Field)
	{
		return R.Data.IsValid() && R.Data->HasField(Field);
	}

	static int32 PIF_Int(const IClaireonTool::FToolResult& R, const TCHAR* Field, int32 Default = -1)
	{
		double Value = 0.0;
		if (R.Data.IsValid() && R.Data->TryGetNumberField(Field, Value))
		{
			return static_cast<int32>(Value);
		}
		return Default;
	}

	/** Join values for readable array-comparison failures. */
	static FString PIF_Join(const TArray<FString>& Values)
	{
		return Values.Num() > 0 ? FString::Join(Values, TEXT("|")) : FString(TEXT("<empty>"));
	}

	static TArray<FString> PIF_StringArray(const IClaireonTool::FToolResult& R, const TCHAR* Field)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (R.Data.IsValid() && R.Data->TryGetArrayField(Field, Values) && Values)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Item;
				if (Value.IsValid() && Value->TryGetString(Item))
				{
					Out.Add(Item);
				}
			}
		}
		return Out;
	}

	/** One island_outcomes row, by representative. */
	static TSharedPtr<FJsonObject> PIF_Row(const IClaireonTool::FToolResult& R,
		const FString& Representative)
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!R.Data.IsValid() || !R.Data->TryGetArrayField(TEXT("island_outcomes"), Rows) || !Rows)
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Rows)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object)
			{
				continue;
			}
			FString Rep;
			if ((*Object)->TryGetStringField(TEXT("representative"), Rep) && Rep == Representative)
			{
				return *Object;
			}
		}
		return nullptr;
	}

	static FString PIF_RowOutcome(const IClaireonTool::FToolResult& R, const FString& Representative)
	{
		TSharedPtr<FJsonObject> Row = PIF_Row(R, Representative);
		FString Outcome;
		if (Row.IsValid())
		{
			Row->TryGetStringField(TEXT("outcome"), Outcome);
		}
		return Outcome;
	}

	static FString PIF_RowField(const IClaireonTool::FToolResult& R, const FString& Representative,
		const TCHAR* Field)
	{
		TSharedPtr<FJsonObject> Row = PIF_Row(R, Representative);
		FString Value;
		if (Row.IsValid())
		{
			Row->TryGetStringField(Field, Value);
		}
		return Value;
	}


	static IClaireonTool::FToolResult PIF_Format(const FString& SessionId,
		const TArray<FString>& IslandGuids, bool bAllIslands)
	{
		ClaireonBlueprintGraphTool_Format Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetBoolField(TEXT("report_delta"), true);
		if (!bAllIslands)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const FString& Guid : IslandGuids)
			{
				Values.Add(MakeShared<FJsonValueString>(Guid));
			}
			Args->SetArrayField(TEXT("island_guids"), Values);
		}
		return Tool.Execute(Args);
	}

	/** Positions of every node currently in the graph, by GUID. */
	static TMap<FGuid, FIntPoint> PIF_Positions(const UEdGraph& Graph)
	{
		TMap<FGuid, FIntPoint> Out;
		for (const UEdGraphNode* Node : Graph.Nodes)
		{
			if (IsValid(Node))
			{
				Out.Add(Node->NodeGuid, FIntPoint(Node->NodePosX, Node->NodePosY));
			}
		}
		return Out;
	}

	/** Record each output-to-input edge once for exact structural recovery checks. */
	static TSet<FString> PIF_Edges(const UEdGraph& Graph)
	{
		TSet<FString> Out;
		for (const UEdGraphNode* Node : Graph.Nodes)
		{
			if (!IsValid(Node))
			{
				continue;
			}
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output)
				{
					continue;
				}
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					const UEdGraphNode* Other = Linked ? Linked->GetOwningNode() : nullptr;
					if (!IsValid(Other))
					{
						continue;
					}
					Out.Add(FString::Printf(TEXT("%s:%s->%s:%s"),
						*Node->NodeGuid.ToString(), *Pin->PinName.ToString(),
						*Other->NodeGuid.ToString(), *Linked->PinName.ToString()));
				}
			}
		}
		return Out;
	}

	/** True when both edge sets hold exactly the same wires. */
	static bool PIF_SameEdges(const TSet<FString>& A, const TSet<FString>& B)
	{
		return A.Num() == B.Num() && A.Includes(B);
	}

	/**
	 * Every LinkedTo entry whose partner does not link back. A non-empty result is a
	 * corrupt graph: the compiler ensures in UEdGraphPin::ResolveReferencesToPin and
	 * fails with an internal compiler error.
	 */
	static TArray<FString> PIF_AsymmetricLinks(const UEdGraph& Graph)
	{
		TArray<FString> Out;
		for (const UEdGraphNode* Node : Graph.Nodes)
		{
			if (!IsValid(Node))
			{
				continue;
			}
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (!Linked || !Linked->LinkedTo.Contains(Pin))
					{
						const UEdGraphNode* Other = Linked ? Linked->GetOwningNode() : nullptr;
						Out.Add(FString::Printf(TEXT("%s.%s -> %s.%s"),
							*Node->GetName(), *Pin->PinName.ToString(),
							IsValid(Other) ? *Other->GetName() : TEXT("<null>"),
							Linked ? *Linked->PinName.ToString() : TEXT("<null>")));
					}
				}
			}
		}
		return Out;
	}

	/** Collapse knot chains when comparing semantics because formatting can add reroutes. */
	static TArray<FString> PIF_SemanticKeys(const UEdGraph& Graph, const TSet<FGuid>& Members)
	{
		TArray<FString> Keys;
		for (const FClaireonCollapsedEdge& Edge :
			ClaireonExecTopology::CollapseEdges(&Graph, /*bIncludeDirect=*/true))
		{
			if (!IsValid(Edge.FromNode) || !IsValid(Edge.ToNode) || !Edge.FromPin || !Edge.ToPin)
			{
				continue;
			}
			if (!Members.Contains(Edge.FromNode->NodeGuid) || !Members.Contains(Edge.ToNode->NodeGuid))
			{
				continue;
			}
			Keys.Add(FString::Printf(TEXT("%s.%s->%s.%s"),
				*PIF_Guid(Edge.FromNode->NodeGuid), *Edge.FromPin->PinName.ToString(),
				*PIF_Guid(Edge.ToNode->NodeGuid), *Edge.ToPin->PinName.ToString()));
		}
		Keys.Sort();
		return Keys;
	}

	static TSet<FGuid> PIF_NonKnotMembers(const UEdGraph& Graph, const FPIF_Island& Island)
	{
		TSet<FGuid> Out;
		for (const FGuid& Member : Island.Members)
		{
			for (const UEdGraphNode* Node : Graph.Nodes)
			{
				if (IsValid(Node) && Node->NodeGuid == Member && !Node->IsA<UK2Node_Knot>())
				{
					Out.Add(Member);
				}
			}
		}
		return Out;
	}


	/** Yield real frames after rebuilding the graph so Slate panels and BA handlers become ready. */
	class FPIF_LatentCommand : public IAutomationLatentCommand
	{
	public:
		explicit FPIF_LatentCommand(FAutomationTestBase* InTest)
			: Test(InTest)
		{
		}

		virtual bool Update() override
		{
			switch (Phase)
			{
			case EPhase::Ensure:
			{
				const FString AssetPath = PIF_FixturePath();
				if (!IsValid(ClaireonBPEditorFixtures::Resolve(AssetPath)))
				{
					FString CreateError;
					UBlueprint* Created = ClaireonBPEditorFixtures::Create(AssetPath, CreateError);
					if (!IsValid(Created))
					{
						Test->AddError(FString::Printf(
							TEXT("could not create the shared fixture at %s: %s"),
							*AssetPath, *CreateError));
						return true;
					}
					// Save before opening the editor in a later update.
					if (!ClaireonBPEditorFixtures::Save(Created))
					{
						Test->AddWarning(FString::Printf(
							TEXT("the shared fixture at %s could not be saved; it will be left as an "
								 "unsaved dirty package, which raises the Restore-Packages modal on "
								 "the next editor launch."), *AssetPath));
					}
				}
				Phase = EPhase::Idle;
				return false;
			}

			case EPhase::Idle:
				// Unwind creation and save before editor opening to avoid reentrant loading.
				if (++IdleUpdates < 3)
				{
					return false;
				}
				Phase = EPhase::Open;
				return false;

			case EPhase::Open:
			{
				UObject* Asset = ClaireonBPEditorFixtures::Resolve(PIF_FixturePath());
				UAssetEditorSubsystem* Subsystem = IsValid(GEditor)
					? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
					: nullptr;
				if (!IsValid(Asset) || !IsValid(Subsystem))
				{
					Test->AddError(TEXT("the shared fixture or the asset-editor subsystem went away."));
					return true;
				}
				// Synchronous, and bShowProgressWindow=false so a test never raises a dialog.
				Subsystem->OpenEditorForAsset(Asset, EToolkitMode::Standalone,
					TSharedPtr<IToolkitHost>(), /*bShowProgressWindow=*/false);
				Phase = EPhase::Shape;
				return false;
			}

			case EPhase::Shape:
			{
				UBlueprint* Blueprint = Cast<UBlueprint>(ClaireonBPEditorFixtures::Resolve(PIF_FixturePath()));
				UEdGraph* Graph = ClaireonBPEditorFixtures::FirstUbergraph(Blueprint);
				if (!IsValid(Blueprint) || !IsValid(Graph))
				{
					Test->AddError(TEXT("the shared fixture has no ubergraph to shape."));
					return true;
				}

				FString ShapeError;
				if (!PIF_BuildShape(*Blueprint, *Graph, Specs(), WantsSpanningComment(),
					Shape, ShapeError))
				{
					Test->AddError(FString::Printf(TEXT("fixture shaping failed: %s"), *ShapeError));
					return true;
				}

				// Rebuild editor widgets after replacing their nodes.
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				Phase = EPhase::Settle;
				SettleUpdates = 0;
				PrimeUpdates = 0;
				return false;
			}

			case EPhase::Settle:
			{
				// Yield frames for the panel rebuild before pumping Slate.
				if (++SettleUpdates < 6)
				{
					return false;
				}

				// Activate and queue the graph tab, then yield frames for BA handler creation.
				UEdGraph* Graph = ClaireonBPEditorFixtures::FirstUbergraph(
					Cast<UBlueprint>(ClaireonBPEditorFixtures::Resolve(PIF_FixturePath())));
				if (IsValid(Graph) && ClaireonBASettleHelper::IsAvailable())
				{
					// Use short priming budgets; bp_format handles node-size and position settlement.
					ClaireonBASettleHelper::FSettleOptions Options;
					Options.HandlerTimeoutSeconds = 1.0;
					Options.NodeSizeTimeoutSeconds = 1.0;
					Options.QuiesceTimeoutSeconds = 1.0;
					Options.RequiredStableRounds = 1;
					const ClaireonBASettleHelper::FSettleReport Report =
						ClaireonBASettleHelper::Settle(Graph, Options);
					if (Report.bIntendedGraphConfirmed)
					{
						Phase = EPhase::Body;
						return false;
					}
					LastPrimeDiagnostics = Report.Diagnostics;
				}

				// On priming timeout, run the body so it reports the tool refusal with diagnostics.
				if (++PrimeUpdates >= 30)
				{
					Test->AddInfo(FString::Printf(
						TEXT("BlueprintAssist was not primed within %d updates; the body runs "
							 "anyway. Last priming diagnostics: %s"),
						PrimeUpdates, *LastPrimeDiagnostics));
					Phase = EPhase::Body;
				}
				return false;
			}

			case EPhase::Body:
			{
				UBlueprint* Blueprint = Cast<UBlueprint>(ClaireonBPEditorFixtures::Resolve(PIF_FixturePath()));
				UEdGraph* Graph = ClaireonBPEditorFixtures::FirstUbergraph(Blueprint);
				if (!IsValid(Blueprint) || !IsValid(Graph))
				{
					Test->AddError(TEXT("the fixture went away before the body could run."));
					return true;
				}

				if (SessionId.IsEmpty())
				{
					ClaireonBlueprintGraphTool_Open OpenTool;
					TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
					Args->SetStringField(TEXT("asset_path"), PIF_FixturePath());
					const IClaireonTool::FToolResult Opened = OpenTool.Execute(Args);
					if (Opened.bIsError || !Opened.Data.IsValid()
						|| !Opened.Data->TryGetStringField(TEXT("session_id"), SessionId))
					{
						Test->AddError(FString::Printf(TEXT("bp_open failed: %s"), *Opened.ErrorMessage));
						Phase = EPhase::Teardown;
						return false;
					}
				}

				const bool bWantsAnotherPass = RunBody(*Blueprint, *Graph, PassIndex);
				++PassIndex;

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

				if (!bWantsAnotherPass)
				{
					Phase = EPhase::Teardown;
					return false;
				}

				// Reset between determinism passes; preserve output between idempotence passes.
				if (WantsReshapeBetweenPasses())
				{
					Phase = EPhase::Shape;
					return false;
				}
				Phase = EPhase::Settle;
				SettleUpdates = 0;
				PrimeUpdates = 0;
				return false;
			}

			case EPhase::Teardown:
			default:
				// Keep the shared editor fixture for later tests and save it for out-of-band cleanup.
				return true;
			}
		}

	protected:
		/** The island shape this test wants. Re-read before every pass. */
		virtual TArray<FPIF_IslandSpec> Specs() const = 0;

		virtual bool WantsSpanningComment() const { return false; }

		/** Reshape for independent determinism runs; disable to test idempotence on the previous output. */
		virtual bool WantsReshapeBetweenPasses() const { return true; }

		/** Return true to run another pass (re-shaping first unless WantsReshapeBetweenPasses is false). */
		virtual bool RunBody(UBlueprint& Blueprint, UEdGraph& Graph, int32 InPassIndex) = 0;

		FAutomationTestBase* Test = nullptr;
		FString SessionId;
		FPIF_Shape Shape;
		int32 PassIndex = 0;

	private:
		enum class EPhase : uint8 { Ensure, Idle, Open, Shape, Settle, Body, Teardown };
		EPhase Phase = EPhase::Ensure;
		int32 IdleUpdates = 0;
		int32 SettleUpdates = 0;
		int32 PrimeUpdates = 0;
		FString LastPrimeDiagnostics = TEXT("<none>");
	};
}

using namespace ClaireonBPEditorPerIslandFormatInternal;

// Complete coverage.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonPIFCoverageComplete,
	"Claireon.BPEditor.PerIslandFormat.CoverageCompleteOnAllRequestedIslands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorPerIslandFormatInternal
{
	class FPIF_CompleteCommand : public FPIF_LatentCommand
	{
	public:
		using FPIF_LatentCommand::FPIF_LatentCommand;

	protected:
		virtual TArray<FPIF_IslandSpec> Specs() const override
		{
			return {FPIF_IslandSpec{2, false}, FPIF_IslandSpec{2, false}};
		}

		virtual bool RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph, int32 /*Pass*/) override
		{
			const IClaireonTool::FToolResult R = PIF_Format(SessionId, {}, /*bAllIslands=*/true);

			Test->TestEqual(TEXT("coverage is complete when every requested island formatted"),
				PIF_Str(R, TEXT("format_status")), FString(TEXT("complete")));
			Test->TestEqual(TEXT("mutation is applied_clean"),
				PIF_Str(R, TEXT("mutation_state")), FString(TEXT("applied_clean")));
			Test->TestFalse(TEXT("a clean format is not an error"), R.bIsError);
			Test->TestEqual(TEXT("both islands are reported requested"),
				PIF_Int(R, TEXT("islands_requested")), 2);
			Test->TestEqual(TEXT("both islands formatted"),
				PIF_Int(R, TEXT("islands_formatted")), 2);

			for (const FPIF_Island& Island : Shape.Islands)
			{
				Test->TestEqual(
					FString::Printf(TEXT("island %s reports formatted"), *Island.Representative()),
					PIF_RowOutcome(R, Island.Representative()), FString(TEXT("formatted")));
			}

			// engine_compile_status is ABSENT, never a sentinel: bp_format never compiles.
			Test->TestFalse(TEXT("no compile ran, so engine_compile_status is absent"),
				PIF_HasField(R, TEXT("engine_compile_status")));

			Test->TestTrue(TEXT("the format moved at least one node"),
				PIF_Int(R, TEXT("nodes_moved")) > 0);

			for (const FPIF_Island& Island : Shape.Islands)
			{
				const TSet<FGuid> Members = PIF_NonKnotMembers(Graph, Island);
				Test->TestEqual(
					FString::Printf(TEXT("island %s kept exactly one knot-transparent edge"),
						*Island.Representative()),
					PIF_SemanticKeys(Graph, Members).Num(), 1);
			}
			return false;
		}
	};
}

bool FClaireonPIFCoverageComplete::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FPIF_CompleteCommand(this));
	return true;
}

// Policy skips reduce coverage without failing mutation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonPIFPartialClean,
	"Claireon.BPEditor.PerIslandFormat.SingletonSkipMakesPartialWithAppliedClean",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorPerIslandFormatInternal
{
	class FPIF_PartialCleanCommand : public FPIF_LatentCommand
	{
	public:
		using FPIF_LatentCommand::FPIF_LatentCommand;

	protected:
		virtual TArray<FPIF_IslandSpec> Specs() const override
		{
			return {FPIF_IslandSpec{2, false}, FPIF_IslandSpec{1, false}};
		}

		virtual bool RunBody(UBlueprint& /*Blueprint*/, UEdGraph& /*Graph*/, int32 /*Pass*/) override
		{
			const IClaireonTool::FToolResult R = PIF_Format(SessionId, {}, /*bAllIslands=*/true);

			Test->TestEqual(TEXT("a policy skip makes coverage partial"),
				PIF_Str(R, TEXT("format_status")), FString(TEXT("partial")));
			Test->TestEqual(TEXT("but the mutation is still applied_clean"),
				PIF_Str(R, TEXT("mutation_state")), FString(TEXT("applied_clean")));
			Test->TestFalse(TEXT("and it is NOT an error"), R.bIsError);

			int32 SingletonIndex = INDEX_NONE;
			int32 PairIndex = INDEX_NONE;
			for (int32 I = 0; I < Shape.Islands.Num(); ++I)
			{
				if (Shape.Islands[I].Members.Num() == 1)
				{
					SingletonIndex = I;
				}
				else
				{
					PairIndex = I;
				}
			}
			if (SingletonIndex == INDEX_NONE || PairIndex == INDEX_NONE)
			{
				Test->AddError(TEXT("the fixture did not produce one singleton and one pair."));
				return false;
			}

			const FString SingletonRep = Shape.Islands[SingletonIndex].Representative();
			Test->TestEqual(TEXT("the singleton is skipped by policy"),
				PIF_RowOutcome(R, SingletonRep), FString(TEXT("skipped_by_policy")));
			Test->TestEqual(TEXT("and the policy is named as the singleton rule"),
				PIF_RowField(R, SingletonRep, TEXT("policy_reason")), FString(TEXT("singleton")));
			Test->TestEqual(TEXT("the pair still formatted"),
				PIF_RowOutcome(R, Shape.Islands[PairIndex].Representative()),
				FString(TEXT("formatted")));

			// Coverage uses requested islands as its denominator.
			Test->TestEqual(TEXT("both islands were requested"),
				PIF_Int(R, TEXT("islands_requested")), 2);
			Test->TestEqual(TEXT("only the pair was eligible"),
				PIF_StringArray(R, TEXT("eligible_islands")).Num(), 1);
			return false;
		}
	};
}

bool FClaireonPIFPartialClean::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FPIF_PartialCleanCommand(this));
	return true;
}

// An empty eligible set reports all_targets_excluded, not complete.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonPIFAllExcluded,
	"Claireon.BPEditor.PerIslandFormat.AllSingletonRequestIsRefusedNotVacuouslyComplete",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorPerIslandFormatInternal
{
	class FPIF_AllExcludedCommand : public FPIF_LatentCommand
	{
	public:
		using FPIF_LatentCommand::FPIF_LatentCommand;

	protected:
		virtual TArray<FPIF_IslandSpec> Specs() const override
		{
			return {FPIF_IslandSpec{1, false}, FPIF_IslandSpec{1, false}, FPIF_IslandSpec{2, false}};
		}

		virtual bool RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph, int32 /*Pass*/) override
		{
			TArray<FString> SingletonGuids;
			for (const FPIF_Island& Island : Shape.Islands)
			{
				if (Island.Members.Num() == 1)
				{
					SingletonGuids.Add(PIF_Guid(Island.Members[0]));
				}
			}
			if (SingletonGuids.Num() != 2)
			{
				Test->AddError(TEXT("the fixture did not produce two singletons."));
				return false;
			}

			const TMap<FGuid, FIntPoint> Before = PIF_Positions(Graph);
			const IClaireonTool::FToolResult R =
				PIF_Format(SessionId, SingletonGuids, /*bAllIslands=*/false);

			// Report pre-plan refusals separately because they have no format_status.
			if (!PIF_HasField(R, TEXT("format_status")))
			{
				Test->AddError(FString::Printf(
					TEXT("bp_format refused before it built a plan, so there is no coverage to "
						 "assert. refusal_reason='%s' mutation_state='%s' message='%s'"),
					*PIF_Str(R, TEXT("refusal_reason")),
					*PIF_Str(R, TEXT("mutation_state")),
					*R.ErrorMessage.Left(400)));
				return false;
			}

			Test->TestNotEqual(TEXT("an all-excluded request is NOT reported complete"),
				PIF_Str(R, TEXT("format_status")), FString(TEXT("complete")));
			Test->TestEqual(
				FString::Printf(TEXT("it is all_targets_excluded (saw '%s', %d requested, %d eligible)"),
					*PIF_Str(R, TEXT("format_status")),
					PIF_Int(R, TEXT("islands_requested")),
					PIF_StringArray(R, TEXT("eligible_islands")).Num()),
				PIF_Str(R, TEXT("format_status")), FString(TEXT("all_targets_excluded")));
			Test->TestEqual(TEXT("and the mutation axis says refused"),
				PIF_Str(R, TEXT("mutation_state")), FString(TEXT("refused")));
			Test->TestFalse(TEXT("a refusal retains nothing"),
				PIF_Bool(R, TEXT("mutation_retained"), true));

			const TMap<FGuid, FIntPoint> After = PIF_Positions(Graph);
			Test->TestEqual(TEXT("a refusal added no nodes"), After.Num(), Before.Num());
			bool bAnyMoved = false;
			for (const TPair<FGuid, FIntPoint>& Pair : Before)
			{
				const FIntPoint* Now = After.Find(Pair.Key);
				if (!Now || *Now != Pair.Value)
				{
					bAnyMoved = true;
				}
			}
			Test->TestFalse(TEXT("a refusal moved nothing"), bAnyMoved);

			for (const FPIF_Island& Island : Shape.Islands)
			{
				if (Island.Members.Num() == 2)
				{
					Test->TestNull(TEXT("an unrequested island gets no row"),
						PIF_Row(R, Island.Representative()).Get());
				}
			}
			return false;
		}
	};
}

bool FClaireonPIFAllExcluded::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FPIF_AllExcludedCommand(this));
	return true;
}

// Comment containment is decided before mutation.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonPIFSpanningComment,
	"Claireon.BPEditor.PerIslandFormat.SpanningCommentSkipsBeforeMutating",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorPerIslandFormatInternal
{
	class FPIF_SpanningCommentCommand : public FPIF_LatentCommand
	{
	public:
		using FPIF_LatentCommand::FPIF_LatentCommand;

	protected:
		virtual TArray<FPIF_IslandSpec> Specs() const override
		{
			return {FPIF_IslandSpec{2, false}, FPIF_IslandSpec{2, false}};
		}

		virtual bool WantsSpanningComment() const override { return true; }

		virtual bool RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph, int32 /*Pass*/) override
		{
			const TMap<FGuid, FIntPoint> Before = PIF_Positions(Graph);
			const IClaireonTool::FToolResult R = PIF_Format(SessionId, {}, /*bAllIslands=*/true);

			Test->TestEqual(TEXT("every island is spanned, so every target is excluded"),
				PIF_Str(R, TEXT("format_status")), FString(TEXT("all_targets_excluded")));
			Test->TestEqual(TEXT("nothing was dispatched, so the mutation axis says refused"),
				PIF_Str(R, TEXT("mutation_state")), FString(TEXT("refused")));

			const FString ExpectedComment = PIF_Guid(Shape.CommentGuid);
			for (const FPIF_Island& Island : Shape.Islands)
			{
				const FString Rep = Island.Representative();
				Test->TestEqual(FString::Printf(TEXT("island %s is skipped by policy"), *Rep),
					PIF_RowOutcome(R, Rep), FString(TEXT("skipped_by_policy")));
				Test->TestEqual(TEXT("the policy is the spanning-comment rule"),
					PIF_RowField(R, Rep, TEXT("policy_reason")), FString(TEXT("spanning_comment")));
				Test->TestEqual(TEXT("and the offending comment is named"),
					PIF_RowField(R, Rep, TEXT("comment_guid")), ExpectedComment);
			}

			// Verify positions remain unchanged on a preflight skip.
			const TMap<FGuid, FIntPoint> After = PIF_Positions(Graph);
			bool bAnyMoved = false;
			for (const TPair<FGuid, FIntPoint>& Pair : Before)
			{
				const FIntPoint* Now = After.Find(Pair.Key);
				if (!Now || *Now != Pair.Value)
				{
					bAnyMoved = true;
				}
			}
			Test->TestFalse(TEXT("a spanning-comment skip moved nothing at all"), bAnyMoved);
			Test->TestEqual(TEXT("and added nothing"), After.Num(), Before.Num());
			return false;
		}
	};
}

bool FClaireonPIFSpanningComment::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FPIF_SpanningCommentCommand(this));
	return true;
}

// none_completed may retain mutations and must not be reported as refusal.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonPIFNoneCompleted,
	"Claireon.BPEditor.PerIslandFormat.FirstIslandFailureIsNoneCompletedNotRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorPerIslandFormatInternal
{
	class FPIF_NoneCompletedCommand : public FPIF_LatentCommand
	{
	public:
		using FPIF_LatentCommand::FPIF_LatentCommand;

	protected:
		virtual TArray<FPIF_IslandSpec> Specs() const override
		{
			return {FPIF_IslandSpec{2, false}, FPIF_IslandSpec{2, false}};
		}

		virtual bool RunBody(UBlueprint& /*Blueprint*/, UEdGraph& /*Graph*/, int32 /*Pass*/) override
		{
			const TArray<int32> Order = Shape.PlanOrder();
			const FString FirstRep = Shape.Islands[Order[0]].Representative();
			const FString SecondRep = Shape.Islands[Order[1]].Representative();

			IClaireonTool::FToolResult R;
			{
				ClaireonBPFaultInjection::FScopedFault Fault(ClaireonBPFormatFaultSeam::Dispatch(0));
				R = PIF_Format(SessionId, {}, /*bAllIslands=*/true);
			}

			Test->TestEqual(TEXT("a first-island failure is none_completed"),
				PIF_Str(R, TEXT("format_status")), FString(TEXT("none_completed")));
			Test->TestNotEqual(TEXT("and NOT all_targets_excluded"),
				PIF_Str(R, TEXT("format_status")), FString(TEXT("all_targets_excluded")));
			Test->TestEqual(TEXT("the mutation axis reports an operation failure"),
				PIF_Str(R, TEXT("mutation_state")), FString(TEXT("applied_operation_failed")));
			Test->TestNotEqual(TEXT("and NOT refused"),
				PIF_Str(R, TEXT("mutation_state")), FString(TEXT("refused")));
			Test->TestTrue(TEXT("an operation failure is an error"), R.bIsError);

			Test->TestEqual(TEXT("the failed island is named"),
				PIF_Str(R, TEXT("failed_island_guid")), FirstRep);
			Test->TestEqual(TEXT("the phase is reported separately from the island"),
				PIF_Str(R, TEXT("failed_phase")), FString(TEXT("format_selective_dispatch")));
			Test->TestEqual(TEXT("the first island's row says failed"),
				PIF_RowOutcome(R, FirstRep), FString(TEXT("failed")));

			// Dispatch failure stops the plan; later islands are not_attempted.
			Test->TestEqual(TEXT("the later island was never attempted"),
				PIF_RowOutcome(R, SecondRep), FString(TEXT("not_attempted")));

			Test->TestEqual(TEXT("the failed island's member GUIDs accompany its representative"),
				PIF_StringArray(R, TEXT("failed_island_member_guids")).Num(),
				Shape.Islands[Order[0]].Members.Num());
			return false;
		}
	};
}

bool FClaireonPIFNoneCompleted::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FPIF_NoneCompletedCommand(this));
	return true;
}

// A later operation failure retains earlier successful formatting.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonPIFPartialOperationFailed,
	"Claireon.BPEditor.PerIslandFormat.LaterIslandFailureRetainsEarlierMutations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorPerIslandFormatInternal
{
	class FPIF_PartialFailedCommand : public FPIF_LatentCommand
	{
	public:
		using FPIF_LatentCommand::FPIF_LatentCommand;

	protected:
		virtual TArray<FPIF_IslandSpec> Specs() const override
		{
			return {FPIF_IslandSpec{2, false}, FPIF_IslandSpec{2, false}, FPIF_IslandSpec{2, false}};
		}

		virtual bool RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph, int32 /*Pass*/) override
		{
			const TArray<int32> Order = Shape.PlanOrder();
			const FPIF_Island& First = Shape.Islands[Order[0]];
			const FPIF_Island& Second = Shape.Islands[Order[1]];
			const FPIF_Island& Third = Shape.Islands[Order[2]];

			const TMap<FGuid, FIntPoint> Before = PIF_Positions(Graph);

			IClaireonTool::FToolResult R;
			{
				// Fail the second island to exercise partial progress.
				ClaireonBPFaultInjection::FScopedFault Fault(ClaireonBPFormatFaultSeam::Settle(1));
				R = PIF_Format(SessionId, {}, /*bAllIslands=*/true);
			}

			Test->TestEqual(TEXT("one island formatted and one failed, so coverage is partial"),
				PIF_Str(R, TEXT("format_status")), FString(TEXT("partial")));
			Test->TestEqual(TEXT("the mutation axis reports an operation failure"),
				PIF_Str(R, TEXT("mutation_state")), FString(TEXT("applied_operation_failed")));
			Test->TestTrue(TEXT("and the mutation is RETAINED"),
				PIF_Bool(R, TEXT("mutation_retained")));
			Test->TestEqual(TEXT("the failing phase is the settle"),
				PIF_Str(R, TEXT("failed_phase")), FString(TEXT("format_settle")));
			Test->TestEqual(TEXT("the failed island is the second one, not the first"),
				PIF_Str(R, TEXT("failed_island_guid")), Second.Representative());

			Test->TestEqual(TEXT("the first island formatted"),
				PIF_RowOutcome(R, First.Representative()), FString(TEXT("formatted")));
			Test->TestEqual(TEXT("the second failed"),
				PIF_RowOutcome(R, Second.Representative()), FString(TEXT("failed")));
			Test->TestEqual(TEXT("the third was never attempted"),
				PIF_RowOutcome(R, Third.Representative()), FString(TEXT("not_attempted")));

			bool bFirstIslandMoved = false;
			for (const FGuid& Member : First.Members)
			{
				const FIntPoint* Was = Before.Find(Member);
				for (const UEdGraphNode* Node : Graph.Nodes)
				{
					if (IsValid(Node) && Node->NodeGuid == Member && Was
						&& FIntPoint(Node->NodePosX, Node->NodePosY) != *Was)
					{
						bFirstIslandMoved = true;
					}
				}
			}
			Test->TestTrue(TEXT("the earlier island's mutation was retained after the later failure"),
				bFirstIslandMoved);

			bool bThirdIslandMoved = false;
			for (const FGuid& Member : Third.Members)
			{
				const FIntPoint* Was = Before.Find(Member);
				for (const UEdGraphNode* Node : Graph.Nodes)
				{
					if (IsValid(Node) && Node->NodeGuid == Member && Was
						&& FIntPoint(Node->NodePosX, Node->NodePosY) != *Was)
					{
						bThirdIslandMoved = true;
					}
				}
			}
			Test->TestFalse(TEXT("a not_attempted island was not moved"), bThirdIslandMoved);
			return false;
		}
	};
}

bool FClaireonPIFPartialOperationFailed::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FPIF_PartialFailedCommand(this));
	return true;
}

// Validation failure rolls back an island only after confirming its top transaction identity.
// Operation failure still stops the plan and retains work; validation failure permits later islands.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonPIFValidationFailed,
	"Claireon.BPEditor.PerIslandFormat.InvariantFailureRollsBackAndContinues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorPerIslandFormatInternal
{
	class FPIF_ValidationFailedCommand : public FPIF_LatentCommand
	{
	public:
		using FPIF_LatentCommand::FPIF_LatentCommand;

	protected:
		virtual TArray<FPIF_IslandSpec> Specs() const override
		{
			return {FPIF_IslandSpec{2, false}, FPIF_IslandSpec{2, false}};
		}

		virtual bool RunBody(UBlueprint& /*Blueprint*/, UEdGraph& /*Graph*/, int32 /*Pass*/) override
		{
			const TArray<int32> Order = Shape.PlanOrder();
			const FString FirstRep = Shape.Islands[Order[0]].Representative();
			const FString SecondRep = Shape.Islands[Order[1]].Representative();

			IClaireonTool::FToolResult R;
			{
				ClaireonBPFaultInjection::FScopedFault Fault(
					ClaireonBPFormatFaultSeam::InvariantValidation(0));
				R = PIF_Format(SessionId, {}, /*bAllIslands=*/true);
			}

			Test->TestEqual(TEXT("every island still formatted, so coverage is complete"),
				PIF_Str(R, TEXT("format_status")), FString(TEXT("complete")));
			Test->TestEqual(TEXT("the mutation axis reports a validation failure"),
				PIF_Str(R, TEXT("mutation_state")), FString(TEXT("applied_validation_failed")));
			Test->TestTrue(TEXT("and it is still an error"), R.bIsError);

			// The second island remains formatted even though the first was rolled back.
			Test->TestTrue(TEXT("a clean island's retained format keeps mutation_retained true"),
				PIF_Bool(R, TEXT("mutation_retained")));

			// Recovery counts standing transactions, including BA's separate link transactions.
			Test->TestTrue(TEXT("an undo record exists for the retained island"),
				PIF_Bool(R, TEXT("undo_record_available")));
			{
				const TSharedPtr<FJsonObject>* Recovery = nullptr;
				const bool bHasRecovery =
					R.Data.IsValid() && R.Data->TryGetObjectField(TEXT("recovery"), Recovery);
				Test->TestTrue(TEXT("recovery block is present for the retained island"), bHasRecovery);
				if (bHasRecovery)
				{
					double UndoCount = 0.0;
					(*Recovery)->TryGetNumberField(TEXT("undo_count"), UndoCount);
					Test->TestTrue(TEXT("recovery advertises at least the retained island's "
					                    "standing transaction"),
						static_cast<int32>(UndoCount) >= 1);
				}
			}

			Test->TestEqual(TEXT("the violating island is named"),
				PIF_Str(R, TEXT("failed_island_guid")), FirstRep);
			Test->TestEqual(TEXT("the phase is the invariant check"),
				PIF_Str(R, TEXT("failed_phase")), FString(TEXT("format_invariant_validation")));

			Test->TestFalse(TEXT("engine_compile_status is absent on an invariant failure"),
				PIF_HasField(R, TEXT("engine_compile_status")));

			// The per-island status remains formatted; invariant failure is reported at call level.
			Test->TestEqual(TEXT("the violating island is still reported formatted"),
				PIF_RowOutcome(R, FirstRep), FString(TEXT("formatted")));
			Test->TestEqual(TEXT("and the plan did NOT stop -- the later island ran too"),
				PIF_RowOutcome(R, SecondRep), FString(TEXT("formatted")));

			Test->TestTrue(TEXT("invariant_rollback is present"),
				PIF_HasField(R, TEXT("invariant_rollback")));
			return false;
		}
	};
}

bool FClaireonPIFValidationFailed::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FPIF_ValidationFailedCommand(this));
	return true;
}

// The invariant rollback undoes BlueprintAssist's transaction, and BlueprintAssist rewires a
// reroute's neighbours (FBAUtils::DisconnectKnotNode) before it calls Modify() on them, so the
// transaction snapshots those nodes post-collapse. The undo must still leave every pin link
// mutual, or the Blueprint stops compiling (ICE in CreateExecutionSchedule).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonPIFRollbackKeepsLinksSymmetric,
	"Claireon.BPEditor.PerIslandFormat.InvariantRollbackKeepsPinLinksSymmetric",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorPerIslandFormatInternal
{
	class FPIF_RollbackKeepsLinksSymmetricCommand : public FPIF_LatentCommand
	{
	public:
		using FPIF_LatentCommand::FPIF_LatentCommand;

	protected:
		virtual TArray<FPIF_IslandSpec> Specs() const override
		{
			return {FPIF_IslandSpec{2, false, /*bWithDataKnotFanOut=*/true}};
		}

		virtual bool RunBody(UBlueprint& Blueprint, UEdGraph& Graph, int32 /*Pass*/) override
		{
			const TArray<FString> PreAsymmetric = PIF_AsymmetricLinks(Graph);
			Test->TestEqual(TEXT("the fixture starts with every link mutual"),
				PreAsymmetric.Num(), 0);
			const TSet<FString> PreCallEdges = PIF_Edges(Graph);

			IClaireonTool::FToolResult R;
			{
				ClaireonBPFaultInjection::FScopedFault Fault(
					ClaireonBPFormatFaultSeam::InvariantValidation(0));
				R = PIF_Format(SessionId, {}, /*bAllIslands=*/true);
			}

			Test->TestEqual(TEXT("the island formatted"),
				PIF_Str(R, TEXT("format_status")), FString(TEXT("complete")));
			Test->TestEqual(TEXT("the injected invariant failure is reported"),
				PIF_Str(R, TEXT("mutation_state")), FString(TEXT("applied_validation_failed")));

			bool bRolledBack = false;
			{
				const TSharedPtr<FJsonObject>* Rollback = nullptr;
				if (R.Data.IsValid() && R.Data->TryGetObjectField(TEXT("invariant_rollback"), Rollback))
				{
					double RolledBack = 0.0;
					(*Rollback)->TryGetNumberField(TEXT("islands_rolled_back"), RolledBack);
					bRolledBack = RolledBack >= 1.0;

					const TArray<TSharedPtr<FJsonValue>>* Islands = nullptr;
					if ((*Rollback)->TryGetArrayField(TEXT("islands"), Islands))
					{
						for (const TSharedPtr<FJsonValue>& Row : *Islands)
						{
							FString Detail;
							if (Row.IsValid() && Row->AsObject().IsValid()
								&& Row->AsObject()->TryGetStringField(TEXT("detail"), Detail))
							{
								Test->AddInfo(FString::Printf(TEXT("rollback detail: %s"), *Detail));
							}
						}
					}
				}
			}
			Test->TestTrue(TEXT("the rollback ran, so this exercises the undo path"), bRolledBack);

			// The undo may leave BlueprintAssist's own link transactions standing; reverse those too
			// so the graph is back at the pre-call structure the rollback promises.
			{
				const TSharedPtr<FJsonObject>* Recovery = nullptr;
				if (R.Data.IsValid() && R.Data->TryGetObjectField(TEXT("recovery"), Recovery))
				{
					double UndoCount = 0.0;
					(*Recovery)->TryGetNumberField(TEXT("undo_count"), UndoCount);
					for (int32 i = 0; i < static_cast<int32>(UndoCount); ++i)
					{
						Test->TestTrue(TEXT("the advertised recovery undo succeeds"),
							GEditor->UndoTransaction());
					}
				}
			}

			const TArray<FString> PostAsymmetric = PIF_AsymmetricLinks(Graph);
			if (PostAsymmetric.Num() > 0)
			{
				Test->AddInfo(FString::Printf(TEXT("one-directional links: %s"), *PIF_Join(PostAsymmetric)));
			}
			Test->TestEqual(TEXT("after the rollback every pin link is still mutual -- a "
			                     "one-directional LinkedTo is a corrupt graph"),
				PostAsymmetric.Num(), 0);
			Test->TestTrue(TEXT("and the pre-call wiring is back exactly"),
				PIF_SameEdges(PreCallEdges, PIF_Edges(Graph)));

			FKismetEditorUtilities::CompileBlueprint(&Blueprint,
				EBlueprintCompileOptions::SkipGarbageCollection);
			Test->TestNotEqual(TEXT("the rolled-back Blueprint compiles"),
				static_cast<int32>(Blueprint.Status), static_cast<int32>(BS_Error));
			return false;
		}
	};
}

bool FClaireonPIFRollbackKeepsLinksSymmetric::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FPIF_RollbackKeepsLinksSymmetricCommand(this));
	return true;
}

// After all island rollbacks, compare retention and recovery against remaining transactions and graph state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonPIFAllRolledBack,
	"Claireon.BPEditor.PerIslandFormat.AllIslandsRolledBackRetainsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorPerIslandFormatInternal
{
	class FPIF_AllRolledBackCommand : public FPIF_LatentCommand
	{
	public:
		using FPIF_LatentCommand::FPIF_LatentCommand;

	protected:
		virtual TArray<FPIF_IslandSpec> Specs() const override
		{
			return {FPIF_IslandSpec{2, false}};
		}

		virtual bool RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph, int32 /*Pass*/) override
		{
			const TMap<FGuid, FIntPoint> PreCall = PIF_Positions(Graph);
			const TSet<FString> PreCallEdges = PIF_Edges(Graph);

			IClaireonTool::FToolResult R;
			{
				ClaireonBPFaultInjection::FScopedFault Fault(
					ClaireonBPFormatFaultSeam::InvariantValidation(0));
				R = PIF_Format(SessionId, {}, /*bAllIslands=*/true);
			}

			Test->TestEqual(TEXT("the island formatted, so coverage is complete"),
				PIF_Str(R, TEXT("format_status")), FString(TEXT("complete")));
			Test->TestEqual(TEXT("the mutation axis reports a validation failure"),
				PIF_Str(R, TEXT("mutation_state")), FString(TEXT("applied_validation_failed")));
			Test->TestTrue(TEXT("and it is an error"), R.bIsError);
			Test->TestTrue(TEXT("the rollback is reported"),
				PIF_HasField(R, TEXT("invariant_rollback")));

			// Distinguish standing undoable transactions from untransacted settle-tick position residue.
			const bool bRetained = PIF_Bool(R, TEXT("mutation_retained"));
			const bool bUndoRecord = PIF_Bool(R, TEXT("undo_record_available"));
			const int32 NodesMoved = PIF_Int(R, TEXT("nodes_moved"), 0);
			const int32 NodesAdded = PIF_Int(R, TEXT("nodes_added"), 0);
			Test->TestEqual(TEXT("mutation_retained equals 'anything remains' -- standing "
			                     "transactions OR graph residue"),
				bRetained, bUndoRecord || NodesMoved > 0 || NodesAdded > 0);
			Test->TestEqual(TEXT("the recovery block's presence tracks the standing "
			                     "transactions"),
				PIF_HasField(R, TEXT("recovery")), bUndoRecord);
			if (bRetained && !bUndoRecord)
			{
				Test->TestTrue(TEXT("residue-only retention is named as untransacted"),
					PIF_Bool(R, TEXT("retained_is_untransacted_residue")));
			}
			if (bUndoRecord)
			{
				const TSharedPtr<FJsonObject>* Recovery = nullptr;
				if (R.Data.IsValid() && R.Data->TryGetObjectField(TEXT("recovery"), Recovery))
				{
					double UndoCount = 0.0;
					(*Recovery)->TryGetNumberField(TEXT("undo_count"), UndoCount);
					for (int32 i = 0; i < static_cast<int32>(UndoCount); ++i)
					{
						Test->TestTrue(TEXT("the retained-work undo succeeds"),
							GEditor->UndoTransaction());
					}
					// Structure exactly; positions can keep untransacted residue.
					const TMap<FGuid, FIntPoint> After = PIF_Positions(Graph);
					bool bEveryNodeSurvives = After.Num() == PreCall.Num();
					for (const TPair<FGuid, FIntPoint>& Pair : PreCall)
					{
						if (!After.Contains(Pair.Key))
						{
							bEveryNodeSurvives = false;
							break;
						}
					}
					Test->TestTrue(TEXT("executing the advertised recovery restores the "
					                    "pre-call structure"), bEveryNodeSurvives);
					Test->TestTrue(TEXT("and the pre-call edge topology: every wire back, no "
					                    "minted wire left behind"),
						PIF_SameEdges(PreCallEdges, PIF_Edges(Graph)));
				}
			}
			return false;
		}
	};
}

bool FClaireonPIFAllRolledBack::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FPIF_AllRolledBackCommand(this));
	return true;
}

// Execute the advertised recovery count after a later settle failure, preserving a sentinel below the format window.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonPIFSettleFailRecovery,
	"Claireon.BPEditor.PerIslandFormat.SettleFailedIslandIsCountedInRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorPerIslandFormatInternal
{
	/**
	 * Run the advertised undos and verify structure and sentinel boundaries.
	 * Allow separate BA link transactions rather than pinning an exact count.
	 */
	static void PIF_ProveRecovery(FAutomationTestBase& Test,
		const IClaireonTool::FToolResult& R, UEdGraph& Graph,
		const TMap<FGuid, FIntPoint>& PreCall, const TSet<FString>& PreCallEdges,
		int32 MinimumUndoCount,
		UEdGraphNode* SentinelNode, int32 SentinelMovedX, int32 SentinelOriginalX)
	{
		Test.TestTrue(TEXT("an undo record is advertised for the retained transaction(s)"),
			PIF_Bool(R, TEXT("undo_record_available")));

		const TSharedPtr<FJsonObject>* Recovery = nullptr;
		const bool bHasRecovery =
			R.Data.IsValid() && R.Data->TryGetObjectField(TEXT("recovery"), Recovery);
		Test.TestTrue(TEXT("a recovery block is present"), bHasRecovery);
		if (!bHasRecovery)
		{
			return;
		}
		double UndoCountRaw = 0.0;
		(*Recovery)->TryGetNumberField(TEXT("undo_count"), UndoCountRaw);
		const int32 UndoCount = static_cast<int32>(UndoCountRaw);
		Test.TestTrue(FString::Printf(
			TEXT("the advertised undo count (%d) covers at least the formatter transactions "
			     "(>= %d), the settle-failed island's included"), UndoCount, MinimumUndoCount),
			UndoCount >= MinimumUndoCount);
		FString Confidence;
		(*Recovery)->TryGetStringField(TEXT("count_confidence"), Confidence);
		Test.TestEqual(TEXT("every dispatch signalled completion and the pre-call head was "
		                    "re-found, so the count is exact"),
			Confidence, FString(TEXT("exact")));

		for (int32 i = 0; i < UndoCount; ++i)
		{
			Test.TestTrue(FString::Printf(TEXT("recovery undo %d of %d succeeds"),
				i + 1, UndoCount), GEditor->UndoTransaction());
		}

		// Require exact structural recovery; undo cannot restore untransacted settle-tick position changes.
		const TMap<FGuid, FIntPoint> AfterRecovery = PIF_Positions(Graph);
		bool bEveryPreCallNodeSurvives = true;
		for (const TPair<FGuid, FIntPoint>& Pair : PreCall)
		{
			if (!AfterRecovery.Contains(Pair.Key))
			{
				bEveryPreCallNodeSurvives = false;
				break;
			}
		}
		Test.TestTrue(TEXT("every pre-call node survives the recovery"), bEveryPreCallNodeSurvives);
		Test.TestEqual(TEXT("no BlueprintAssist-minted node is left behind by the recovery"),
			AfterRecovery.Num(), PreCall.Num());
		// Compare edges as well as node identity to catch leftover reroutes.
		Test.TestTrue(TEXT("the recovery restores the pre-call edge topology exactly"),
			PIF_SameEdges(PreCallEdges, PIF_Edges(Graph)));

		// The advertised undo count must preserve the sentinel; one further undo must restore its recorded pre-state.
		Test.TestTrue(TEXT("the sentinel edit is still in effect before the boundary undo -- "
		                   "the advertised count did not consume it"),
			SentinelNode->NodePosX != SentinelOriginalX);
		Test.TestTrue(TEXT("the transaction beneath the recovery is undoable"),
			GEditor->UndoTransaction());
		Test.TestEqual(TEXT("that undo restores the sentinel node's pre-sentinel X exactly -- "
		                    "the advertised count stopped precisely at this call's own "
		                    "transactions"),
			SentinelNode->NodePosX, SentinelOriginalX);
		(void)SentinelMovedX;
	}

	class FPIF_SettleFailRecoveryCommand : public FPIF_LatentCommand
	{
	public:
		using FPIF_LatentCommand::FPIF_LatentCommand;

	protected:
		virtual TArray<FPIF_IslandSpec> Specs() const override
		{
			return {FPIF_IslandSpec{2, false}, FPIF_IslandSpec{2, false}};
		}

		virtual bool RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph, int32 /*Pass*/) override
		{
			UEdGraphNode* SentinelNode = nullptr;
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				if (IsValid(Node))
				{
					SentinelNode = Node;
					break;
				}
			}
			if (!SentinelNode)
			{
				Test->AddError(TEXT("the fixture has no node to use as a sentinel."));
				return false;
			}
			const int32 SentinelOriginalX = SentinelNode->NodePosX;
			{
				const FScopedTransaction Sentinel(FText::FromString(TEXT("PIF recovery sentinel")));
				SentinelNode->Modify();
				SentinelNode->NodePosX += 731;
			}
			const int32 SentinelMovedX = SentinelNode->NodePosX;

			const TMap<FGuid, FIntPoint> PreCall = PIF_Positions(Graph);
			const TSet<FString> PreCallEdges = PIF_Edges(Graph);

			IClaireonTool::FToolResult R;
			{
				// Fail settlement after the second island has opened its format transaction.
				ClaireonBPFaultInjection::FScopedFault Fault(ClaireonBPFormatFaultSeam::Settle(1));
				R = PIF_Format(SessionId, {}, /*bAllIslands=*/true);
			}
			Test->TestEqual(TEXT("the mutation axis reports an operation failure"),
				PIF_Str(R, TEXT("mutation_state")), FString(TEXT("applied_operation_failed")));
			Test->TestTrue(TEXT("and the mutation is retained"),
				PIF_Bool(R, TEXT("mutation_retained")));

			PIF_ProveRecovery(*Test, R, Graph, PreCall, PreCallEdges, /*MinimumUndoCount=*/2,
				SentinelNode, SentinelMovedX, SentinelOriginalX);
			return false;
		}
	};
}

bool FClaireonPIFSettleFailRecovery::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FPIF_SettleFailRecoveryCommand(this));
	return true;
}

// A first-island settle failure must still advertise its retained transaction.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonPIFFirstSettleFailRecovery,
	"Claireon.BPEditor.PerIslandFormat.FirstIslandSettleFailureStillAdvertisesRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorPerIslandFormatInternal
{
	class FPIF_FirstSettleFailCommand : public FPIF_LatentCommand
	{
	public:
		using FPIF_LatentCommand::FPIF_LatentCommand;

	protected:
		virtual TArray<FPIF_IslandSpec> Specs() const override
		{
			return {FPIF_IslandSpec{2, false}};
		}

		virtual bool RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph, int32 /*Pass*/) override
		{
			UEdGraphNode* SentinelNode = nullptr;
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				if (IsValid(Node))
				{
					SentinelNode = Node;
					break;
				}
			}
			if (!SentinelNode)
			{
				Test->AddError(TEXT("the fixture has no node to use as a sentinel."));
				return false;
			}
			const int32 SentinelOriginalX = SentinelNode->NodePosX;
			{
				const FScopedTransaction Sentinel(FText::FromString(TEXT("PIF recovery sentinel")));
				SentinelNode->Modify();
				SentinelNode->NodePosX += 731;
			}
			const int32 SentinelMovedX = SentinelNode->NodePosX;

			const TMap<FGuid, FIntPoint> PreCall = PIF_Positions(Graph);
			const TSet<FString> PreCallEdges = PIF_Edges(Graph);

			IClaireonTool::FToolResult R;
			{
				ClaireonBPFaultInjection::FScopedFault Fault(ClaireonBPFormatFaultSeam::Settle(0));
				R = PIF_Format(SessionId, {}, /*bAllIslands=*/true);
			}
			Test->TestEqual(TEXT("the mutation axis reports an operation failure"),
				PIF_Str(R, TEXT("mutation_state")), FString(TEXT("applied_operation_failed")));

			PIF_ProveRecovery(*Test, R, Graph, PreCall, PreCallEdges, /*MinimumUndoCount=*/1,
				SentinelNode, SentinelMovedX, SentinelOriginalX);
			return false;
		}
	};
}

bool FClaireonPIFFirstSettleFailRecovery::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FPIF_FirstSettleFailCommand(this));
	return true;
}

// Restore borrowed selection, reporting deleted members.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonPIFSelectionRestored,
	"Claireon.BPEditor.PerIslandFormat.BorrowedSelectionIsRestoredOverSurvivors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorPerIslandFormatInternal
{
	class FPIF_SelectionCommand : public FPIF_LatentCommand
	{
	public:
		using FPIF_LatentCommand::FPIF_LatentCommand;

	protected:
		virtual TArray<FPIF_IslandSpec> Specs() const override
		{
			// BA may replace reroutes with new GUIDs, preventing exact selection restoration.
			return {FPIF_IslandSpec{2, true}, FPIF_IslandSpec{2, false}};
		}

		virtual bool RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph, int32 /*Pass*/) override
		{
			int32 KnotIsland = INDEX_NONE;
			for (int32 I = 0; I < Shape.Islands.Num(); ++I)
			{
				if (Shape.Islands[I].KnotGuid.IsValid())
				{
					KnotIsland = I;
				}
			}
			if (KnotIsland == INDEX_NONE)
			{
				Test->AddError(TEXT("the fixture produced no reroute to select."));
				return false;
			}

			const FString KnotGuid = PIF_Guid(Shape.Islands[KnotIsland].KnotGuid);
			const FString SurvivorGuid = PIF_Guid(Shape.Islands[KnotIsland].EventGuid);

			ClaireonBlueprintGraphTool_SelectionSet SetTool;
			TSharedPtr<FJsonObject> SetArgs = MakeShared<FJsonObject>();
			SetArgs->SetStringField(TEXT("session_id"), SessionId);
			TArray<TSharedPtr<FJsonValue>> Seed;
			Seed.Add(MakeShared<FJsonValueString>(KnotGuid));
			Seed.Add(MakeShared<FJsonValueString>(SurvivorGuid));
			SetArgs->SetArrayField(TEXT("node_guids"), Seed);
			const IClaireonTool::FToolResult Seeded = SetTool.Execute(SetArgs);
			if (Seeded.bIsError)
			{
				Test->AddError(FString::Printf(TEXT("seeding the selection failed: %s"),
					*Seeded.ErrorMessage));
				return false;
			}

			const IClaireonTool::FToolResult R = PIF_Format(SessionId, {}, /*bAllIslands=*/true);

			const FString RestoreStatus = PIF_Str(R, TEXT("selection_restore_status"));
			Test->TestTrue(TEXT("the restore status is reported at all"), !RestoreStatus.IsEmpty());

			// Selection restoration must not change the primary format result.
			Test->TestEqual(TEXT("a partial restore leaves the mutation state alone"),
				PIF_Str(R, TEXT("mutation_state")), FString(TEXT("applied_clean")));
			Test->TestFalse(TEXT("and does not make the call an error"), R.bIsError);

			if (RestoreStatus == TEXT("restored_partial"))
			{
				const TArray<FString> Missing = PIF_StringArray(R, TEXT("selection_restore_missing"));
				Test->TestTrue(TEXT("the GUID that could not be restored is named"),
					Missing.Contains(KnotGuid));
			}
			else
			{
				// Knot pooling may preserve the reroute; accept either outcome without changing BA settings.
				Test->AddInfo(FString::Printf(
					TEXT("restore status was '%s', so this BlueprintAssist configuration did not "
						 "delete the selected reroute; the partial-restore branch was not exercised."),
					*RestoreStatus));
				Test->TestEqual(TEXT("then the restore must be exact"),
					RestoreStatus, FString(TEXT("restored")));
			}

			// Read restoration through the same widget the format used.
			FBlueprintEditToolData* Data = ClaireonBlueprintGraphEditToolBase::FindToolData(SessionId);
			if (!Data)
			{
				Test->AddError(TEXT("the session lost its tool data."));
				return false;
			}
			EClaireonEditorBindingStatus Status = EClaireonEditorBindingStatus::NotBound;
			TSharedPtr<SGraphEditor> Widget = Data->EditorBinding.ResolveGraphEditor(&Graph, Status);
			if (!Widget.IsValid())
			{
				Test->AddError(FString::Printf(TEXT("the bound widget did not resolve: %s"),
					ClaireonEditorBindingStatusToWireString(Status)));
				return false;
			}

			bool bSurvivorSelected = false;
			TArray<FString> WidgetNow;
			for (UObject* Selected : Widget->GetSelectedNodes())
			{
				const UEdGraphNode* Node = Cast<UEdGraphNode>(Selected);
				if (!IsValid(Node))
				{
					WidgetNow.Add(TEXT("<non-node>"));
					continue;
				}
				WidgetNow.Add(PIF_Guid(Node->NodeGuid));
				if (PIF_Guid(Node->NodeGuid) == SurvivorGuid)
				{
					bSurvivorSelected = true;
				}
			}

			bool bSurvivorAlive = false;
			for (const UEdGraphNode* Node : Graph.Nodes)
			{
				if (IsValid(Node) && PIF_Guid(Node->NodeGuid) == SurvivorGuid)
				{
					bSurvivorAlive = true;
				}
			}

			Test->AddInfo(FString::Printf(
				TEXT("restore_status='%s' missing=[%s] survivor=%s alive=%s widget_now=[%s]"),
				*RestoreStatus,
				*PIF_Join(PIF_StringArray(R, TEXT("selection_restore_missing"))),
				*SurvivorGuid,
				bSurvivorAlive ? TEXT("yes") : TEXT("NO"),
				*PIF_Join(WidgetNow)));

			Test->TestTrue(TEXT("the survivor still exists after the format"), bSurvivorAlive);
			Test->TestTrue(TEXT("the surviving pre-call selection is selected again"),
				bSurvivorSelected);
			return false;
		}
	};
}

bool FClaireonPIFSelectionRestored::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FPIF_SelectionCommand(this));
	return true;
}

// Formatting one island must preserve the others.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonPIFIsolation,
	"Claireon.BPEditor.PerIslandFormat.FormattingOneIslandLeavesTheOtherAlone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorPerIslandFormatInternal
{
	class FPIF_IsolationCommand : public FPIF_LatentCommand
	{
	public:
		using FPIF_LatentCommand::FPIF_LatentCommand;

	protected:
		virtual TArray<FPIF_IslandSpec> Specs() const override
		{
			return {FPIF_IslandSpec{2, false}, FPIF_IslandSpec{2, false}};
		}

		virtual bool RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph, int32 /*Pass*/) override
		{
			const TArray<int32> Order = Shape.PlanOrder();
			const FPIF_Island& Target = Shape.Islands[Order[0]];
			const FPIF_Island& Bystander = Shape.Islands[Order[1]];

			const TSet<FGuid> BystanderMembers = PIF_NonKnotMembers(Graph, Bystander);
			const TArray<FString> BystanderSemanticsBefore = PIF_SemanticKeys(Graph, BystanderMembers);

			TMap<FGuid, FIntPoint> BystanderBefore;
			for (const UEdGraphNode* Node : Graph.Nodes)
			{
				if (IsValid(Node) && Bystander.Members.Contains(Node->NodeGuid))
				{
					BystanderBefore.Add(Node->NodeGuid, FIntPoint(Node->NodePosX, Node->NodePosY));
				}
			}

			const IClaireonTool::FToolResult R =
				PIF_Format(SessionId, {Target.Representative()}, /*bAllIslands=*/false);

			Test->TestEqual(TEXT("a single-island request that succeeds is complete"),
				PIF_Str(R, TEXT("format_status")), FString(TEXT("complete")));
			Test->TestEqual(TEXT("only one island was requested"),
				PIF_Int(R, TEXT("islands_requested")), 1);
			Test->TestEqual(TEXT("and it formatted"),
				PIF_RowOutcome(R, Target.Representative()), FString(TEXT("formatted")));

			bool bBystanderMoved = false;
			for (const UEdGraphNode* Node : Graph.Nodes)
			{
				if (!IsValid(Node))
				{
					continue;
				}
				const FIntPoint* Was = BystanderBefore.Find(Node->NodeGuid);
				if (Was && FIntPoint(Node->NodePosX, Node->NodePosY) != *Was)
				{
					bBystanderMoved = true;
				}
			}
			Test->TestFalse(TEXT("formatting one island moved no node of the other"), bBystanderMoved);

			// Compare knot-transparent semantics as well as positions.
			Test->TestEqual(TEXT("and rewired nothing in the other"),
				PIF_Join(PIF_SemanticKeys(Graph, BystanderMembers)),
				PIF_Join(BystanderSemanticsBefore));
			return false;
		}
	};
}

bool FClaireonPIFIsolation::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FPIF_IsolationCommand(this));
	return true;
}

// Compare execution order and semantic preservation across independent runs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonPIFDeterminism,
	"Claireon.BPEditor.PerIslandFormat.OrderAndOutcomesAreDeterministicAcrossRuns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorPerIslandFormatInternal
{
	class FPIF_DeterminismCommand : public FPIF_LatentCommand
	{
	public:
		using FPIF_LatentCommand::FPIF_LatentCommand;

	protected:
		virtual TArray<FPIF_IslandSpec> Specs() const override
		{
			return {FPIF_IslandSpec{2, false}, FPIF_IslandSpec{2, false}, FPIF_IslandSpec{1, false}};
		}

		virtual bool RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph, int32 InPassIndex) override
		{
			const IClaireonTool::FToolResult R = PIF_Format(SessionId, {}, /*bAllIslands=*/true);

			// Derive ascending representative-GUID order independently of the result.
			TArray<FString> Expected;
			for (int32 Index : Shape.PlanOrder())
			{
				Expected.Add(Shape.Islands[Index].Representative());
			}
			Test->TestEqual(
				FString::Printf(TEXT("pass %d requests islands in ascending representative order"),
					InPassIndex),
				PIF_Join(PIF_StringArray(R, TEXT("requested_islands"))), PIF_Join(Expected));

			FPass Observed;
			Observed.Coverage = PIF_Str(R, TEXT("format_status"));
			Observed.Mutation = PIF_Str(R, TEXT("mutation_state"));
			Observed.Requested = PIF_Int(R, TEXT("islands_requested"));
			Observed.Formatted = PIF_Int(R, TEXT("islands_formatted"));

			// Compare outcomes in fixture-spec order because regenerated GUIDs can change plan order.
			for (int32 Index = 0; Index < Shape.Islands.Num(); ++Index)
			{
				const FPIF_Island& Island = Shape.Islands[Index];
				Observed.Outcomes.Add(PIF_RowOutcome(R, Island.Representative()));
				Observed.Semantics.Append(PIF_SemanticKeys(Graph, PIF_NonKnotMembers(Graph, Island)));
			}

			if (InPassIndex == 0)
			{
				First = MoveTemp(Observed);
				// Reset the fixture for an independent second run.
				return true;
			}

			Test->TestEqual(TEXT("coverage is the same on both runs"),
				Observed.Coverage, First.Coverage);
			Test->TestEqual(TEXT("the mutation state is the same"),
				Observed.Mutation, First.Mutation);
			Test->TestEqual(TEXT("the requested count is the same"),
				Observed.Requested, First.Requested);
			Test->TestEqual(TEXT("the formatted count is the same"),
				Observed.Formatted, First.Formatted);
			Test->TestEqual(
				TEXT("each island of the spec gets the same outcome on both passes"),
				PIF_Join(Observed.Outcomes), PIF_Join(First.Outcomes));

			// Fresh node and knot GUIDs prevent identity comparisons; compare preserved semantic-edge counts.
			Test->TestEqual(TEXT("the same number of knot-transparent edges survive both runs"),
				Observed.Semantics.Num(), First.Semantics.Num());
			return false;
		}

	private:
		struct FPass
		{
			FString Coverage;
			FString Mutation;
			int32 Requested = -1;
			int32 Formatted = -1;
			TArray<FString> Outcomes;
			TArray<FString> Semantics;
		};

		FPass First;
	};
}

bool FClaireonPIFDeterminism::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FPIF_DeterminismCommand(this));
	return true;
}

// Run formatting on its own output and require a fixed point in both report and graph.
// Compare non-knot positions and knot-transparent semantics.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonPIFFixedPoint,
	"Claireon.BPEditor.PerIslandFormat.FormattingIsAFixedPointOnASecondPass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorPerIslandFormatInternal
{
	class FPIF_FixedPointCommand : public FPIF_LatentCommand
	{
	public:
		using FPIF_LatentCommand::FPIF_LatentCommand;

	protected:
		virtual TArray<FPIF_IslandSpec> Specs() const override
		{
			return {FPIF_IslandSpec{3, false}, FPIF_IslandSpec{2, false}};
		}

		/** Start the next pass from the previous output. */
		virtual bool WantsReshapeBetweenPasses() const override { return false; }

		virtual bool RunBody(UBlueprint& /*Blueprint*/, UEdGraph& Graph, int32 InPassIndex) override
		{
			const IClaireonTool::FToolResult R = PIF_Format(SessionId, {}, /*bAllIslands=*/true);

			// Require successful runs so a refusal cannot satisfy the no-op assertions.
			Test->TestEqual(
				FString::Printf(TEXT("pass %d formatted every requested island"), InPassIndex),
				PIF_Str(R, TEXT("format_status")), FString(TEXT("complete")));
			Test->TestEqual(
				FString::Printf(TEXT("pass %d applied cleanly"), InPassIndex),
				PIF_Str(R, TEXT("mutation_state")), FString(TEXT("applied_clean")));

			TArray<FString> Placements;
			TSet<FGuid> AllNonKnot;
			for (const UEdGraphNode* Node : Graph.Nodes)
			{
				if (!IsValid(Node) || Node->IsA<UK2Node_Knot>() || Node->IsA<UEdGraphNode_Comment>())
				{
					continue;
				}
				AllNonKnot.Add(Node->NodeGuid);
				Placements.Add(FString::Printf(TEXT("%s@%d,%d"),
					*Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
					Node->NodePosX, Node->NodePosY));
			}
			Placements.Sort();
			const TArray<FString> Semantics = PIF_SemanticKeys(Graph, AllNonKnot);

			int32 KnotCount = 0;
			for (const UEdGraphNode* Node : Graph.Nodes)
			{
				if (IsValid(Node) && Node->IsA<UK2Node_Knot>())
				{
					++KnotCount;
				}
			}

			if (InPassIndex == 0)
			{
				// Require the first pass to change the graph before testing a no-op second pass.
				Test->TestTrue(TEXT("the first pass moved at least one node"),
					PIF_Int(R, TEXT("nodes_moved")) > 0);

				FirstPlacements = MoveTemp(Placements);
				FirstSemantics = Semantics;
				FirstKnotCount = KnotCount;
				return true;
			}

			Test->TestEqual(TEXT("the second pass moved no node"),
				PIF_Int(R, TEXT("nodes_moved")), 0);
			Test->TestEqual(TEXT("and added no reroute"),
				PIF_Int(R, TEXT("nodes_added")), 0);

			Test->TestEqual(TEXT("every non-knot node is where the first pass left it"),
				PIF_Join(Placements), PIF_Join(FirstPlacements));
			Test->TestEqual(TEXT("the knot count is unchanged"), KnotCount, FirstKnotCount);
			Test->TestEqual(TEXT("and nothing was rewired"),
				PIF_Join(Semantics), PIF_Join(FirstSemantics));
			return false;
		}

	private:
		TArray<FString> FirstPlacements;
		TArray<FString> FirstSemantics;
		int32 FirstKnotCount = -1;
	};
}

bool FClaireonPIFFixedPoint::RunTest(const FString& /*Parameters*/)
{
	ADD_LATENT_AUTOMATION_COMMAND(FPIF_FixedPointCommand(this));
	return true;
}

#endif // WITH_CLAIREON_TESTS && WITH_EDITOR
