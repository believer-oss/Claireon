// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintGraphTool_Extract.h"

#include "ClaireonBlueprintEditorAccess.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonExecTopology.h"
#include "ClaireonGraphIslands.h"
#include "ClaireonLog.h"
#include "Tools/ClaireonBPFunctionRecipe.h"
#include "Tools/ClaireonBPMutationResult.h"
#include "Tools/ClaireonBPSnapshot.h"
#include "Tools/ClaireonTransactionGroupState.h"
#include "Tools/FToolSchemaBuilder.h"

#include "BlueprintEditor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/Blueprint.h"
#include "GraphEditor.h"
#include "EdGraphToken.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Composite.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Knot.h"
#include "K2Node_Tunnel.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;

namespace ClaireonExtractInternal
{

	enum class EExtractKind : uint8
	{
		Function,
		Macro,
		Composite,
		Event,
	};

	const TCHAR* Extract_KindName(EExtractKind Kind)
	{
		switch (Kind)
		{
		case EExtractKind::Function:  return TEXT("function");
		case EExtractKind::Macro:     return TEXT("macro");
		case EExtractKind::Composite: return TEXT("composite");
		case EExtractKind::Event:     return TEXT("event");
		}
		return TEXT("unknown");
	}

	/** Shared transaction title for extraction and ownership reporting. */
	FText Extract_TransactionTitle(EExtractKind Kind)
	{
		return FText::Format(
			LOCTEXT("ClaireonExtract", "Extract {0}"),
			FText::FromString(Extract_KindName(Kind)));
	}

	FString Extract_NodeId(const UEdGraphNode* Node)
	{
		return IsValid(Node)
			? Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)
			: TEXT("<null>");
	}

	/** Normalize both GUID spellings emitted by Claireon; report hyphenated GUIDs. */
	FString Extract_NodeKey(const FString& Id)
	{
		return Id.Replace(TEXT("-"), TEXT("")).ToUpper();
	}

	/** Resolve explicit node GUIDs or an exec traversal from an anchor. */
	bool Extract_ResolveSelection(const TSharedPtr<FJsonObject>& Params, UEdGraph* Graph,
	                              TSet<UEdGraphNode*>& OutSelection, FString& OutError)
	{
		TMap<FString, UEdGraphNode*> ById;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (IsValid(Node))
			{
				ById.Add(Extract_NodeKey(Extract_NodeId(Node)), Node);
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* GuidArray = nullptr;
		if (Params->TryGetArrayField(TEXT("node_guids"), GuidArray) && GuidArray)
		{
			TArray<FString> Missing;
			for (const TSharedPtr<FJsonValue>& Value : *GuidArray)
			{
				const FString Id = Value->AsString();
				if (UEdGraphNode** Found = ById.Find(Extract_NodeKey(Id)))
				{
					OutSelection.Add(*Found);
				}
				else
				{
					Missing.Add(Id);
				}
			}
			if (Missing.Num() > 0)
			{
				OutError = FString::Printf(
					TEXT("%d node_guids are not in graph '%s': %s. Use bp_get_graph to list ")
					TEXT("the graph's node guids."),
					Missing.Num(), *Graph->GetName(), *FString::Join(Missing, TEXT(", ")));
				return false;
			}
			return true;
		}

		FString AnchorId;
		if (!Params->TryGetStringField(TEXT("anchor_node_guid"), AnchorId) || AnchorId.IsEmpty())
		{
			OutError = TEXT("Supply node_guids (the explicit selection) or anchor_node_guid "
			                "with traversal_depth (walk outward from one node).");
			return false;
		}
		UEdGraphNode** Anchor = ById.Find(Extract_NodeKey(AnchorId));
		if (!Anchor)
		{
			OutError = FString::Printf(TEXT("anchor_node_guid '%s' is not in graph '%s'."),
				*AnchorId, *Graph->GetName());
			return false;
		}

		double DepthValue = 1.0;
		Params->TryGetNumberField(TEXT("traversal_depth"), DepthValue);
		const int32 Depth = FMath::Max(0, static_cast<int32>(DepthValue));

		// Traverse exec edges in both directions through knots, excluding knots from the selection.
		TArray<UEdGraphNode*> Frontier;
		Frontier.Add(*Anchor);
		OutSelection.Add(*Anchor);
		for (int32 Step = 0; Step < Depth; ++Step)
		{
			TArray<UEdGraphNode*> NextFrontier;
			for (UEdGraphNode* Node : Frontier)
			{
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
					{
						continue;
					}
					for (UEdGraphPin* Linked : Pin->LinkedTo)
					{
						UEdGraphNode* Other = Linked ? Linked->GetOwningNode() : nullptr;
						while (IsValid(Other) && Cast<UK2Node_Knot>(Other))
						{
							UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Other);
							UEdGraphPin* Through = (Pin->Direction == EGPD_Output)
								? Knot->GetOutputPin() : Knot->GetInputPin();
							Other = (Through && Through->LinkedTo.Num() > 0 && Through->LinkedTo[0])
								? Through->LinkedTo[0]->GetOwningNode() : nullptr;
						}
						if (IsValid(Other) && !OutSelection.Contains(Other))
						{
							OutSelection.Add(Other);
							NextFrontier.Add(Other);
						}
					}
				}
			}
			Frontier = MoveTemp(NextFrontier);
		}
		return true;
	}

	/** An exec edge that leaves the selection, resolved through any reroute chain. */
	struct FBoundaryEdge
	{
		UEdGraphNode* FromNode = nullptr;
		FString FromPin;
		UEdGraphNode* ToNode = nullptr;
	};

	UEdGraphNode* Extract_ResolveThroughKnots(UEdGraphPin* StartPin, EEdGraphPinDirection Direction)
	{
		UEdGraphNode* Node = StartPin ? StartPin->GetOwningNode() : nullptr;
		int32 Guard = 0;
		while (IsValid(Node) && Cast<UK2Node_Knot>(Node) && Guard++ < 64)
		{
			UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node);
			UEdGraphPin* Through = (Direction == EGPD_Output) ? Knot->GetOutputPin() : Knot->GetInputPin();
			Node = (Through && Through->LinkedTo.Num() > 0 && Through->LinkedTo[0])
				? Through->LinkedTo[0]->GetOwningNode() : nullptr;
		}
		return Node;
	}

	/**
	 * Event extraction requires a terminal exec region with no crossing data pins.
	 * Custom events do not block callers, and this tool does not synthesize event parameters.
	 */
	void Extract_FindBoundary(const TSet<UEdGraphNode*>& Selection,
	                          TArray<FBoundaryEdge>& OutExecLeaving,
	                          TArray<FString>& OutCrossingDataPins)
	{
		for (UEdGraphNode* Node : Selection)
		{
			if (!IsValid(Node))
			{
				continue;
			}
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}
				const bool bExec = Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					UEdGraphNode* Other = Extract_ResolveThroughKnots(Linked, Pin->Direction);
					if (!IsValid(Other) || Selection.Contains(Other))
					{
						continue;
					}
					if (bExec)
					{
						if (Pin->Direction == EGPD_Output)
						{
							FBoundaryEdge Edge;
							Edge.FromNode = Node;
							Edge.FromPin = Pin->PinName.ToString();
							Edge.ToNode = Other;
							OutExecLeaving.Add(Edge);
						}
					}
					else
					{
						OutCrossingDataPins.Add(FString::Printf(TEXT("%s.%s"),
							*Extract_NodeId(Node), *Pin->PinName.ToString()));
					}
				}
			}
		}
	}

	/** Exec edges ARRIVING at the selection from outside, which become the call site. */
	void Extract_FindEntries(const TSet<UEdGraphNode*>& Selection,
	                         TArray<UEdGraphPin*>& OutExternalOutputPins,
	                         TArray<UEdGraphPin*>& OutInternalInputPins)
	{
		for (UEdGraphNode* Node : Selection)
		{
			if (!IsValid(Node))
			{
				continue;
			}
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input
					|| Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					continue;
				}
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					UEdGraphNode* Other = Extract_ResolveThroughKnots(Linked, EGPD_Input);
					if (IsValid(Other) && !Selection.Contains(Other))
					{
						OutExternalOutputPins.Add(Linked);
						OutInternalInputPins.Add(Pin);
					}
				}
			}
		}
	}

	/** Test purity against the computed outgoing boundary and a fresh incoming-boundary walk. */
	bool Extract_IsExecFreeGivenBoundary(const TSet<UEdGraphNode*>& Selection,
	                                     const TArray<FBoundaryEdge>& ExecLeaving,
	                                     FString& OutWhyNot)
	{
		if (Selection.Num() == 0)
		{
			OutWhyNot = TEXT("the selection is empty");
			return false;
		}

		for (UEdGraphNode* Node : Selection)
		{
			// Use the shared purity predicate without inheriting lint's single-consumer restriction.
			if (!ClaireonExecTopology::IsPureNonKnot(Node))
			{
				OutWhyNot = FString::Printf(
					TEXT("node %s (%s) is not pure"),
					*Extract_NodeId(Node),
					IsValid(Node) ? *Node->GetClass()->GetName() : TEXT("<null>"));
				return false;
			}
		}

		if (ExecLeaving.Num() > 0)
		{
			OutWhyNot = FString::Printf(TEXT("%d exec edge(s) leave the selection"), ExecLeaving.Num());
			return false;
		}

		TArray<UEdGraphPin*> EntryExternalOutputs;
		TArray<UEdGraphPin*> EntryInternalInputs;
		Extract_FindEntries(Selection, EntryExternalOutputs, EntryInternalInputs);
		if (EntryInternalInputs.Num() > 0)
		{
			OutWhyNot = FString::Printf(
				TEXT("%d exec edge(s) arrive at the selection"), EntryInternalInputs.Num());
			return false;
		}

		return true;
	}

	TSharedPtr<FJsonObject> Extract_BoundaryEvidence(const TArray<FBoundaryEdge>& ExecLeaving,
	                                                const TArray<FString>& CrossingData)
	{
		TSharedPtr<FJsonObject> Evidence = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ExecValues;
		for (const FBoundaryEdge& Edge : ExecLeaving)
		{
			ExecValues.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s.%s -> %s"),
				*Extract_NodeId(Edge.FromNode), *Edge.FromPin, *Extract_NodeId(Edge.ToNode))));
		}
		Evidence->SetArrayField(TEXT("exec_edges_leaving_selection"), ExecValues);
		TArray<TSharedPtr<FJsonValue>> DataValues;
		for (const FString& PinId : CrossingData)
		{
			DataValues.Add(MakeShared<FJsonValueString>(PinId));
		}
		Evidence->SetArrayField(TEXT("data_pins_crossing_boundary"), DataValues);
		return Evidence;
	}

	void Extract_AddCommonSchema(FToolSchemaBuilder& Builder)
	{
		Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
		Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
		Builder.AddString(TEXT("graph_name"), TEXT("Graph holding the selection. Defaults to the session's current graph."), false);
		Builder.AddString(TEXT("anchor_node_guid"), TEXT("Walk outward from this node instead of listing node_guids. Pair with traversal_depth."), false);
		Builder.AddNumber(TEXT("traversal_depth"), TEXT("Exec hops to walk out from anchor_node_guid (default 1). Reroutes are traversed but never selected."));
		Builder.AddString(TEXT("new_name"), TEXT("Name for the extracted graph. Omitted means the engine's default name."), false);
		Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
		{
			TSharedPtr<FJsonObject> GuidsProp = MakeShared<FJsonObject>();
			GuidsProp->SetStringField(TEXT("type"), TEXT("array"));
			TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
			Items->SetStringField(TEXT("type"), TEXT("string"));
			GuidsProp->SetObjectField(TEXT("items"), Items);
			GuidsProp->SetStringField(TEXT("description"),
				TEXT("Node guids to extract. The explicit selection; alternative to anchor_node_guid."));
			Builder.Properties->SetObjectField(TEXT("node_guids"), GuidsProp);
		}
	}
}

using namespace ClaireonExtractInternal;

namespace ClaireonExtractQuiescence
{
	FString FocusedGraphMismatchRefusal(const UEdGraph* Resolved, const UEdGraph* Focused)
	{
		if (Resolved == Focused && Resolved != nullptr)
		{
			return FString();
		}

		const FString FocusedName = (Focused != nullptr)
			? Focused->GetName()
			: FString(TEXT("(none -- the editor has no focused graph)"));
		return FString::Printf(
			TEXT("bp_extract_composite refuses: the editor's focused graph is '%s' but the "
			     "resolved target graph is '%s'. Composite extraction runs the editor's "
			     "CollapseNodes, which creates the composite in the FOCUSED graph and ignores "
			     "the graph this call named -- so proceeding would collapse nodes into a graph "
			     "the tool never named, and the gateway diff, which looks only at the named "
			     "graph, would then report that nothing was produced. Refused BEFORE any "
			     "mutation: no transaction was opened and no mutating API was called, so the "
			     "asset is untouched. Focus the intended graph first (bp_switch_graph, which "
			     "requests editor focus on the graph it switches to), then re-issue."),
			*FocusedName,
			Resolved != nullptr ? *Resolved->GetName() : TEXT("(none)"));
	}
}

namespace ClaireonExtractSemantics
{
	namespace ClaireonExtractSemanticsInternal
	{
		/** Process-wide diagnostic tally. Game thread only; these tools are all game-thread. */
		int32 ClaireonExtractSemantics_TotalDiagnostics = 0;
		int32 ClaireonExtractSemantics_TokenlessDiagnostics = 0;

		FString ClaireonExtractSemantics_PinId(const UEdGraphPin* Pin)
		{
			const UEdGraphNode* Owner = Pin ? Pin->GetOwningNodeUnchecked() : nullptr;
			return FString::Printf(TEXT("%s.%s"),
				*Extract_NodeId(Owner),
				Pin ? *Pin->PinName.ToString() : TEXT("<null>"));
		}

		bool ClaireonExtractSemantics_IsExec(const UEdGraphPin* Pin)
		{
			return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
		}
	}

	using namespace ClaireonExtractSemanticsInternal;

	const TCHAR* ToWireString(EEnclosingLocalReason Reason)
	{
		switch (Reason)
		{
		case EEnclosingLocalReason::ExplicitWrite:              return TEXT("explicit_write");
		case EEnclosingLocalReason::ReachesMutableReferencePin: return TEXT("reaches_mutable_reference_pin");
		case EEnclosingLocalReason::ReadOnlyReference:          return TEXT("read_only_reference");
		}
		return TEXT("unknown");
	}

	bool IsArrayParmPin(const UEdGraphPin* Pin)
	{
		if (!Pin)
		{
			return false;
		}
		const UK2Node_CallFunction* Call = Cast<const UK2Node_CallFunction>(Pin->GetOwningNodeUnchecked());
		if (!IsValid(Call))
		{
			return false;
		}
		const UFunction* TargetFunction = Call->GetTargetFunction();
		if (TargetFunction == nullptr)
		{
			return false;
		}

		// ArrayParm metadata uses comma-separated groups with pipe-separated pin names.
		const FString& ArrayParmMetaData = TargetFunction->GetMetaData(FBlueprintMetadata::MD_ArrayParam);
		if (ArrayParmMetaData.IsEmpty())
		{
			return false;
		}

		const FString PinName = Pin->PinName.ToString();
		TArray<FString> Combos;
		ArrayParmMetaData.ParseIntoArray(Combos, TEXT(","), true);
		for (const FString& Combo : Combos)
		{
			TArray<FString> Names;
			Combo.ParseIntoArray(Names, TEXT("|"), true);
			if (Names.Num() > 0 && Names[0].Equals(PinName))
			{
				return true;
			}
		}
		return false;
	}

	bool ReachesMutableReferencePin(const UEdGraphPin* Pin, TArray<FString>& OutReachedReferencePins)
	{
		if (!Pin)
		{
			return false;
		}

		// Walk pins forward through knots and pure nodes; reaching an input does not imply following its node outputs.
		TSet<const UEdGraphPin*> Visited;
		TArray<const UEdGraphPin*> Frontier;
		Frontier.Add(Pin);
		Visited.Add(Pin);

		bool bFound = false;
		int32 Guard = 0;
		while (Frontier.Num() > 0 && Guard++ < 8192)
		{
			const UEdGraphPin* Current = Frontier.Pop(EAllowShrinking::No);
			for (const UEdGraphPin* Linked : Current->LinkedTo)
			{
				if (!Linked || Visited.Contains(Linked))
				{
					continue;
				}
				Visited.Add(Linked);

				if (ClaireonExtractSemantics_IsExec(Linked))
				{
					continue;
				}

				const UEdGraphNode* Owner = Linked->GetOwningNodeUnchecked();
				if (!IsValid(Owner))
				{
					continue;
				}

				// Test impure-node inputs too. ArrayParm references can mutate despite bIsConst
				// derived from a CustomThunk's native stub signature.
				if (Linked->PinType.bIsReference
					&& (!Linked->PinType.bIsConst || IsArrayParmPin(Linked)))
				{
					bFound = true;
					OutReachedReferencePins.AddUnique(ClaireonExtractSemantics_PinId(Linked));

				}

				// Propagate only through presentation (knots) and pure computation. An impure
				// node's outputs are a different value, not the local, so the walk stops there.
				if (!ClaireonExecTopology::IsKnot(Owner) && !ClaireonExecTopology::IsPureNonKnot(Owner))
				{
					continue;
				}
				for (const UEdGraphPin* Onward : Owner->Pins)
				{
					if (Onward && Onward->Direction == EGPD_Output
						&& !ClaireonExtractSemantics_IsExec(Onward) && !Visited.Contains(Onward))
					{
						Visited.Add(Onward);
						Frontier.Add(Onward);
					}
				}
			}
		}

		OutReachedReferencePins.Sort();
		return bFound;
	}

	void FindEnclosingLocalViolations(const TSet<UEdGraphNode*>& Selection,
	                                  TArray<FEnclosingLocalFinding>& OutFindings)
	{
		for (UEdGraphNode* Node : Selection)
		{
			const UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(Node);
			if (!IsValid(VariableNode))
			{
				continue;
			}

			// IsLocalScope excludes member variables, which resolve on self.
			if (!VariableNode->VariableReference.IsLocalScope())
			{
				continue;
			}

			const FString VariableName = VariableNode->VariableReference.GetMemberName().ToString();

			if (Node->IsA<UK2Node_VariableSet>())
			{
				FEnclosingLocalFinding Finding;
				Finding.VariableName = VariableName;
				Finding.NodeGuid = Extract_NodeId(Node);
				Finding.Reason = EEnclosingLocalReason::ExplicitWrite;
				OutFindings.Add(MoveTemp(Finding));
				continue;
			}

			// Record read-only Gets too; only opt-in promotion can accept them.
			// Walk outputs to detect mutations without a Set node.
			TArray<FString> ReachedReferencePins;
			for (const UEdGraphPin* Output : Node->Pins)
			{
				if (Output && Output->Direction == EGPD_Output && !ClaireonExtractSemantics_IsExec(Output))
				{
					ReachesMutableReferencePin(Output, ReachedReferencePins);
				}
			}

			FEnclosingLocalFinding Finding;
			Finding.VariableName = VariableName;
			Finding.NodeGuid = Extract_NodeId(Node);
			if (ReachedReferencePins.Num() > 0)
			{
				ReachedReferencePins.Sort();
				Finding.Reason = EEnclosingLocalReason::ReachesMutableReferencePin;
				Finding.ReachedReferencePins = MoveTemp(ReachedReferencePins);
			}
			else
			{
				Finding.Reason = EEnclosingLocalReason::ReadOnlyReference;
			}
			OutFindings.Add(MoveTemp(Finding));
		}

		OutFindings.Sort([](const FEnclosingLocalFinding& A, const FEnclosingLocalFinding& B)
		{
			return A.NodeGuid < B.NodeGuid;
		});
	}

	FString EnclosingLocalRefusal(const TCHAR* ToolName, const TArray<FEnclosingLocalFinding>& Findings)
	{
		if (Findings.Num() == 0)
		{
			return FString();
		}

		TArray<FString> Written;
		TArray<FString> MutablyReferenced;
		TArray<FString> ReadOnly;
		for (const FEnclosingLocalFinding& Finding : Findings)
		{
			switch (Finding.Reason)
			{
			case EEnclosingLocalReason::ExplicitWrite:
				Written.AddUnique(Finding.VariableName);
				break;
			case EEnclosingLocalReason::ReachesMutableReferencePin:
				MutablyReferenced.AddUnique(Finding.VariableName);
				break;
			default:
				ReadOnly.AddUnique(Finding.VariableName);
				break;
			}
		}

		// Group violations by remedy; promotion accepts only read-only references.
		FString Detail;
		if (ReadOnly.Num() > 0)
		{
			Detail += FString::Printf(
				TEXT(" Read only: %s -- this is the group promotion accepts. Pass ")
				TEXT("promote_enclosing_locals=true to bp_extract_function to turn each of them ")
				TEXT("into an input parameter wired at the call site."),
				*FString::Join(ReadOnly, TEXT(", ")));
		}
		if (Written.Num() > 0)
		{
			Detail += FString::Printf(
				TEXT(" Written by a Set node: %s -- refused even when promotion is enabled, ")
				TEXT("because rewiring a read/write local needs dominance analysis."),
				*FString::Join(Written, TEXT(", ")));
		}
		if (MutablyReferenced.Num() > 0)
		{
			Detail += FString::Printf(
				TEXT(" Read into a by-reference, non-const pin, which mutates the local with no Set ")
				TEXT("node anywhere: %s -- also refused even when promotion is enabled, because ")
				TEXT("promoting it as an ordinary input would silently discard the mutation."),
				*FString::Join(MutablyReferenced, TEXT(", ")));
		}

		return FString::Printf(
			TEXT("%s refuses: the selection reads or writes %d local variable(s) or parameter(s) of the ")
			TEXT("ENCLOSING graph.%s The engine's collapse converts only WIRED boundary pins into ")
			TEXT("parameters, and these are read by nodes INSIDE the selection, so nothing crosses the ")
			TEXT("boundary to convert and the extracted graph would reference locals that do not exist ")
			TEXT("in it -- 'Unable to find local variable with name ...' at compile time. Refused BEFORE ")
			TEXT("any mutation: no transaction was opened and no mutating API was called, so the asset ")
			TEXT("is untouched. Widen the selection to include whatever declares them, or promote them ")
			TEXT("to member variables, which resolve on self in any graph of the class."),
			ToolName,
			Findings.Num(),
			*Detail);
	}


	FEdGraphPinType PromotedParameterPinType(const FEdGraphPinType& SourcePinType)
	{
		// Preserve resolved qualifiers and container detail; the string grammar loses them.
		FEdGraphPinType PinType = SourcePinType;

		// Match engine boundary promotion: strip weak pointers only outside containers.
		if (PinType.bIsWeakPointer && !PinType.IsContainer())
		{
			PinType.bIsWeakPointer = false;
		}

		// Promoted parameters are input copies, not in/out references.
		PinType.bIsReference = false;

		return PinType;
	}

	bool PinPrecedesByConnectionPosition(const UEdGraphPin* A, const UEdGraphPin* B)
	{
		if (A == B)
		{
			return false;
		}
		if (!A)
		{
			return false;
		}
		if (!B)
		{
			return true;
		}

		// Match the engine pin comparator, including exec-first ordering.
		const bool bAExec = ClaireonExtractSemantics_IsExec(A);
		const bool bBExec = ClaireonExtractSemantics_IsExec(B);
		if (bAExec != bBExec)
		{
			return bAExec;
		}

		const UEdGraphNode* NodeA = A->GetOwningNodeUnchecked();
		const UEdGraphNode* NodeB = B->GetOwningNodeUnchecked();
		if (!IsValid(NodeA) || !IsValid(NodeB))
		{
			return NodeA != nullptr;
		}

		if (NodeA->NodePosY != NodeB->NodePosY)
		{
			return NodeA->NodePosY < NodeB->NodePosY;
		}
		if (NodeA->NodePosX != NodeB->NodePosX)
		{
			return NodeA->NodePosX < NodeB->NodePosX;
		}
		return NodeA->Pins.IndexOfByKey(A) < NodeB->Pins.IndexOfByKey(B);
	}

	bool BuildPromotionPlan(const TSet<UEdGraphNode*>& Selection,
	                        const TArray<FEnclosingLocalFinding>& Findings,
	                        TArray<FPromotionCandidate>& OutPlan,
	                        FString& OutRefusal)
	{
		OutPlan.Reset();
		OutRefusal.Reset();

		// Promotion cannot preserve writes through an input copy.
		TArray<FString> Written;
		TArray<FString> MutablyReferenced;
		for (const FEnclosingLocalFinding& Finding : Findings)
		{
			if (Finding.Reason == EEnclosingLocalReason::ExplicitWrite)
			{
				Written.AddUnique(Finding.VariableName);
			}
			else if (Finding.Reason == EEnclosingLocalReason::ReachesMutableReferencePin)
			{
				MutablyReferenced.AddUnique(Finding.VariableName);
			}
		}
		if (Written.Num() > 0 || MutablyReferenced.Num() > 0)
		{
			FString Detail;
			if (Written.Num() > 0)
			{
				Detail += FString::Printf(TEXT(" Written by a Set node: %s."),
					*FString::Join(Written, TEXT(", ")));
			}
			if (MutablyReferenced.Num() > 0)
			{
				Detail += FString::Printf(
					TEXT(" Read into a by-reference, non-const pin, which mutates the local with no ")
					TEXT("Set node anywhere: %s."),
					*FString::Join(MutablyReferenced, TEXT(", ")));
			}
			OutRefusal = FString::Printf(
				TEXT("promote_enclosing_locals accepts genuinely READ-ONLY locals only.%s Promoting ")
				TEXT("either shape as an ordinary input parameter would silently discard the ")
				TEXT("mutation, so both stay refused: write promotion needs dominance and ")
				TEXT("intervening-write analysis and is a separate follow-up. Narrow the selection ")
				TEXT("to exclude them, or promote them to member variables, which resolve on self ")
				TEXT("in any graph of the class."),
				*Detail);
			return false;
		}

		struct FPromotionGroup
		{
			FString VariableName;
			TArray<UEdGraphNode*> Nodes;
			UEdGraphPin* EarliestValuePin = nullptr;
			UEdGraphNode* EarliestNode = nullptr;
		};
		TArray<FPromotionGroup> Groups;
		TMap<FString, int32> GroupByName;

		// Findings are GUID-sorted, keeping group discovery independent of pointer-hash order.
		TMap<FString, UEdGraphNode*> NodesByGuid;
		for (UEdGraphNode* Node : Selection)
		{
			if (IsValid(Node))
			{
				NodesByGuid.Add(Extract_NodeId(Node), Node);
			}
		}

		for (const FEnclosingLocalFinding& Finding : Findings)
		{
			UEdGraphNode** Found = NodesByGuid.Find(Finding.NodeGuid);
			UK2Node_Variable* VariableNode = Found ? Cast<UK2Node_Variable>(*Found) : nullptr;
			if (!IsValid(VariableNode))
			{
				OutRefusal = FString::Printf(
					TEXT("promote_enclosing_locals could not re-resolve the refused reference to ")
					TEXT("'%s' (node %s) inside the selection, so no promotion plan can be built. ")
					TEXT("Nothing was mutated."),
					*Finding.VariableName, *Finding.NodeGuid);
				return false;
			}

			UEdGraphPin* ValuePin = VariableNode->GetValuePin();
			if (!ValuePin)
			{
				OutRefusal = FString::Printf(
					TEXT("promote_enclosing_locals refuses: the Get of '%s' (node %s) has no value ")
					TEXT("pin, so there is no resolved pin type to copy into a parameter. Nothing ")
					TEXT("was mutated."),
					*Finding.VariableName, *Finding.NodeGuid);
				return false;
			}

			// Promotion rewires only the parent value pin. Connected subpins would be
			// disconnected when the Get is deleted, even if consumers compile on defaults.
			{
				TArray<UEdGraphPin*> SubPinStack = ValuePin->SubPins;
				while (SubPinStack.Num() > 0)
				{
					UEdGraphPin* SubPin = SubPinStack.Pop();
					if (!SubPin)
					{
						continue;
					}
					if (SubPin->LinkedTo.Num() > 0)
					{
						OutRefusal = FString::Printf(
							TEXT("promote_enclosing_locals refuses: the Get of '%s' (node %s) has a SPLIT ")
							TEXT("value pin with connected field subpins ('%s' is linked). Promotion rewires ")
							TEXT("only the parent value pin, so the split-field consumers would be silently ")
							TEXT("disconnected. Recombine the struct pin on that Get first, or exclude it ")
							TEXT("from the selection. Reason: split_pin_unsupported. Nothing was mutated."),
							*Finding.VariableName, *Finding.NodeGuid, *SubPin->PinName.ToString());
						return false;
					}
					SubPinStack.Append(SubPin->SubPins);
				}
			}

			// An external consumer becomes a result parameter during collapse; deleting
			// the Get would strand that output.
			for (const UEdGraphPin* Linked : ValuePin->LinkedTo)
			{
				UEdGraphNode* LinkedNode = Linked ? Linked->GetOwningNodeUnchecked() : nullptr;
				if (!IsValid(LinkedNode) || !Selection.Contains(LinkedNode))
				{
					OutRefusal = FString::Printf(
						TEXT("promote_enclosing_locals refuses: the Get of '%s' (node %s) is also read ")
						TEXT("by a node OUTSIDE the selection (%s). The engine's collapse would turn ")
						TEXT("that link into a return value of the extracted function, and removing ")
						TEXT("the Get would then strand it. Either include the outside consumer in ")
						TEXT("the selection or exclude this Get. Reason: ")
						TEXT("read_reaches_outside_selection. Nothing was mutated."),
						*Finding.VariableName, *Finding.NodeGuid,
						IsValid(LinkedNode) ? *Extract_NodeId(LinkedNode) : TEXT("<unresolved>"));
					return false;
				}
			}

			int32* ExistingIndex = GroupByName.Find(Finding.VariableName);
			if (!ExistingIndex)
			{
				FPromotionGroup Group;
				Group.VariableName = Finding.VariableName;
				Group.Nodes.Add(VariableNode);
				Group.EarliestValuePin = ValuePin;
				Group.EarliestNode = VariableNode;
				GroupByName.Add(Finding.VariableName, Groups.Add(MoveTemp(Group)));
				continue;
			}

			FPromotionGroup& Group = Groups[*ExistingIndex];
			Group.Nodes.Add(VariableNode);
			if (PinPrecedesByConnectionPosition(ValuePin, Group.EarliestValuePin))
			{
				Group.EarliestValuePin = ValuePin;
				Group.EarliestNode = VariableNode;
			}
		}

		// Order by the earliest Get's position, with variable name breaking ties.
		Groups.Sort([](const FPromotionGroup& A, const FPromotionGroup& B)
		{
			if (PinPrecedesByConnectionPosition(A.EarliestValuePin, B.EarliestValuePin))
			{
				return true;
			}
			if (PinPrecedesByConnectionPosition(B.EarliestValuePin, A.EarliestValuePin))
			{
				return false;
			}
			return A.VariableName < B.VariableName;
		});

		for (const FPromotionGroup& Group : Groups)
		{
			FPromotionCandidate Candidate;
			Candidate.VariableName = Group.VariableName;
			Candidate.ParameterPinType = PromotedParameterPinType(Group.EarliestValuePin->PinType);
			Candidate.EarliestNodeGuid = Extract_NodeId(Group.EarliestNode);
			Candidate.ReferencingGetNodes = Group.Nodes;

			// Capture sort keys before collapse repositions the nodes.
			Candidate.SortNodePosY = Group.EarliestNode->NodePosY;
			Candidate.SortNodePosX = Group.EarliestNode->NodePosX;
			Candidate.SortPinIndex = Group.EarliestNode->Pins.IndexOfByKey(Group.EarliestValuePin);

			OutPlan.Add(MoveTemp(Candidate));
		}

		return true;
	}

	bool IsSelectionExecFree(const TSet<UEdGraphNode*>& Selection, FString& OutWhyNot)
	{
		TArray<FBoundaryEdge> ExecLeaving;
		TArray<FString> CrossingData;
		Extract_FindBoundary(Selection, ExecLeaving, CrossingData);
		return Extract_IsExecFreeGivenBoundary(Selection, ExecLeaving, OutWhyNot);
	}

	void GetDiagnosticTokenTally(int32& OutTotalDiagnostics, int32& OutTokenlessDiagnostics)
	{
		OutTotalDiagnostics = ClaireonExtractSemantics_TotalDiagnostics;
		OutTokenlessDiagnostics = ClaireonExtractSemantics_TokenlessDiagnostics;
	}

	void ResetDiagnosticTokenTally()
	{
		ClaireonExtractSemantics_TotalDiagnostics = 0;
		ClaireonExtractSemantics_TokenlessDiagnostics = 0;
	}

	/** Recorded by the collection pass below. Internal, not part of the public surface. */
	void ClaireonExtractSemantics_AddToTally(int32 Total, int32 Tokenless)
	{
		ClaireonExtractSemantics_TotalDiagnostics += Total;
		ClaireonExtractSemantics_TokenlessDiagnostics += Tokenless;
	}
}

namespace ClaireonExtractApplyInternal
{
	// Observe retained effects independently of transaction status.
	// Cancel removes the undo record without restoring objects; commit retains an undo record.

	/** Capture every Blueprint graph, including nested graphs created or moved by collapse. */
	TArray<UEdGraph*> Extract_SnapshotGraphs(UBlueprint* Blueprint)
	{
		TArray<UEdGraph*> Graphs;
		if (IsValid(Blueprint))
		{
			Blueprint->GetAllGraphs(Graphs);
		}
		return Graphs;
	}

	/** What a post-transaction return reports about itself. */
	struct FExtractOutcome
	{
		EClaireonMutationState State = EClaireonMutationState::AppliedOperationFailed;

		/** Wire string of the phase that failed, or empty when no phase failed. */
		FString FailedPhase;

		FString LastCompletedPhase = kClaireonBPPhaseNone;

		TOptional<EClaireonEngineCompileStatus> CompileStatus;

		/** The tool records cancellation status; the buffer cannot reliably attribute undo availability after cancellation. */
		bool bUndoRecordAvailable = false;

		/** True where the asset is known to be left dirty and uncompiled. */
		bool bDirtyAndUncompiled = false;

		/** Record the targeted graph because failure can leave the session cursor on another graph. */
		FString TargetGraph;

		/** True when the failure return skipped the session's current-graph update. */
		bool bSessionGraphStale = false;
	};

	/** Recovery guidance must fit the inline scalar bound and offer only available undo. */
	TSharedPtr<FJsonObject> Extract_RecoveryHint(const FClaireonBPMutationResult& Envelope)
	{
		if (Envelope.bRollbackAvailable && !Envelope.bRollbackGroupSafe)
		{
			// Close the editor-wide group before unrelated work can enter it.
			return IClaireonTool::MakeGuidanceHint(
				TEXT("transaction_end_group"),
				TEXT("The extraction is retained and your transaction group is still open, so "
				     "every later edit is being swept into it. Rollback safety could not be "
				     "established (pending third-party formatting is not provably settled), so "
				     "transaction_end_group is the safe terminal action; do not roll back."));
		}

		if (Envelope.bUndoRecordAvailable)
		{
			// Warn that collapse undo can restore graph membership without restoring node Outer.
			return IClaireonTool::MakeGuidanceHint(
				TEXT("transaction_undo"),
				TEXT("The extraction is retained on this asset and an undo record for it "
				     "exists. Read operation_delta for the full inventory of what remains. "
				     "PREFER REPAIRING FORWARD: undoing a collapse leaves the containing graph "
				     "still listing the collapsed node, whose Outer is the sub-graph the undo "
				     "removed, and SGraphPanel ensures on that mismatch the next time it "
				     "paints. If you do undo, undo once, and do not leave that graph open."));
		}

		// Do not offer undo after cancellation; it would target an earlier transaction.
		return IClaireonTool::MakeGuidanceHint(
			TEXT("bp_get_graph"),
			TEXT("The extraction is retained on this asset and NO undo record for it exists: "
			     "the transaction was canceled, which discards the record without reverting "
			     "anything. Do NOT undo -- that would revert an unrelated earlier "
			     "transaction. Inspect the graph, then repair forward from operation_delta."));
	}

	/** Build every post-transaction failure from an observed snapshot delta, not the phase journal. */
	FToolResult Extract_RetainedFailure(
		UBlueprint* Blueprint,
		const FString& SessionId,
		EExtractKind Kind,
		const FExtractOutcome& Outcome,
		const FClaireonBPSnapshot& Before,
		const FClaireonBPPhaseJournal& Journal,
		const FString& Message,
		const FClaireonBPSnapshot* PreCapturedAfter = nullptr)
	{
		// Reuse the post-operation snapshot already captured for structural validation.
		FClaireonBPSnapshot LocalAfter;
		if (!PreCapturedAfter)
		{
			ClaireonBPSnapshot::Capture(Blueprint, Extract_SnapshotGraphs(Blueprint),
				EClaireonBPSnapshotFamily::Extraction, LocalAfter);
		}
		const FClaireonBPSnapshot& After = PreCapturedAfter ? *PreCapturedAfter : LocalAfter;
		const FClaireonBPSnapshotDelta Delta = ClaireonBPSnapshot::Diff(Before, After);

		FClaireonBPMutationResult Envelope;
		Envelope.MutationState = Outcome.State;
		Envelope.bMutationRetained = ClaireonBPMutation::RetainsMutation(Outcome.State);
		Envelope.FailedPhase = Outcome.FailedPhase;
		Envelope.LastCompletedPhase = Outcome.LastCompletedPhase;
		Envelope.EngineCompileStatus = Outcome.CompileStatus;
		Envelope.bRollbackAvailable = ClaireonTransactionGroupState::bGroupActive;

		// No product-side settlement proof is available here, so retain the rollback guard.
		Envelope.bRollbackGroupSafe = false;
		Envelope.bUndoRecordAvailable = Outcome.bUndoRecordAvailable;
		Envelope.AssetPath = IsValid(Blueprint) ? Blueprint->GetPathName() : FString();
		Envelope.SessionId = SessionId;

		FToolResult Result = IClaireonTool::MakeErrorResult(Message);
		Result.Data = MakeShared<FJsonObject>();
		Envelope.WriteInlineScalars(*Result.Data);

		Result.Data->SetStringField(TEXT("extract_kind"), Extract_KindName(Kind));

		// Report the one tool-owned transaction separately from asynchronous plugin work.
		Result.Data->SetObjectField(TEXT("claireon_transactions"),
			ClaireonBPMutation::MakeClaireonTransactionsReport(
				{Extract_TransactionTitle(Kind).ToString()},
				Envelope.bRollbackAvailable));

		if (TSharedPtr<FJsonObject> DeltaJson = Delta.ToJson(); DeltaJson.IsValid())
		{
			Result.Data->SetObjectField(TEXT("operation_delta"), DeltaJson);
		}

		if (!Outcome.FailedPhase.IsEmpty())
		{
			FClaireonBPPhaseJournal PhaseOnly;
			PhaseOnly.Entries = Journal.EntriesForPhase(Outcome.FailedPhase);
			if (TSharedPtr<FJsonObject> PhaseJson = PhaseOnly.ToJson(); PhaseJson.IsValid())
			{
				Result.Data->SetObjectField(TEXT("failed_phase_delta"), PhaseJson);
			}
		}
		if (TSharedPtr<FJsonObject> JournalJson = Journal.ToJson(); JournalJson.IsValid())
		{
			Result.Data->SetObjectField(TEXT("phase_journal"), JournalJson);
		}

		// Skipped compilation leaves the retained mutation dirty and uncompiled.
		if (Outcome.bDirtyAndUncompiled)
		{
			Result.Data->SetBoolField(TEXT("asset_dirty"), true);
			Result.Data->SetBoolField(TEXT("asset_compiled"), false);
		}

		// Report the target graph and whether the session cursor was updated.
		if (!Outcome.TargetGraph.IsEmpty())
		{
			Result.Data->SetStringField(TEXT("target_graph"), Outcome.TargetGraph);
		}
		if (Outcome.bSessionGraphStale)
		{
			Result.Data->SetBoolField(TEXT("session_graph_stale"), true);
		}

		Result.AddHint(Extract_RecoveryHint(Envelope));
		return Result;
	}

	// Validate purity and preserved execution structurally, independent of diagnostic wording.

	/** Shared compiler diagnostic captured by value. */
	using FExtractDiagnostic = ClaireonBPFunctionRecipe::FCompilerDiagnostic;

	/** Collect diagnostics and update the extraction-specific tokenless tally. */
	void Extract_CollectDiagnostics(const FCompilerResultsLog& Log,
	                                TArray<FExtractDiagnostic>& OutDiagnostics,
	                                int32& OutTokenless)
	{
		ClaireonBPFunctionRecipe::CollectCompilerDiagnostics(Log, OutDiagnostics, OutTokenless);

		ClaireonExtractSemantics::ClaireonExtractSemantics_AddToTally(OutDiagnostics.Num(), OutTokenless);

		// Log tokenless diagnostics individually under a stable prefix.
		UE_LOG(LogClaireon, Log,
			TEXT("CLAIREON_EXTRACT_DIAG_TALLY total=%d tokenless=%d errors=%d warnings=%d"),
			OutDiagnostics.Num(), OutTokenless, Log.NumErrors, Log.NumWarnings);
		for (const FExtractDiagnostic& Diagnostic : OutDiagnostics)
		{
			if (!Diagnostic.bHasNode)
			{
				UE_LOG(LogClaireon, Log,
					TEXT("CLAIREON_EXTRACT_DIAG_TOKENLESS severity=%s id='%s' message='%s'"),
					*Diagnostic.SeverityLabel,
					*Diagnostic.Identifier,
					*Diagnostic.Message);
			}
		}
	}

	/** Attach diagnostics on every return that ran a compile. */
	void Extract_AttachDiagnostics(FToolResult& Result,
	                              const TArray<FExtractDiagnostic>& Diagnostics,
	                              int32 TokenlessDiagnostics)
	{
		ClaireonBPFunctionRecipe::AttachDiagnostics(Result, Diagnostics, TokenlessDiagnostics);
	}

	/** Extract affected GUIDs from the reported delta's node, pin, and link targets. */
	TSet<FGuid> Extract_DeltaNodeGuids(const FClaireonBPSnapshotDelta& Delta)
	{
		TSet<FGuid> Guids;
		for (const FClaireonBPDeltaEntry& Entry : Delta.Entries)
		{
			if (Entry.EffectClass != EClaireonBPDurableEffectClass::GraphTopology)
			{
				continue;
			}

			FString Target = Entry.Target;
			int32 HashIndex = INDEX_NONE;
			if (Target.FindChar(TEXT('#'), HashIndex))
			{
				Target.LeftInline(HashIndex);
			}

			FString GuidText;
			if (Target.StartsWith(TEXT("node:")))
			{
				GuidText = Target.RightChop(5);
			}
			else if (Target.StartsWith(TEXT("pin:")))
			{
				GuidText = Target.RightChop(4);
			}
			else if (Target.StartsWith(TEXT("link:")))
			{
				GuidText = Target.RightChop(5);
			}
			else
			{
				continue;
			}

			int32 SlashIndex = INDEX_NONE;
			if (GuidText.FindChar(TEXT('/'), SlashIndex))
			{
				GuidText.LeftInline(SlashIndex);
			}

			FGuid Parsed;
			if (FGuid::Parse(GuidText, Parsed))
			{
				Guids.Add(Parsed);
			}
		}
		return Guids;
	}

	/**
	 * Require the live pure-function gateway to have no non-orphan exec pins.
	 * It has no pre-operation snapshot; flag checks alone cannot validate its pins.
	 */
	bool Extract_GatewayHasNoExecPins(const UEdGraphNode* Gateway, FString& OutOffendingPins)
	{
		if (!IsValid(Gateway))
		{
			OutOffendingPins = TEXT("the gateway node is no longer valid");
			return false;
		}

		TArray<FString> Offenders;
		for (const UEdGraphPin* Pin : Gateway->Pins)
		{
			// UEdGraphSchema_K2::IsExecPin, the same predicate Extract_FindBoundary uses.
			if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && !Pin->bOrphanedPin)
			{
				Offenders.Add(Pin->PinName.ToString());
			}
		}
		if (Offenders.Num() == 0)
		{
			return true;
		}
		OutOffendingPins = FString::Join(Offenders, TEXT(", "));
		return false;
	}

	/**
	 * Find formerly connected exec inputs that remain present but lose all links.
	 * Match node GUID and pin ID; skip absent nodes/pins and post-operation orphans,
	 * which are represented separately in the operation delta.
	 */
	void Extract_FindLostExecInputs(const FClaireonBPSnapshot& Before,
	                               const FClaireonBPSnapshot& After,
	                               TArray<FString>& OutViolations)
	{
		for (const TPair<FGuid, FClaireonBPGraphTopologySnapshot>& GraphPair : Before.Graphs)
		{
			for (const TPair<FGuid, FClaireonBPNodeSnapshot>& NodePair : GraphPair.Value.Nodes)
			{
				const FClaireonBPNodeSnapshot* AfterNode = After.FindNode(NodePair.Key);
				if (!AfterNode)
				{
					continue;
				}
				for (const TPair<FGuid, FClaireonBPPinSnapshot>& PinPair : NodePair.Value.Pins)
				{
					const FClaireonBPPinSnapshot& BeforePin = PinPair.Value;
					if (!BeforePin.bIsExec || !BeforePin.bIsInput || BeforePin.Links.Num() == 0)
					{
						continue;
					}
					const FClaireonBPPinSnapshot* AfterPin = AfterNode->Pins.Find(PinPair.Key);
					if (!AfterPin || AfterPin->bOrphaned)
					{
						continue;
					}
					if (AfterPin->Links.Num() == 0)
					{
						OutViolations.Add(FString::Printf(
							TEXT("%s.%s lost its %d incoming exec edge(s)"),
							*NodePair.Key.ToString(EGuidFormats::DigitsWithHyphens),
							*BeforePin.PinName.ToString(),
							BeforePin.Links.Num()));
					}
				}
			}
		}
		OutViolations.Sort();
	}

	/**
	 * Apply purity through the shared flag recipe, including entry and call-site
	 * reconstruction. The gateway must lose exec pins; the entry keeps its own Then pin.
	 */
	bool Extract_MakeFunctionPure(UBlueprint* Blueprint, UEdGraph* FunctionGraph,
	                              FClaireonBPPhaseJournal& Journal, FString& OutError)
	{
		ClaireonBPFunctionRecipe::FFlagRecipeSteps Steps;
		Steps.FlagPhase = ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::PurityUpdate);
		Steps.EntryReconstructionPhase =
			ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::EntryReconstruction);
		Steps.CallsiteRefreshPhase =
			ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::CallsiteRefresh);
		Steps.bReconstructEntry = true;
		Steps.bRefreshCallSites = true;

		return ClaireonBPFunctionRecipe::ApplyFunctionFlagChange(
			Blueprint, FunctionGraph, /*SetMask=*/FUNC_BlueprintPure, /*ClearMask=*/0,
			Steps, Journal, OutError);
	}


	/** Render UserDefinedPins as the durable signature; live Pins also contains the entry's Then pin. */
	FString Extract_DescribeEntrySignature(const UK2Node_EditablePinBase* EntryNode)
	{
		if (!IsValid(EntryNode))
		{
			return FString();
		}
		TArray<FString> Parts;
		for (const TSharedPtr<FUserPinInfo>& UserPin : EntryNode->UserDefinedPins)
		{
			if (!UserPin.IsValid())
			{
				continue;
			}
			Parts.Add(FString::Printf(TEXT("%s: %s"),
				*UserPin->PinName.ToString(),
				*UEdGraphSchema_K2::TypeToText(UserPin->PinType).ToString()));
		}
		return FString::Join(Parts, TEXT(", "));
	}

	/**
	 * Promote planned read-only locals inside the extraction transaction after rename and purity repair.
	 * Copy full pin types, read back allocated names, and refresh the gateway before wiring its new inputs.
	 */
	bool Extract_PromoteEnclosingLocals(
		UBlueprint* Blueprint,
		UEdGraph* SourceGraph,
		UEdGraph* FunctionGraph,
		UEdGraphNode* Gateway,
		const TArray<ClaireonExtractSemantics::FPromotionCandidate>& Plan,
		FClaireonBPPhaseJournal& Journal,
		TArray<TSharedPtr<FJsonObject>>& OutPromoted,
		FString& OutSignatureBefore,
		FString& OutSignatureAfter,
		FString& OutError)
	{
		const TCHAR* const SynthesisPhase =
			ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::ParameterSynthesis);

		if (!IsValid(Blueprint) || !IsValid(SourceGraph) || !IsValid(FunctionGraph) || !IsValid(Gateway))
		{
			OutError = TEXT("no Blueprint, source graph, extracted function graph or gateway node");
			return false;
		}

		UK2Node_FunctionEntry* EntryNode = nullptr;
		for (UEdGraphNode* Node : FunctionGraph->Nodes)
		{
			if (UK2Node_FunctionEntry* Candidate = Cast<UK2Node_FunctionEntry>(Node); IsValid(Candidate))
			{
				EntryNode = Candidate;
				break;
			}
		}
		if (!IsValid(EntryNode))
		{
			OutError = FString::Printf(
				TEXT("graph '%s' has no UK2Node_FunctionEntry to hang parameters on"),
				*FunctionGraph->GetName());
			return false;
		}

		OutSignatureBefore = Extract_DescribeEntrySignature(EntryNode);

		EntryNode->Modify();
		SourceGraph->Modify();
		FunctionGraph->Modify();

		// Retain identities for lookup after refresh invalidates pin pointers.
		struct FCallSiteWiring
		{
			FName ParameterName;
			UK2Node_VariableGet* SourceRead = nullptr;
		};
		TArray<FCallSiteWiring> Wirings;

		int32 CandidateIndex = 0;
		for (const ClaireonExtractSemantics::FPromotionCandidate& Candidate : Plan)
		{
			UEdGraphPin* EntryPin = EntryNode->CreateUserDefinedPin(
				FName(*Candidate.VariableName), Candidate.ParameterPinType, EGPD_Output);
			if (!EntryPin)
			{
				OutError = FString::Printf(
					TEXT("CreateUserDefinedPin returned no pin for local '%s'"), *Candidate.VariableName);
				return false;
			}
			const FName ParameterName = EntryPin->PinName;

			// Create the call-site getter before deleting its source references.
			UK2Node_Variable* Template = nullptr;
			for (UEdGraphNode* GetNode : Candidate.ReferencingGetNodes)
			{
				if (UK2Node_Variable* AsVariable = Cast<UK2Node_Variable>(GetNode); IsValid(AsVariable))
				{
					Template = AsVariable;
					break;
				}
			}
			if (!IsValid(Template))
			{
				OutError = FString::Printf(
					TEXT("every planned VariableGet of local '%s' went invalid between the plan and ")
					TEXT("the collapse"), *Candidate.VariableName);
				return false;
			}

			UK2Node_VariableGet* SourceRead = NewObject<UK2Node_VariableGet>(SourceGraph);
			SourceGraph->AddNode(SourceRead, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			SourceRead->CreateNewGuid();
			SourceRead->VariableReference = Template->VariableReference;

			// RenameGraph rewrites local MemberScope on moved getters. Restore the original top-level function scope
			// while preserving the template name and GUID.
			const UEdGraph* SourceScopeGraph = FBlueprintEditorUtils::GetTopLevelGraph(SourceGraph);
			const FString SourceScopeName = IsValid(SourceScopeGraph)
				? SourceScopeGraph->GetName() : SourceGraph->GetName();
			SourceRead->VariableReference.SetLocalMember(
				Template->VariableReference.GetMemberName(),
				SourceScopeName,
				Template->VariableReference.GetMemberGuid());

			SourceRead->NodePosX = Gateway->NodePosX - 280;
			SourceRead->NodePosY = Gateway->NodePosY + 48 * CandidateIndex;
			SourceRead->AllocateDefaultPins();

			// Copy links before rewiring consumers to the entry and deleting each getter.
			int32 RewiredLinks = 0;
			for (UEdGraphNode* GetNode : Candidate.ReferencingGetNodes)
			{
				UK2Node_Variable* VariableNode = Cast<UK2Node_Variable>(GetNode);
				if (!IsValid(VariableNode))
				{
					OutError = FString::Printf(
						TEXT("a planned VariableGet of local '%s' went invalid before rewiring"),
						*Candidate.VariableName);
					return false;
				}
				UEdGraphPin* ValuePin = VariableNode->GetValuePin();
				if (!ValuePin)
				{
					OutError = FString::Printf(
						TEXT("the VariableGet %s of local '%s' lost its value pin during the collapse"),
						*Extract_NodeId(VariableNode), *Candidate.VariableName);
					return false;
				}
				ValuePin->Modify();

				TArray<UEdGraphPin*> Consumers = ValuePin->LinkedTo;
				for (UEdGraphPin* Consumer : Consumers)
				{
					if (!Consumer)
					{
						continue;
					}
					Consumer->Modify();
					ValuePin->BreakLinkTo(Consumer);
					Consumer->MakeLinkTo(EntryPin);
					++RewiredLinks;
				}
			}

			for (UEdGraphNode* GetNode : Candidate.ReferencingGetNodes)
			{
				if (IsValid(GetNode))
				{
					FBlueprintEditorUtils::RemoveNode(Blueprint, GetNode, /*bDontRecompile=*/true);
				}
			}

			Journal.Append({SynthesisPhase, TEXT("local_promoted"),
				EntryNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
				FString::Printf(
					TEXT("'%s' -> parameter '%s' (%s); %d Get node(s) deleted, %d consumer link(s) ")
					TEXT("rewired to the entry pin, one call-site read created in '%s'"),
					*Candidate.VariableName, *ParameterName.ToString(),
					*UEdGraphSchema_K2::TypeToText(Candidate.ParameterPinType).ToString(),
					Candidate.ReferencingGetNodes.Num(), RewiredLinks, *SourceGraph->GetName())});

			TSharedPtr<FJsonObject> Record = MakeShared<FJsonObject>();
			Record->SetStringField(TEXT("variable"), Candidate.VariableName);
			Record->SetStringField(TEXT("parameter"), ParameterName.ToString());
			Record->SetStringField(TEXT("parameter_type"),
				UEdGraphSchema_K2::TypeToText(Candidate.ParameterPinType).ToString());
			Record->SetStringField(TEXT("earliest_get_node"), Candidate.EarliestNodeGuid);
			Record->SetNumberField(TEXT("get_nodes_deleted"), Candidate.ReferencingGetNodes.Num());
			Record->SetNumberField(TEXT("consumer_links_rewired"), RewiredLinks);
			Record->SetNumberField(TEXT("order_node_pos_y"), Candidate.SortNodePosY);
			Record->SetNumberField(TEXT("order_node_pos_x"), Candidate.SortNodePosX);
			Record->SetNumberField(TEXT("order_pin_index"), Candidate.SortPinIndex);
			Record->SetStringField(TEXT("callsite_read_node"), Extract_NodeId(SourceRead));
			OutPromoted.Add(Record);

			Wirings.Add({ParameterName, SourceRead});
			++CandidateIndex;
		}

		// Reconstruct the entry with orphan saving disabled, then broadcast the signature change.
		{
			const bool bPreviousDisableOrphanPinSaving = EntryNode->bDisableOrphanPinSaving;
			EntryNode->bDisableOrphanPinSaving = true;
			EntryNode->ReconstructNode();
			EntryNode->bDisableOrphanPinSaving = bPreviousDisableOrphanPinSaving;
		}
		Journal.Append({ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::EntryReconstruction),
			TEXT("entry_node_reconstructed_for_signature"),
			EntryNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
			FString::Printf(TEXT("%d synthesized parameter(s) now on UserDefinedPins"), Plan.Num())});

		GetDefault<UEdGraphSchema_K2>()->HandleParameterDefaultValueChanged(EntryNode);
		Journal.Append({ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::CallsiteRefresh),
			TEXT("callsite_refresh_broadcast_for_signature"), FunctionGraph->GetName(),
			TEXT("HandleParameterDefaultValueChanged regenerated the skeleton and reconstructed "
			     "every call site, which is what gives the gateway its new input pins")});

		// Resolve both pin endpoints after refresh; prior pointers are invalid.
		for (const FCallSiteWiring& Wiring : Wirings)
		{
			UEdGraphPin* GatewayPin = Gateway->FindPin(Wiring.ParameterName, EGPD_Input);
			if (!GatewayPin)
			{
				OutError = FString::Printf(
					TEXT("the gateway call node grew no input pin named '%s' after the call-site ")
					TEXT("refresh, so the promoted parameter has no value at the call site"),
					*Wiring.ParameterName.ToString());
				return false;
			}
			UEdGraphPin* SourceValuePin = IsValid(Wiring.SourceRead) ? Wiring.SourceRead->GetValuePin() : nullptr;
			if (!SourceValuePin)
			{
				// Report the unresolved local scope when a getter has no value pin.
				OutError = FString::Printf(
					TEXT("the call-site read created for parameter '%s' has no value pin -- its "
					     "reference is '%s' in local scope '%s', which did not resolve to a "
					     "property on the skeleton class"),
					*Wiring.ParameterName.ToString(),
					IsValid(Wiring.SourceRead)
						? *Wiring.SourceRead->VariableReference.GetMemberName().ToString()
						: TEXT("<node gone>"),
					IsValid(Wiring.SourceRead)
						? *Wiring.SourceRead->VariableReference.GetMemberScopeName()
						: TEXT("<node gone>"));
				return false;
			}
			GatewayPin->Modify();
			SourceValuePin->Modify();
			SourceValuePin->MakeLinkTo(GatewayPin);

			Journal.Append({ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::Wiring),
				TEXT("callsite_parameter_wired"), Extract_NodeId(Gateway),
				FString::Printf(TEXT("%s.%s <- %s"),
					*Extract_NodeId(Gateway), *Wiring.ParameterName.ToString(),
					*Extract_NodeId(Wiring.SourceRead))});
		}

		OutSignatureAfter = Extract_DescribeEntrySignature(EntryNode);
		return true;
	}

	/** Use the shared stack gutter so extraction and stacking agree on island spacing. */
	inline constexpr double kExtract_IslandGutter = ClaireonGraphIslands::DefaultStackGutter;

	/** Nonzero clearance between the event and extracted body before formatting. */
	inline constexpr double kExtract_BodyDropY = 128.0;

	/**
	 * Carry knots whose reachable non-knot endpoints are nonempty and all moved.
	 * Traverse complete knot chains and sort output by GUID. OutRounds includes the confirming pass.
	 */
	void Extract_CollectCarriedKnots(const UEdGraph* Graph, const TSet<UEdGraphNode*>& MovedNodes,
		TArray<UK2Node_Knot*>& OutKnots, int32& OutRounds)
	{
		OutKnots.Reset();
		OutRounds = 0;
		if (!IsValid(Graph) || MovedNodes.Num() == 0)
		{
			return;
		}

		TSet<UK2Node_Knot*> Carried;
		bool bAdmitted = true;
		while (bAdmitted)
		{
			bAdmitted = false;
			++OutRounds;

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node);
				if (!IsValid(Knot) || Carried.Contains(Knot))
				{
					continue;
				}

				// Traverse knots to collect real endpoints.
				TSet<UEdGraphNode*> Endpoints;
				TSet<UEdGraphNode*> SeenKnots;
				TArray<UEdGraphNode*> Frontier;
				Frontier.Add(Knot);
				SeenKnots.Add(Knot);
				while (Frontier.Num() > 0)
				{
					UEdGraphNode* Current = Frontier.Pop(EAllowShrinking::No);
					for (UEdGraphPin* Pin : Current->Pins)
					{
						if (!Pin)
						{
							continue;
						}
						for (UEdGraphPin* Linked : Pin->LinkedTo)
						{
							UEdGraphNode* Neighbour = Linked ? Linked->GetOwningNode() : nullptr;
							if (!IsValid(Neighbour))
							{
								continue;
							}
							if (Neighbour->IsA<UK2Node_Knot>())
							{
								if (!SeenKnots.Contains(Neighbour))
								{
									SeenKnots.Add(Neighbour);
									Frontier.Add(Neighbour);
								}
								continue;
							}
							Endpoints.Add(Neighbour);
						}
					}
				}

				if (Endpoints.Num() == 0)
				{
					continue;
				}

				bool bEveryEndpointMoved = true;
				for (UEdGraphNode* Endpoint : Endpoints)
				{
					if (!MovedNodes.Contains(Endpoint))
					{
						bEveryEndpointMoved = false;
						break;
					}
				}
				if (!bEveryEndpointMoved)
				{
					continue;
				}

				// Do not add knots to MovedNodes; eligibility depends only on non-knot endpoints.
				Carried.Add(Knot);
				bAdmitted = true;
			}
		}

		OutKnots = Carried.Array();
		OutKnots.Sort([](const UK2Node_Knot& A, const UK2Node_Knot& B)
		{
			return A.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)
				< B.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
		});
	}

	/** Shared extraction preflight, mutation, and validation. */
	FToolResult ApplyExtract(ClaireonBlueprintGraphEditToolBase& Tool, EExtractKind Kind,
	                         const FString& SessionId, FBlueprintEditToolData* Data,
	                         const TSharedPtr<FJsonObject>& Params)
	{
		UBlueprint* Blueprint = Data ? Data->Blueprint.Get() : nullptr;
		if (!IsValid(Blueprint))
		{
			return IClaireonTool::MakeErrorResult(TEXT("Session no longer references a valid Blueprint."));
		}

		UEdGraph* Graph = Data->Graph.Get();
		FString GraphName;
		if (Params->TryGetStringField(TEXT("graph_name"), GraphName) && !GraphName.IsEmpty())
		{
			Graph = nullptr;
			TArray<UEdGraph*> AllGraphs;
			Blueprint->GetAllGraphs(AllGraphs);
			for (UEdGraph* Candidate : AllGraphs)
			{
				if (IsValid(Candidate) && Candidate->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
				{
					Graph = Candidate;
					break;
				}
			}
			if (!IsValid(Graph))
			{
				return IClaireonTool::MakeErrorResult(FString::Printf(
					TEXT("No graph named '%s' on this Blueprint."), *GraphName));
			}
		}
		if (!IsValid(Graph))
		{
			return IClaireonTool::MakeErrorResult(TEXT("No target graph. Pass graph_name."));
		}

		TSet<UEdGraphNode*> Selection;
		FString SelectionError;
		if (!Extract_ResolveSelection(Params, Graph, Selection, SelectionError))
		{
			return IClaireonTool::MakeErrorResult(SelectionError);
		}
		if (Selection.Num() == 0)
		{
			return IClaireonTool::MakeErrorResult(TEXT("The resolved selection is empty."));
		}

		// Exclude knots because passing them to engine collapse changes boundary wiring.
		TSet<UEdGraphNode*> Filtered;
		int32 KnotsDropped = 0;
		for (UEdGraphNode* Node : Selection)
		{
			if (Cast<UK2Node_Knot>(Node))
			{
				++KnotsDropped;
			}
			else
			{
				Filtered.Add(Node);
			}
		}
		Selection = MoveTemp(Filtered);
		if (Selection.Num() == 0)
		{
			return IClaireonTool::MakeErrorResult(
				TEXT("The selection contains only reroute nodes, which are presentation and "
				     "cannot be extracted. Select the nodes the reroutes connect."));
		}

		TArray<FBoundaryEdge> ExecLeaving;
		TArray<FString> CrossingData;
		Extract_FindBoundary(Selection, ExecLeaving, CrossingData);

		// Function and event extraction change the UFunction frame, so enclosing locals require preflight.
		// Macros and composites expand in the caller frame. Opt-in promotion accepts only eligible read-only locals.
		bool bPromoteEnclosingLocals = false;
		Params->TryGetBoolField(TEXT("promote_enclosing_locals"), bPromoteEnclosingLocals);
		TArray<ClaireonExtractSemantics::FPromotionCandidate> PromotionPlan;

		if (Kind == EExtractKind::Function || Kind == EExtractKind::Event)
		{
			TArray<ClaireonExtractSemantics::FEnclosingLocalFinding> LocalFindings;
			ClaireonExtractSemantics::FindEnclosingLocalViolations(Selection, LocalFindings);

			FString PromotionRefusal;
			bool bPromotionAccepted = false;
			if (LocalFindings.Num() > 0 && bPromoteEnclosingLocals)
			{
				if (Kind != EExtractKind::Function)
				{
					// Event extraction cannot accept promotion because it does not wire boundary parameters.
					PromotionRefusal = FString::Printf(
						TEXT("promote_enclosing_locals is honoured by bp_extract_function only, and ")
						TEXT("this is bp_extract_%s. Macro and composite extraction never refuse ")
						TEXT("enclosing locals at all -- their nodes are expanded back into the ")
						TEXT("calling function, so MemberScope still resolves -- and bp_extract_event ")
						TEXT("refuses every crossing data pin because it does not wire event ")
						TEXT("parameters at the call site."),
						Extract_KindName(Kind));
				}
				else
				{
					bPromotionAccepted = ClaireonExtractSemantics::BuildPromotionPlan(
						Selection, LocalFindings, PromotionPlan, PromotionRefusal);
				}
			}
			if (!bPromotionAccepted)
			{
				PromotionPlan.Reset();
			}

			if (LocalFindings.Num() > 0 && !bPromotionAccepted)
			{
				const FString ToolName = FString::Printf(TEXT("bp_extract_%s"), Extract_KindName(Kind));
				FString Refusal = ClaireonExtractSemantics::EnclosingLocalRefusal(
					*ToolName, LocalFindings);
				if (!PromotionRefusal.IsEmpty())
				{
					Refusal += TEXT(" ");
					Refusal += PromotionRefusal;
				}

				FClaireonBPMutationResult Envelope;
				Envelope.MutationState = EClaireonMutationState::Refused;
				Envelope.bMutationRetained = ClaireonBPMutation::RetainsMutation(Envelope.MutationState);
				Envelope.LastCompletedPhase = kClaireonBPPhaseNone;
				Envelope.bUndoRecordAvailable = false;
				Envelope.AssetPath = Blueprint->GetPathName();
				Envelope.SessionId = SessionId;

				FToolResult Error = IClaireonTool::MakeErrorResult(Refusal);
				Error.Data = MakeShared<FJsonObject>();
				Envelope.WriteInlineScalars(*Error.Data);
				Error.Data->SetStringField(TEXT("extract_kind"), Extract_KindName(Kind));

				Error.Data->SetStringField(TEXT("quiescence_proof"), TEXT("by_construction"));
				Error.Data->SetStringField(TEXT("target_graph"), Graph->GetName());

				TArray<TSharedPtr<FJsonValue>> FindingValues;
				for (const ClaireonExtractSemantics::FEnclosingLocalFinding& Finding : LocalFindings)
				{
					TSharedPtr<FJsonObject> FindingJson = MakeShared<FJsonObject>();
					FindingJson->SetStringField(TEXT("variable"), Finding.VariableName);
					FindingJson->SetStringField(TEXT("node_guid"), Finding.NodeGuid);
					FindingJson->SetStringField(TEXT("reason"),
						ClaireonExtractSemantics::ToWireString(Finding.Reason));
					if (Finding.ReachedReferencePins.Num() > 0)
					{
						TArray<TSharedPtr<FJsonValue>> PinValues;
						for (const FString& PinId : Finding.ReachedReferencePins)
						{
							PinValues.Add(MakeShared<FJsonValueString>(PinId));
						}
						FindingJson->SetArrayField(TEXT("reached_reference_pins"), PinValues);
					}
					FindingValues.Add(MakeShared<FJsonValueObject>(FindingJson));
				}
				Error.Data->SetArrayField(TEXT("enclosing_locals"), FindingValues);

				// Distinguish requested-but-refused promotion from an omitted request.
				if (bPromoteEnclosingLocals)
				{
					Error.Data->SetBoolField(TEXT("promotion_requested"), true);
					const FString Because = PromotionRefusal.IsEmpty()
						? FString(TEXT("promote_enclosing_locals produced no plan for this selection"))
						: PromotionRefusal;
					Error.Data->SetStringField(TEXT("promotion_refused_because"), Because);
				}
				return Error;
			}
		}

		// Decide automatic function purity before collapse moves the selection.
		FString PurityGateWhyNot;
		const bool bSelectionIsExecFree = (Kind == EExtractKind::Function)
			&& Extract_IsExecFreeGivenBoundary(Selection, ExecLeaving, PurityGateWhyNot);

		// Read the requested name before event conflict preflight.
		FString NewName;
		Params->TryGetStringField(TEXT("new_name"), NewName);

		if (Kind == EExtractKind::Event)
		{
			if (ExecLeaving.Num() > 0)
			{
				FToolResult Error = IClaireonTool::MakeErrorResult(FString::Printf(
					TEXT("bp_extract_event refuses a selection with %d exec edge(s) leaving it. ")
					TEXT("A custom event does not block its caller, so a continuation wired after ")
					TEXT("the call would run when the extracted work STARTS, not when it finishes ")
					TEXT("-- silently reordering the graph. Either include the whole completion ")
					TEXT("tail in the selection, or use bp_extract_function, whose gateway has a ")
					TEXT("completion exec pin that expresses this correctly."),
					ExecLeaving.Num()));
				Error.Data = Extract_BoundaryEvidence(ExecLeaving, CrossingData);
				return Error;
			}
			// Require exactly one distinct incoming target pin; multiple callers of that pin remain valid.
			{
				TArray<UEdGraphPin*> ProbeExternal;
				TArray<UEdGraphPin*> ProbeInternal;
				Extract_FindEntries(Selection, ProbeExternal, ProbeInternal);
				if (ProbeInternal.Num() == 0)
				{
					FToolResult Error = IClaireonTool::MakeErrorResult(
						TEXT("No exec edge arrives at this selection from outside it, so there is "
						     "no call site to replace. The nodes are already unreachable; "
						     "extracting them into an event would leave two orphans instead of one."));
					Error.Data = Extract_BoundaryEvidence(ExecLeaving, CrossingData);
					return Error;
				}

				const TSet<UEdGraphPin*> DistinctEntries(ProbeInternal);
				if (DistinctEntries.Num() > 1)
				{
					// Sort evidence independently of pointer-hash iteration.
					TArray<FString> EntryIds;
					for (UEdGraphPin* Entry : DistinctEntries)
					{
						EntryIds.Add(FString::Printf(TEXT("%s.%s"),
							*Extract_NodeId(Entry ? Entry->GetOwningNode() : nullptr),
							Entry ? *Entry->PinName.ToString() : TEXT("<null>")));
					}
					EntryIds.Sort();

					TArray<TSharedPtr<FJsonValue>> EntryValues;
					for (const FString& EntryId : EntryIds)
					{
						EntryValues.Add(MakeShared<FJsonValueString>(EntryId));
					}

					FToolResult Error = IClaireonTool::MakeErrorResult(FString::Printf(
						TEXT("bp_extract_event refuses a selection with %d distinct entry point(s). ")
						TEXT("A custom event has one 'then' pin, so only one of them could be ")
						TEXT("reconnected and the rest would be left unreachable under the event ")
						TEXT("while the call reported success. Either extend the selection so every ")
						TEXT("entry flows through a single node, or extract each region separately. ")
						TEXT("entry_pins names them."),
						DistinctEntries.Num()));
					Error.Data = Extract_BoundaryEvidence(ExecLeaving, CrossingData);
					Error.Data->SetArrayField(TEXT("entry_pins"), EntryValues);
					return Error;
				}
			}
			if (CrossingData.Num() > 0)
			{
				FToolResult Error = IClaireonTool::MakeErrorResult(FString::Printf(
					TEXT("bp_extract_event refuses a selection with %d data pin(s) crossing its ")
					TEXT("boundary. Those would have to become event parameters wired at the call ")
					TEXT("site, which this tool does not do -- it would leave an unwired pin and a ")
					TEXT("graph that does not compile. Use bp_extract_function, which promotes ")
					TEXT("boundary pins to parameters."),
					CrossingData.Num()));
				Error.Data = Extract_BoundaryEvidence(ExecLeaving, CrossingData);
				return Error;
			}

			// Validate explicit event names with the shared conflict gate; do not silently rename them.
			if (!NewName.IsEmpty())
			{
				const ClaireonBlueprintHelpers::FCustomEventNameConflict Conflict =
					ClaireonBlueprintHelpers::FindCustomEventNameConflict(Blueprint, NewName);
				if (Conflict.IsConflict())
				{
					FString Message = FString::Printf(
						TEXT("bp_extract_event cannot name the event '%s': %s %s"),
						*NewName, *Conflict.Explanation, *Conflict.Remedy);
					if (!Conflict.ResolutionNote.IsEmpty())
					{
						Message += FString::Printf(TEXT(" (%s)"), *Conflict.ResolutionNote);
					}

					FToolResult Error = IClaireonTool::MakeErrorResult(Message);
					Error.Data = Extract_BoundaryEvidence(ExecLeaving, CrossingData);
					Error.Data->SetStringField(TEXT("name_conflict_kind"),
						ClaireonBlueprintHelpers::ToString(Conflict.Kind));
					Error.Data->SetStringField(TEXT("name_conflict_owner"), Conflict.OwnerName);
					if (!Conflict.ExistingNodeGuid.IsEmpty())
					{
						Error.Data->SetStringField(TEXT("name_conflict_node_guid"),
							Conflict.ExistingNodeGuid);
					}
					if (!Conflict.ExistingGraphName.IsEmpty())
					{
						Error.Data->SetStringField(TEXT("name_conflict_graph"),
							Conflict.ExistingGraphName);
					}
					return Error;
				}
			}
		}

		// Check editor capability before opening; preflight refusals above remain headless.
		if (!FSlateApplication::IsInitialized())
		{
			return IClaireonTool::MakeErrorResult(FString::Printf(
				TEXT("bp_extract_%s requires an interactive editor. It runs the Blueprint "
				     "editor's own collapse implementation, which operates on a live "
				     "SGraphEditor widget, and no Slate application exists in this process "
				     "(a commandlet or -nullrhi run)."),
				Extract_KindName(Kind)));
		}

		// Create the graph tab synchronously and leave the editor open for the session.
		FScopedBlueprintEditor ScopedEditor(Blueprint, /*bInSilent=*/false, /*bInCloseOnDestroy=*/false);
		if (!ScopedEditor.IsValid())
		{
			return IClaireonTool::MakeErrorResult(
				TEXT("Could not open a Blueprint editor for this asset. Extraction runs the "
				     "editor's own collapse implementation and needs one."));
		}
		TSharedPtr<FBlueprintEditor> Editor = ScopedEditor.GetBlueprintEditor();
		TSharedPtr<SGraphEditor> GraphEditor = ScopedEditor.GetGraphEditor(Graph);
		if (!Editor.IsValid() || !GraphEditor.IsValid())
		{
			return IClaireonTool::MakeErrorResult(FString::Printf(
				TEXT("Could not obtain a graph editor widget for '%s'. Extraction runs the "
				     "editor's own collapse implementation, which operates on that widget's "
				     "selection state, so it cannot proceed without one."), *Graph->GetName()));
		}

		// Use the engine's collapse eligibility checks.
		if (Kind == EExtractKind::Function && !ClaireonBlueprintEditorAccess::CanCollapseToFunction(*Editor, Selection))
		{
			FToolResult Error = IClaireonTool::MakeErrorResult(
				TEXT("The editor refuses this selection for function extraction -- the same "
				     "condition that greys out Collapse to Function in the right-click menu. "
				     "Usual causes: the selection spans an event node, a tunnel, or the graph's "
				     "entry. Try bp_extract_composite, which accepts more."));
			Error.Data = Extract_BoundaryEvidence(ExecLeaving, CrossingData);
			return Error;
		}
		if (Kind == EExtractKind::Macro && !ClaireonBlueprintEditorAccess::CanCollapseToMacro(*Editor, Selection))
		{
			FToolResult Error = IClaireonTool::MakeErrorResult(
				TEXT("The editor refuses this selection for macro extraction -- the same condition "
				     "that greys out Collapse to Macro in the right-click menu."));
			Error.Data = Extract_BoundaryEvidence(ExecLeaving, CrossingData);
			return Error;
		}

		// Require composite focus to match the resolved graph before opening the transaction.
		if (Kind == EExtractKind::Composite)
		{
			const FString FocusRefusal = ClaireonExtractQuiescence::FocusedGraphMismatchRefusal(
				Graph, Editor->GetFocusedGraph());
			if (!FocusRefusal.IsEmpty())
			{
				FClaireonBPMutationResult Envelope;
				Envelope.MutationState = EClaireonMutationState::Refused;
				Envelope.bMutationRetained = ClaireonBPMutation::RetainsMutation(Envelope.MutationState);
				Envelope.LastCompletedPhase = kClaireonBPPhaseNone;
				Envelope.bUndoRecordAvailable = false;
				Envelope.AssetPath = Blueprint->GetPathName();
				Envelope.SessionId = SessionId;

				FToolResult Error = IClaireonTool::MakeErrorResult(FocusRefusal);
				Error.Data = MakeShared<FJsonObject>();
				Envelope.WriteInlineScalars(*Error.Data);
				Error.Data->SetStringField(TEXT("extract_kind"), Extract_KindName(Kind));

				Error.Data->SetStringField(TEXT("quiescence_proof"), TEXT("by_construction"));
				Error.Data->SetStringField(TEXT("resolved_graph"), Graph->GetName());
				Error.Data->SetStringField(TEXT("focused_graph"),
					IsValid(Editor->GetFocusedGraph()) ? Editor->GetFocusedGraph()->GetName() : FString());
				return Error;
			}
		}

		const int32 SelectionCount = Selection.Num();
		UEdGraph* Created = nullptr;
		UEdGraphNode* Gateway = nullptr;
		FString CreatedName;

		// Retain movement deltas for reporting; read final event and gateway positions from live nodes.
		UEdGraphNode* EventPlacedNode = nullptr;
		int32 EventBodyNodesMoved = 0;
		int32 EventBodyKnotsMoved = 0;
		int32 EventBodyDeltaX = 0;
		int32 EventBodyDeltaY = 0;

		bool bRenamed = false;

		bool bMadePure = false;
		bool bPurityRepairFailed = false;
		FString PurityRepairError;

		bool bParametersSynthesized = false;
		bool bPromotionFailed = false;
		FString PromotionFailureError;
		TArray<TSharedPtr<FJsonObject>> PromotedRecords;
		FString PromotionSignatureBefore;
		FString PromotionSignatureAfter;

		// Identify the gateway by node-set difference because composite collapse returns no graph.
		TSet<UEdGraphNode*> NodesBefore;
		for (UEdGraphNode* Existing : Graph->Nodes)
		{
			NodesBefore.Add(Existing);
		}

		// Capture all effect classes after preflight and before mutation, including skeleton and transaction state.
		FClaireonBPSnapshot StartSnapshot;
		ClaireonBPSnapshot::Capture(Blueprint, Extract_SnapshotGraphs(Blueprint),
			EClaireonBPSnapshotFamily::Extraction, StartSnapshot);

		// The journal records intent; snapshots determine result state.
		FClaireonBPPhaseJournal Journal;
		const FString CollapsePhase = ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::Collapse);

		{
			FScopedTransaction Transaction(Extract_TransactionTitle(Kind));
			Blueprint->Modify();
			Graph->Modify();

			Journal.Append({CollapsePhase, TEXT("transaction_opened"), Graph->GetName(),
				TEXT("Blueprint and Graph were both marked modified unconditionally, so the "
				     "package is dirty from here on, on every path out")});

			switch (Kind)
			{
			case EExtractKind::Function:
				Journal.Append({CollapsePhase, TEXT("collapse_to_function_entered"), Graph->GetName(),
					TEXT("CollapseSelectionToFunction relocates the selection out of this graph "
					     "before it returns, so a null result does NOT mean nothing moved")});
				Created = ClaireonBlueprintEditorAccess::CollapseToFunction(
					*Editor, GraphEditor, Selection, Gateway);
				break;
			case EExtractKind::Macro:
				Journal.Append({CollapsePhase, TEXT("collapse_to_macro_entered"), Graph->GetName(),
					TEXT("CollapseSelectionToMacro relocates the selection out of this graph "
					     "before it returns, so a null result does NOT mean nothing moved")});
				Created = ClaireonBlueprintEditorAccess::CollapseToMacro(
					*Editor, GraphEditor, Selection, Gateway);
				break;
			case EExtractKind::Composite:
				// Find the composite gateway from the graph after the void engine call.
				Journal.Append({CollapsePhase, TEXT("collapse_nodes_entered"), Graph->GetName(),
					TEXT("focused-graph identity was asserted equal to this graph before entry")});
				ClaireonBlueprintEditorAccess::CollapseToComposite(*Editor, Selection);
				break;
			case EExtractKind::Event:
			{
				// Replace the incoming boundary with a call and connect the custom event to the single entry.
				TArray<UEdGraphPin*> ExternalOutputs;
				TArray<UEdGraphPin*> InternalInputs;
				Extract_FindEntries(Selection, ExternalOutputs, InternalInputs);

				// Recheck entry arity after editor opening; Modify already ran, so failure must report retained effects.
				const TSet<UEdGraphPin*> DistinctInternal(InternalInputs);
				if (InternalInputs.Num() == 0 || DistinctInternal.Num() > 1)
				{
					Journal.Append({CollapsePhase, TEXT("entry_invariant_broken"), Graph->GetName(),
						FString::Printf(
							TEXT("%d distinct entry pin(s) at mutation time; preflight proved one"),
							DistinctInternal.Num())});
					Transaction.Cancel();

					FExtractOutcome Outcome;
					Outcome.State = EClaireonMutationState::AppliedOperationFailed;
					Outcome.FailedPhase = CollapsePhase;
					Outcome.LastCompletedPhase = kClaireonBPPhaseNone;
					Outcome.bUndoRecordAvailable = false;
					Outcome.bDirtyAndUncompiled = true;
					Outcome.TargetGraph = Graph->GetName();
					Outcome.bSessionGraphStale = false;
					return Extract_RetainedFailure(Blueprint, SessionId, Kind, Outcome,
						StartSnapshot, Journal,
						FString::Printf(
							TEXT("The selection's entry topology changed between preflight and "
							     "mutation: %d distinct exec edge target(s) now arrive from "
							     "outside the selection, where exactly one is required. No node "
							     "was created and no wire was moved; the package is dirty "
							     "because the transaction had already marked it."),
							DistinctInternal.Num()));
				}

				// Allocate the event's slot before adding nodes, place the gateway at the old selection,
				// and translate the body rigidly below the event.
				const ClaireonGraphIslands::FStackSlot Slot =
					ClaireonGraphIslands::AllocateSlot(Graph, kExtract_IslandGutter);

				// Capture the selection origin before moving any member.
				bool bHaveSelectionOrigin = false;
				int32 SelectionMinX = 0;
				int32 SelectionMinY = 0;
				for (UEdGraphNode* Selected : Selection)
				{
					if (!IsValid(Selected))
					{
						continue;
					}
					SelectionMinX = bHaveSelectionOrigin
						? FMath::Min(SelectionMinX, Selected->NodePosX)
						: Selected->NodePosX;
					SelectionMinY = bHaveSelectionOrigin
						? FMath::Min(SelectionMinY, Selected->NodePosY)
						: Selected->NodePosY;
					bHaveSelectionOrigin = true;
				}

				UK2Node_CustomEvent* EventNode = NewObject<UK2Node_CustomEvent>(Graph);
				Graph->AddNode(EventNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
				EventNode->CreateNewGuid();
				EventNode->CustomFunctionName = NewName.IsEmpty()
					? FBlueprintEditorUtils::FindUniqueKismetName(Blueprint, TEXT("ExtractedEvent"))
					: FName(*NewName);
				EventNode->NodePosX = FMath::RoundToInt32(Slot.X);
				EventNode->NodePosY = FMath::RoundToInt32(Slot.Y);
				EventNode->AllocateDefaultPins();
				Journal.Append({CollapsePhase, TEXT("custom_event_node_added"),
					EventNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
					EventNode->CustomFunctionName.ToString()});
				Journal.Append({CollapsePhase, TEXT("event_island_slot_allocated"),
					EventNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
					FString::Printf(
						TEXT("root of a NEW island at (%d,%d): rail x=%.0f, y = max NodePosY over "
						     "every node + gutter %.0f, allocated before either new node existed"),
						EventNode->NodePosX, EventNode->NodePosY, Slot.X, kExtract_IslandGutter)});

				// Regenerate the skeleton before allocating call pins so the new event UFunction exists.
				FKismetEditorUtilities::GenerateBlueprintSkeleton(Blueprint, /*bForceRegeneration=*/true);
				Journal.Append({CollapsePhase, TEXT("skeleton_force_regenerated"),
					Blueprint->GetPathName(),
					TEXT("GenerateBlueprintSkeleton cannot witness its own effect -- it returns "
					     "false unconditionally -- so the only proof is the SkeletonGeneratedClass "
					     "pointer identity the snapshot records")});

				UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(Graph);
				Graph->AddNode(CallNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
				CallNode->CreateNewGuid();
				CallNode->FunctionReference.SetSelfMember(EventNode->CustomFunctionName);
				CallNode->NodePosX = SelectionMinX;
				CallNode->NodePosY = SelectionMinY;
				CallNode->AllocateDefaultPins();
				Journal.Append({CollapsePhase, TEXT("call_node_added"),
					CallNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
					EventNode->CustomFunctionName.ToString()});
				Journal.Append({CollapsePhase, TEXT("gateway_placed_at_selection_origin"),
					CallNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
					FString::Printf(
						TEXT("(%d,%d), the top-left of the %d selected node(s) it stands in for; it "
						     "stays in the ORIGINAL island and takes no stack slot"),
						CallNode->NodePosX, CallNode->NodePosY, Selection.Num())});

				UEdGraphPin* EventThen = EventNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
				UEdGraphPin* CallExec = CallNode->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);

				// Inject a missing pin after real node creation and skeleton regeneration to exercise cancellation reporting.
				if (CLAIREON_BP_SHOULD_INJECT_FAILURE(*CollapsePhase))
				{
					EventThen = nullptr;
				}

				// Check both pins before rewiring; new nodes and skeleton changes already exist.
				if (!EventThen || !CallExec)
				{
					// Cancel closes the transaction and removes its undo record without restoring the mutation.
					Journal.Append({CollapsePhase, TEXT("transaction_canceled"), Graph->GetName(),
						TEXT("Cancel() popped the undo record without reverting; the added nodes "
						     "and the regenerated skeleton remain with no undo path")});
					Transaction.Cancel();

					FExtractOutcome Outcome;
					Outcome.State = EClaireonMutationState::AppliedOperationFailed;
					Outcome.FailedPhase = CollapsePhase;
					Outcome.LastCompletedPhase = kClaireonBPPhaseNone;
					Outcome.bUndoRecordAvailable = false;

					Outcome.bDirtyAndUncompiled = true;
					Outcome.TargetGraph = Graph->GetName();
					Outcome.bSessionGraphStale = true;
					return Extract_RetainedFailure(Blueprint, SessionId, Kind, Outcome,
						StartSnapshot, Journal,
						FString::Printf(
							TEXT("Could not build the call site for event '%s': the %s pin is missing "
							     "after skeleton regeneration. The operation is RETAINED, not "
							     "reverted: the custom event node, the call node and the "
							     "force-regenerated skeleton class all remain. Cancelling the "
							     "transaction discarded its undo record without reverting anything, "
							     "so undo_record_available is false and undo must not be used -- it "
							     "would revert an unrelated earlier transaction. operation_delta "
							     "lists exactly what remains."),
							*EventNode->CustomFunctionName.ToString(),
							EventThen ? TEXT("call node's exec input") : TEXT("event's then")));
				}

				for (UEdGraphPin* External : ExternalOutputs)
				{
					if (External)
					{
						External->BreakAllPinLinks();
						External->MakeLinkTo(CallExec);
					}
				}
				// Preserve internal backedges on the entry pin; external boundary links were already removed.
				// All InternalInputs refer to the same pin.
				EventThen->MakeLinkTo(InternalInputs[0]);

				// Move the body only after pin validation, applying one delta to every member and internal reroute.
				const int32 BodyDeltaX = EventNode->NodePosX - SelectionMinX;
				const int32 BodyDeltaY =
					(EventNode->NodePosY + FMath::RoundToInt32(kExtract_BodyDropY)) - SelectionMinY;
				int32 BodyNodesMoved = 0;
				for (UEdGraphNode* Selected : Selection)
				{
					if (!IsValid(Selected))
					{
						continue;
					}
					// Record positions with Modify so they belong to the extraction transaction.
					Selected->Modify();
					Selected->NodePosX += BodyDeltaX;
					Selected->NodePosY += BodyDeltaY;
					++BodyNodesMoved;
				}
				Journal.Append({CollapsePhase, TEXT("body_translated_rigidly"), Graph->GetName(),
					FString::Printf(
						TEXT("%d node(s) moved by one shared delta (%+d,%+d): selection top-left "
						     "(%d,%d) -> (%d,%d), %.0f under the event at (%d,%d)"),
						BodyNodesMoved, BodyDeltaX, BodyDeltaY, SelectionMinX, SelectionMinY,
						SelectionMinX + BodyDeltaX, SelectionMinY + BodyDeltaY,
						kExtract_BodyDropY, EventNode->NodePosX, EventNode->NodePosY)});

				// Carry internal reroutes by the same delta as the body.
				TArray<UK2Node_Knot*> CarriedKnots;
				int32 CarryRounds = 0;
				Extract_CollectCarriedKnots(Graph, Selection, CarriedKnots, CarryRounds);
				for (UK2Node_Knot* Knot : CarriedKnots)
				{
					Knot->Modify();
					Knot->NodePosX += BodyDeltaX;
					Knot->NodePosY += BodyDeltaY;
				}
				Journal.Append({CollapsePhase, TEXT("body_knots_carried"), Graph->GetName(),
					FString::Printf(
						TEXT("%d reroute(s) carried by the same delta (%+d,%+d) after %d fixpoint "
						     "round(s); a knot with any link outside the moved set stayed put, "
						     "because it spans the new boundary"),
						CarriedKnots.Num(), BodyDeltaX, BodyDeltaY, CarryRounds)});

				EventPlacedNode = EventNode;

				// Count moved knots both in the total and separately from the selected nodes.
				EventBodyNodesMoved = BodyNodesMoved + CarriedKnots.Num();
				EventBodyKnotsMoved = CarriedKnots.Num();
				EventBodyDeltaX = BodyDeltaX;
				EventBodyDeltaY = BodyDeltaY;

				Gateway = CallNode;
				CreatedName = EventNode->CustomFunctionName.ToString();
				break;
			}
			}

			if (!IsValid(Gateway))
			{
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (IsValid(Node) && !NodesBefore.Contains(Node))
					{
						Gateway = Node;
						break;
					}
				}
			}

			// Read the composite graph from the new gateway's BoundGraph.
			if (Kind == EExtractKind::Composite)
			{
				if (UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Gateway); IsValid(Composite))
				{
					Created = Composite->BoundGraph;
				}
			}

			if (IsValid(Created))
			{
				CreatedName = Created->GetName();
				if (!NewName.IsEmpty() && !CreatedName.Equals(NewName))
				{
					FBlueprintEditorUtils::RenameGraph(Created, NewName);
					CreatedName = Created->GetName();
					bRenamed = true;
					Journal.Append({ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::Rename),
						TEXT("graph_renamed"), CreatedName, NewName});
				}
			}

			// Apply purity after rename because call-site refresh matches function names.
			if (bSelectionIsExecFree && IsValid(Created))
			{
				FString PurityError;
				if (Extract_MakeFunctionPure(Blueprint, Created, Journal, PurityError))
				{
					bMadePure = true;
				}
				else
				{
					// Record recipe failure; structural validation will inspect the resulting gateway pins.
					bPurityRepairFailed = true;
					PurityRepairError = PurityError;
					Journal.Append({ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::PurityUpdate),
						TEXT("purity_update_failed"), Graph->GetName(), PurityError});
				}
			}
			else if (Kind == EExtractKind::Function && !PurityGateWhyNot.IsEmpty())
			{
				Journal.Append({ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::PurityUpdate),
					TEXT("purity_update_not_applicable"), Graph->GetName(), PurityGateWhyNot});
			}

			// Synthesize parameters after rename and purity repair, then refresh the new gateway inputs.
			if (PromotionPlan.Num() > 0 && IsValid(Created) && IsValid(Gateway))
			{
				FString PromotionError;
				if (Extract_PromoteEnclosingLocals(Blueprint, Graph, Created, Gateway, PromotionPlan,
					Journal, PromotedRecords, PromotionSignatureBefore, PromotionSignatureAfter,
					PromotionError))
				{
					bParametersSynthesized = true;
				}
				else
				{
					// Report synthesis failure after the transaction scope commits.
					bPromotionFailed = true;
					PromotionFailureError = PromotionError;
					Journal.Append({ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::ParameterSynthesis),
						TEXT("parameter_synthesis_failed"), Graph->GetName(), PromotionError});
				}
			}

			Journal.Append({CollapsePhase, TEXT("transaction_committing"), Graph->GetName(),
				TEXT("the scope ends here, so everything above is committed as one record and "
				     "an undo record for it exists from this point on")});
		}

		// Report only phases that actually completed.
		const FString LastCompletedBeforePromotion = bMadePure
			? FString(ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::CallsiteRefresh))
			: (bRenamed
				? FString(ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::Rename))
				: CollapsePhase);

		const FString LastCompletedBeforeCompile = bParametersSynthesized
			? FString(ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::Wiring))
			: LastCompletedBeforePromotion;

		// Inject a missing result after the real transaction commits to exercise retained-failure reporting.
		if (Kind != EExtractKind::Event && CLAIREON_BP_SHOULD_INJECT_FAILURE(*CollapsePhase))
		{
			Created = nullptr;
		}

		if (Kind != EExtractKind::Event && !IsValid(Created))
		{
			// The committed mutation remains dirty and uncompiled, with an undo record; the session cursor update was skipped.
			Journal.Append({CollapsePhase, TEXT("collapse_produced_no_graph"), Graph->GetName(),
				TEXT("the transaction had already committed when this was discovered")});

			FExtractOutcome Outcome;
			Outcome.State = EClaireonMutationState::AppliedOperationFailed;
			Outcome.FailedPhase = CollapsePhase;
			Outcome.LastCompletedPhase = kClaireonBPPhaseNone;
			Outcome.bUndoRecordAvailable = true;
			Outcome.bDirtyAndUncompiled = true;
			Outcome.TargetGraph = Graph->GetName();
			Outcome.bSessionGraphStale = true;
			return Extract_RetainedFailure(Blueprint, SessionId, Kind, Outcome,
				StartSnapshot, Journal,
				FString::Printf(
					TEXT("The editor's collapse produced no %s graph. The selection was accepted by "
					     "its own eligibility check, so this is an engine-side failure rather than a "
					     "rejected selection -- but the transaction had ALREADY COMMITTED when that "
					     "was discovered, so the operation is RETAINED. The package is dirty and the "
					     "asset was NOT recompiled, and on the function and macro paths the selected "
					     "nodes have already been moved out of the source graph. An undo record for "
					     "the whole operation does exist. operation_delta lists what remains."),
					Extract_KindName(Kind)));
		}

		// Report synthesis failure at its own phase, retaining completed work and skipping compile.
		if (bPromotionFailed)
		{
			FExtractOutcome Outcome;
			Outcome.State = EClaireonMutationState::AppliedOperationFailed;
			Outcome.FailedPhase =
				ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::ParameterSynthesis);
			Outcome.LastCompletedPhase = LastCompletedBeforePromotion;
			Outcome.bUndoRecordAvailable = true;
			Outcome.bDirtyAndUncompiled = true;
			Outcome.TargetGraph = Graph->GetName();
			Outcome.bSessionGraphStale = true;
			return Extract_RetainedFailure(Blueprint, SessionId, Kind, Outcome,
				StartSnapshot, Journal,
				FString::Printf(
					TEXT("The function extraction was applied and RETAINED, and promoting the ")
					TEXT("read-only enclosing locals then failed: %s. The extracted graph, its ")
					TEXT("gateway and the relocated nodes are all still there, and part of the ")
					TEXT("signature may already have been synthesized. An undo record for the whole ")
					TEXT("operation exists. Inspect operation_delta, then either repair forward or ")
					TEXT("undo once."),
					*PromotionFailureError));
		}

		// Capture compiler messages alongside Blueprint status.
		FCompilerResultsLog CompilerLog;
		CompilerLog.SetSourcePath(Blueprint->GetPathName());
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompilerLog);

		// Copy graph-token identities while their weak references still resolve.
		TArray<FExtractDiagnostic> Diagnostics;
		int32 TokenlessDiagnostics = 0;
		Extract_CollectDiagnostics(CompilerLog, Diagnostics, TokenlessDiagnostics);

		// Report BS_Error as compiler failure; warnings remain a separate semantic-validation concern.
		const bool bCompileFailed = (Blueprint->Status == BS_Error);
		Journal.Append({ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::CompileValidate),
			bCompileFailed ? TEXT("compile_failed") : TEXT("compile_succeeded"),
			Blueprint->GetPathName(),
			FString::Printf(TEXT("EBlueprintStatus=%d"), static_cast<int32>(Blueprint->Status))});

		if (bCompileFailed)
		{
			FExtractOutcome Outcome;

			Outcome.State = EClaireonMutationState::AppliedValidationFailed;
			Outcome.FailedPhase = ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::CompileValidate);
			Outcome.LastCompletedPhase = LastCompletedBeforeCompile;
			Outcome.CompileStatus = EClaireonEngineCompileStatus::Failed;
			Outcome.bUndoRecordAvailable = true;
			Outcome.TargetGraph = Graph->GetName();
			Outcome.bSessionGraphStale = true;
			FToolResult Failure = Extract_RetainedFailure(Blueprint, SessionId, Kind, Outcome,
				StartSnapshot, Journal,
				FString::Printf(
					TEXT("The %s extraction was applied and RETAINED, and the Blueprint then failed "
					     "to compile. The new graph, its gateway and the relocated nodes are all "
					     "still there; an undo record for the whole operation exists. Inspect "
					     "operation_delta, then either repair forward or undo once."),
					Extract_KindName(Kind)));
			Extract_AttachDiagnostics(Failure, Diagnostics, TokenlessDiagnostics);
			return Failure;
		}

		// A successful compile still requires structural validation for lost execution and gateway purity.
		FClaireonBPSnapshot AfterSnapshot;
		ClaireonBPSnapshot::Capture(Blueprint, Extract_SnapshotGraphs(Blueprint),
			EClaireonBPSnapshotFamily::Extraction, AfterSnapshot);
		const FClaireonBPSnapshotDelta ValidationDelta = ClaireonBPSnapshot::Diff(StartSnapshot, AfterSnapshot);

		TArray<FString> ValidationFailures;

		// Inspect the new gateway directly because it has no pre-operation counterpart.
		if (bMadePure)
		{
			FString OffendingPins;
			if (!Extract_GatewayHasNoExecPins(Gateway, OffendingPins))
			{
				ValidationFailures.Add(FString::Printf(
					TEXT("the extracted function was made pure but its gateway call node %s still "
					     "carries exec pin(s): %s. A pure island has no exec edge to attach, so the "
					     "compiler prunes the call and every consumer reads a type default instead "
					     "of the computed value."),
					*Extract_NodeId(Gateway), *OffendingPins));
			}
		}
		if (bPurityRepairFailed)
		{
			ValidationFailures.Add(FString::Printf(
				TEXT("the selection was exec-free, so the extracted function must be pure, but the "
				     "purity repair could not be applied (%s)."),
				*PurityRepairError));
		}

		Extract_FindLostExecInputs(StartSnapshot, AfterSnapshot, ValidationFailures);

		// Warnings or higher block clean status when they name a delta node or have no graph token.
		// Use captured GUIDs, not titles or formatted text; lower severity numbers indicate higher severity.
		const TSet<FGuid> DeltaNodeGuids = Extract_DeltaNodeGuids(ValidationDelta);
		for (const FExtractDiagnostic& Diagnostic : Diagnostics)
		{
			if (Diagnostic.Severity > static_cast<int32>(EMessageSeverity::Warning))
			{
				continue;
			}
			const bool bNamesDeltaNode = Diagnostic.bHasNode && DeltaNodeGuids.Contains(Diagnostic.NodeGuid);
			if (!bNamesDeltaNode && Diagnostic.bHasNode)
			{
				continue;
			}
			ValidationFailures.Add(FString::Printf(
				TEXT("compiler %s%s in the operation delta window: %s"),
				*Diagnostic.SeverityLabel,
				Diagnostic.Identifier.IsEmpty()
					? TEXT("")
					: *FString::Printf(TEXT(" [%s]"), *Diagnostic.Identifier),
				*Diagnostic.Message));
		}

		if (ValidationFailures.Num() > 0)
		{
			FExtractOutcome Outcome;

			// Preserve the successful compiler verdict when structural validation fails.
			Outcome.State = EClaireonMutationState::AppliedValidationFailed;
			Outcome.FailedPhase = ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::CompileValidate);
			Outcome.LastCompletedPhase = LastCompletedBeforeCompile;
			Outcome.CompileStatus = EClaireonEngineCompileStatus::Succeeded;
			Outcome.bUndoRecordAvailable = true;
			Outcome.TargetGraph = Graph->GetName();
			Outcome.bSessionGraphStale = true;

			Journal.Append({ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::CompileValidate),
				TEXT("structural_validation_failed"), Graph->GetName(),
				FString::Join(ValidationFailures, TEXT(" | "))});

			FToolResult Failure = Extract_RetainedFailure(Blueprint, SessionId, Kind, Outcome,
				StartSnapshot, Journal,
				FString::Printf(
					TEXT("The %s extraction was applied and RETAINED, the Blueprint COMPILED "
					     "SUCCESSFULLY, and structural validation then proved the result wrong: %s "
					     "The mutation was NOT reverted -- automatic rollback is never performed. An "
					     "undo record for the whole operation exists. Inspect operation_delta and "
					     "validation_failures, then either repair forward or undo once."),
					Extract_KindName(Kind),
					*FString::Join(ValidationFailures, TEXT(" "))),
				&AfterSnapshot);

			TArray<TSharedPtr<FJsonValue>> FailureValues;
			for (const FString& Reason : ValidationFailures)
			{
				FailureValues.Add(MakeShared<FJsonValueString>(Reason));
			}
			if (Failure.Data.IsValid())
			{
				Failure.Data->SetArrayField(TEXT("validation_failures"), FailureValues);
			}
			Extract_AttachDiagnostics(Failure, Diagnostics, TokenlessDiagnostics);
			return Failure;
		}

		if (Data)
		{
			Data->Graph = Graph;
		}
		FToolResult Result = Tool.PublicBuildStateResponse(SessionId, Data);

		// State rendering can fail after a successful mutation and compile; still report retained effects and undo.
		if (Result.bIsError)
		{
			FExtractOutcome Outcome;
			Outcome.State = EClaireonMutationState::AppliedOperationFailed;
			Outcome.LastCompletedPhase = ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::CompileValidate);
			Outcome.CompileStatus = EClaireonEngineCompileStatus::Succeeded;
			Outcome.bUndoRecordAvailable = true;
			Outcome.TargetGraph = Graph->GetName();

			Outcome.bSessionGraphStale = false;
			return Extract_RetainedFailure(Blueprint, SessionId, Kind, Outcome,
				StartSnapshot, Journal,
				FString::Printf(
					TEXT("The %s extraction was applied and compiled successfully, but the session "
					     "state response could not be built (%s), so its payload is unavailable. "
					     "The mutation is RETAINED and an undo record for it exists."),
					Extract_KindName(Kind),
					*Result.ErrorMessage));
		}

		if (!Result.Data.IsValid())
		{
			Result.Data = MakeShared<FJsonObject>();
		}

		{
			FClaireonBPMutationResult Envelope;
			Envelope.MutationState = EClaireonMutationState::AppliedClean;
			Envelope.bMutationRetained = ClaireonBPMutation::RetainsMutation(Envelope.MutationState);
			Envelope.LastCompletedPhase = ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::CompileValidate);
			Envelope.EngineCompileStatus = EClaireonEngineCompileStatus::Succeeded;
			Envelope.bRollbackAvailable = ClaireonTransactionGroupState::bGroupActive;
			Envelope.bRollbackGroupSafe = false;
			Envelope.bUndoRecordAvailable = true;
			Envelope.AssetPath = Blueprint->GetPathName();
			Envelope.SessionId = SessionId;
			Envelope.WriteInlineScalars(*Result.Data);
		}

		Result.Data->SetStringField(TEXT("extract_kind"), Extract_KindName(Kind));
		Result.Data->SetStringField(TEXT("extracted_graph"), CreatedName);
		Result.Data->SetStringField(TEXT("gateway_node"), Extract_NodeId(Gateway));
		Result.Data->SetNumberField(TEXT("nodes_extracted"), SelectionCount);
		Result.Data->SetNumberField(TEXT("reroutes_ignored"), KnotsDropped);
		Result.Data->SetNumberField(TEXT("compile_status"),
			static_cast<int32>(Blueprint->Status));

		// Report event/body/gateway placement, reading final coordinates after compilation.
		if (Kind == EExtractKind::Event)
		{
			if (IsValid(EventPlacedNode))
			{
				Result.Data->SetStringField(TEXT("event_node"), Extract_NodeId(EventPlacedNode));
				Result.Data->SetNumberField(TEXT("event_node_pos_x"), EventPlacedNode->NodePosX);
				Result.Data->SetNumberField(TEXT("event_node_pos_y"), EventPlacedNode->NodePosY);
			}
			if (IsValid(Gateway))
			{
				Result.Data->SetNumberField(TEXT("gateway_pos_x"), Gateway->NodePosX);
				Result.Data->SetNumberField(TEXT("gateway_pos_y"), Gateway->NodePosY);
			}
			Result.Data->SetNumberField(TEXT("body_nodes_moved"), EventBodyNodesMoved);
			Result.Data->SetNumberField(TEXT("body_knots_moved"), EventBodyKnotsMoved);
			Result.Data->SetNumberField(TEXT("body_translation_dx"), EventBodyDeltaX);
			Result.Data->SetNumberField(TEXT("body_translation_dy"), EventBodyDeltaY);
		}

		// Report whether automatic purity applied.
		if (Kind == EExtractKind::Function)
		{
			Result.Data->SetBoolField(TEXT("extracted_function_is_pure"), bMadePure);
			if (!bMadePure && !PurityGateWhyNot.IsEmpty())
			{
				Result.Data->SetStringField(TEXT("purity_not_applied_because"), PurityGateWhyNot);
			}
		}

		// When promotion was requested, report its synthesized signatures and call-site wiring.
		if (Kind == EExtractKind::Function && bPromoteEnclosingLocals)
		{
			Result.Data->SetBoolField(TEXT("enclosing_locals_promoted"), bParametersSynthesized);
			if (bParametersSynthesized)
			{
				TArray<TSharedPtr<FJsonValue>> PromotedValues;
				for (const TSharedPtr<FJsonObject>& Record : PromotedRecords)
				{
					PromotedValues.Add(MakeShared<FJsonValueObject>(Record));
				}
				Result.Data->SetArrayField(TEXT("promoted_locals"), PromotedValues);
				Result.Data->SetStringField(TEXT("signature_before"), PromotionSignatureBefore);
				Result.Data->SetStringField(TEXT("signature_after"), PromotionSignatureAfter);
			}
		}
		Extract_AttachDiagnostics(Result, Diagnostics, TokenlessDiagnostics);
		return Result;
	}
}

// ---------------------------------------------------------------- function

FString ClaireonBlueprintGraphTool_ExtractFunction::GetOperation() const { return TEXT("extract_function"); }

FString ClaireonBlueprintGraphTool_ExtractFunction::GetDescription() const
{
	return TEXT("Move selected nodes into a new function graph, leaving a call node behind. "
		"Runs the editor's own 'Collapse to Function', so boundary data pins become parameters. "
		"Select via node_guids, or anchor_node_guid plus traversal_depth. Refuses what the menu "
		"greys out, and refuses an enclosing-graph local unless promote_enclosing_locals=true. "
		"Needs an open session and a live editor. Transactional.");
}

FString ClaireonBlueprintGraphTool_ExtractFunction::GetFullDescription() const
{
	return TEXT(
		"Runs FBlueprintEditor's OWN 'Collapse to Function', through ClaireonBlueprintEditorAccess, "
		"so a selection behaves here exactly as it behaves in the right-click menu: same "
		"eligibility rules, same gateway creation, same promotion of data pins crossing the "
		"boundary. Reimplementing that on CreateNewGraph/AddFunctionGraph was the alternative, and "
		"it would drift from the menu every engine version -- so the two would disagree about the "
		"same selection."
		"\n\n"
		"SELECTION. Either node_guids, or anchor_node_guid plus traversal_depth. Needs an open "
		"session and a live interactive editor. Transactional."
		"\n\n"
		"WHAT IT REFUSES, AND WHY THE REFUSALS CARRY EVIDENCE. Anything the menu greys out. Plus "
		"any reference to a LOCAL or PARAMETER of the enclosing graph, unless "
		"promote_enclosing_locals=true -- an extracted function cannot address the caller's "
		"locals, and the measured failure was four plain read-only Gets producing 'Unable to find "
		"local variable'. With promotion on, genuinely read-only locals become input parameters; a "
		"local written by a Set node, or read into a mutable by-reference pin, still refuses, "
		"because promoting either would silently discard the write."
		"\n\n"
		"PURITY IS AUTOMATIC. An exec-free data selection extracts as a PURE function -- no exec "
		"pins on the gateway -- rather than as an impure one with an unused exec chain."
		"\n\n"
		"BEFORE YOU RESTRUCTURE, read the judgement rules: instructions_read(\"blueprint-authoring\") "
		"or claireon://instructions/blueprint-authoring. bp_lint measures counts and spans; that doc "
		"decides what they mean -- which islands are worth extracting, why a function entry with "
		"wires leaving its parameter pins is the fix that takes the reroute knots with it, and why a "
		"reroute or wire count that RISES right after bp_format is debt made visible rather than a "
		"regression to revert.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_ExtractFunction::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Extract_AddCommonSchema(Builder);

	// Only function extraction supports local promotion.
	Builder.AddBoolean(TEXT("promote_enclosing_locals"),
		TEXT("Promote genuinely read-only locals of the enclosing graph to input parameters "
		     "instead of refusing. A local written by a Set node, or read into a mutable "
		     "by-reference pin, still refuses."));
	return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_ExtractFunction::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	TSharedPtr<FJsonObject> Params;
	FString SessionId;
	FBlueprintEditToolData* Data = nullptr;
	FToolResult Error;
	if (!BeginSessionOp(Arguments, TEXT("extract_function"), Params, SessionId, Data, Error))
	{
		return Error;
	}
	return ClaireonExtractApplyInternal::ApplyExtract(
		*this, EExtractKind::Function, SessionId, Data, Params);
}

// ---------------------------------------------------------------- macro

FString ClaireonBlueprintGraphTool_ExtractMacro::GetOperation() const { return TEXT("extract_macro"); }

FString ClaireonBlueprintGraphTool_ExtractMacro::GetDescription() const
{
	return TEXT("Move selected nodes into a new macro graph -- the right-click 'Collapse to "
		"Macro', running the editor's own implementation. A macro inlines at compile time rather "
		"than being called, so it may hold latent nodes and several exec paths, but it adds no "
		"callable member to the class. Needs an open session and an interactive editor. "
		"Transactional; compiles before reporting.");
}

FString ClaireonBlueprintGraphTool_ExtractMacro::GetFullDescription() const
{
	return TEXT(
		"Runs FBlueprintEditor's OWN 'Collapse to Macro', through ClaireonBlueprintEditorAccess, so "
		"the selection behaves exactly as it does in the right-click menu. Needs an open session and "
		"a live interactive editor. Transactional."
		"\n\n"
		"A MACRO IS NOT A FUNCTION, and the difference decides when to reach for this. A macro "
		"INLINES at compile time rather than being called, so it may hold latent nodes and several "
		"exec paths -- both of which a function forbids -- but it adds no callable member to the "
		"class, cannot be invoked from elsewhere in the project, and cannot be overridden or "
		"replicated. It also duplicates its body at every call site, so a macro used at several "
		"sites costs bytecode where a function would not."
		"\n\n"
		"So: prefer bp_extract_function. Reach for a macro when the selection genuinely needs a "
		"latent node or multiple exec outputs, and reach for bp_extract_composite when you only "
		"want the nodes out of the way."
		"\n\n"
		"UNLIKE bp_extract_function, this never refuses an enclosing-graph local: an inlined macro "
		"expands in the caller's scope, so the local is still in scope. That is a real capability "
		"difference and not an oversight -- it is also why a macro is the wrong home for logic you "
		"wanted isolated from the caller."
		"\n\n"
		"BEFORE YOU RESTRUCTURE, read the judgement rules: instructions_read(\"blueprint-authoring\") "
		"or claireon://instructions/blueprint-authoring. bp_lint measures counts and spans; that doc "
		"decides what they mean, including why a reroute or wire count that RISES right after "
		"bp_format is debt made visible rather than a regression to revert.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_ExtractMacro::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Extract_AddCommonSchema(Builder);
	return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_ExtractMacro::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	TSharedPtr<FJsonObject> Params;
	FString SessionId;
	FBlueprintEditToolData* Data = nullptr;
	FToolResult Error;
	if (!BeginSessionOp(Arguments, TEXT("extract_macro"), Params, SessionId, Data, Error))
	{
		return Error;
	}
	return ClaireonExtractApplyInternal::ApplyExtract(
		*this, EExtractKind::Macro, SessionId, Data, Params);
}

// ---------------------------------------------------------------- composite

FString ClaireonBlueprintGraphTool_ExtractComposite::GetOperation() const { return TEXT("extract_composite"); }

FString ClaireonBlueprintGraphTool_ExtractComposite::GetDescription() const
{
	return TEXT("Move selected nodes into a composite sub-graph -- the right-click 'Collapse "
		"Nodes'. The nodes live in a nested graph reached by double-clicking the collapsed node; "
		"nothing becomes callable. The most permissive of the three engine collapses, so it is "
		"the fallback when bp_extract_function is refused. Needs an open session and an "
		"interactive editor. Transactional.");
}

FString ClaireonBlueprintGraphTool_ExtractComposite::GetFullDescription() const
{
	return TEXT(
		"Runs FBlueprintEditor's OWN 'Collapse Nodes', through ClaireonBlueprintEditorAccess. The "
		"nodes move into a nested graph reached by double-clicking the collapsed node; nothing "
		"becomes callable, nothing is added to the class, and nothing inlines anywhere else. Needs "
		"an open session and a live interactive editor. Transactional."
		"\n\n"
		"THE MOST PERMISSIVE OF THE THREE ENGINE COLLAPSES, which makes it the fallback when "
		"bp_extract_function refuses. It accepts latent nodes, multiple exec paths and "
		"enclosing-graph locals, because the composite expands in the caller's scope. Reach for "
		"it to get a region out of the way; reach for bp_extract_function when the region deserves "
		"a name and a boundary."
		"\n\n"
		"ONE STRUCTURAL CAVEAT specific to this kind. FBlueprintEditor::CollapseNodes takes no "
		"graph argument: it reads its own focused graph, calls Modify() on THAT graph, and creates "
		"the composite there. So this tool asserts POSITIVELY, before the transaction opens and "
		"before any mutating API is called, that the graph it resolved is the graph the editor has "
		"focused -- and refuses naming both if they differ. Without that assertion a mismatch would "
		"mutate a graph the tool never named and then report that the collapse produced nothing."
		"\n\n"
		"BEFORE YOU RESTRUCTURE, read the judgement rules: instructions_read(\"blueprint-authoring\") "
		"or claireon://instructions/blueprint-authoring. A composite hides a region without "
		"decomposing it, so it is the collapse most easily used to make a lint count fall without "
		"the graph getting better -- and that doc is explicit that a count is not the goal.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_ExtractComposite::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Extract_AddCommonSchema(Builder);
	return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_ExtractComposite::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	TSharedPtr<FJsonObject> Params;
	FString SessionId;
	FBlueprintEditToolData* Data = nullptr;
	FToolResult Error;
	if (!BeginSessionOp(Arguments, TEXT("extract_composite"), Params, SessionId, Data, Error))
	{
		return Error;
	}
	return ClaireonExtractApplyInternal::ApplyExtract(
		*this, EExtractKind::Composite, SessionId, Data, Params);
}

// ---------------------------------------------------------------- event

FString ClaireonBlueprintGraphTool_ExtractEvent::GetOperation() const { return TEXT("extract_event"); }

FString ClaireonBlueprintGraphTool_ExtractEvent::GetDescription() const
{
	return TEXT("Move a terminal selection under a new Custom Event and call it from the "
		"original site; use bp_extract_function otherwise. No menu equivalent, so it refuses, "
		"with evidence, an exec edge leaving the selection or a crossing data pin. An exec join "
		"is RELOCATED, not removed: every inbound edge lands on the one call node. Needs an open "
		"session and a live editor.");
}

FString ClaireonBlueprintGraphTool_ExtractEvent::GetFullDescription() const
{
	return TEXT(
		"Moves a terminal selection under a new Custom Event and calls it from the original site. "
		"Needs an open session and a live interactive editor. Transactional."
		"\n\n"
		"NO MENU EQUIVALENT, which is why this is the strictest of the four. The editor registers "
		"exactly three collapse commands -- CollapseNodes, CollapseSelectionToFunction, "
		"CollapseSelectionToMacro -- and none of them produces a custom event. So this is new "
		"behaviour rather than a port, and it is deliberately restricted rather than permissive: "
		"an event runs when it is called, on its own exec entry, so a selection that is not "
		"terminal would run at a different time after the move."
		"\n\n"
		"TWO PRECONDITIONS, both refused WITH EVIDENCE rather than with a bare no. An exec edge "
		"LEAVING the selection: whatever followed the selection would no longer follow it, because "
		"the call node returns immediately and the extracted chain continues on its own. A data pin "
		"CROSSING the boundary: a custom event's parameters are its own, so a crossing data pin has "
		"nowhere to come from. Use bp_extract_function for either -- it has real parameters and a "
		"real return path."
		"\n\n"
		"AN EXEC JOIN IS RELOCATED, NOT REMOVED. Several inbound edges converging on the selection "
		"all land on the one call node instead. This is stated because 'extract' reads as 'remove', "
		"and a caller rediscovered it from scratch when nothing said so."
		"\n\n"
		"BEFORE YOU RESTRUCTURE, read the judgement rules: instructions_read(\"blueprint-authoring\") "
		"or claireon://instructions/blueprint-authoring -- in particular the sections on execution "
		"topology and on why an exec join is the thing to fix rather than the thing to route "
		"around.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_ExtractEvent::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Extract_AddCommonSchema(Builder);
	return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_ExtractEvent::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	TSharedPtr<FJsonObject> Params;
	FString SessionId;
	FBlueprintEditToolData* Data = nullptr;
	FToolResult Error;
	if (!BeginSessionOp(Arguments, TEXT("extract_event"), Params, SessionId, Data, Error))
	{
		return Error;
	}
	return ClaireonExtractApplyInternal::ApplyExtract(
		*this, EExtractKind::Event, SessionId, Data, Params);
}

#undef LOCTEXT_NAMESPACE
