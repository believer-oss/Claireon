// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonLintTypes.h"
#include "ClaireonExecTopology.h"
#include "ClaireonGraphIslands.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"

namespace ClaireonLint
{

namespace ClaireonLintLayoutInternal
{
	FString ClaireonLintLayout_NodeId(const UEdGraphNode* Node)
	{
		return IsValid(Node) ? Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) : TEXT("<null>");
	}

	using FIslandBox = ClaireonGraphIslands::FIslandBox;
	using FIsland = ClaireonGraphIslands::FIsland;

	inline bool ClaireonLintLayout_IsCountedNode(const UEdGraphNode* Node)
	{
		return ClaireonGraphIslands::IsIslandMember(Node);
	}

	inline void ClaireonLintLayout_BuildIslands(const UEdGraph* Graph, TArray<FIsland>& OutIslands)
	{
		ClaireonGraphIslands::Build(Graph, OutIslands);
	}

	// Group collapsed edges by source pin and connected knot component so fan-out
	// produces one finding per chain. Knot adjacency defines groups; CollapseEdges
	// remains responsible for signal endpoints.

	/** A node's anchor position in graph units. Node BOUNDS need a live Slate widget; anchors do not. */
	struct FClaireonLintLayout_Anchor
	{
		double X = 0.0;
		double Y = 0.0;
	};

	inline FClaireonLintLayout_Anchor ClaireonLintLayout_NodeAnchor(const UEdGraphNode* Node)
	{
		FClaireonLintLayout_Anchor Anchor;
		if (IsValid(Node))
		{
			Anchor.X = static_cast<double>(Node->NodePosX);
			Anchor.Y = static_cast<double>(Node->NodePosY);
		}
		return Anchor;
	}

	inline double ClaireonLintLayout_AnchorDistance(
		const FClaireonLintLayout_Anchor& A, const FClaireonLintLayout_Anchor& B)
	{
		const double DX = B.X - A.X;
		const double DY = B.Y - A.Y;
		return FMath::Sqrt(DX * DX + DY * DY);
	}

	/** One terminal consumer of a knot chain, measured along THAT consumer's knot order. */
	struct FClaireonLintLayout_ChainConsumer
	{
		const UEdGraphNode* Node = nullptr;
		const UEdGraphPin* Pin = nullptr;

		/** Straight-line source-to-consumer distance. */
		double Span = 0.0;

		/**
		 * Routed source/knot/consumer length divided by Span. Zero means unmeasurable
		 * when Span is degenerate; keep the sentinel finite for JSON.
		 */
		double DetourRatio = 0.0;

		/** Segments of that routed path whose dx opposes the overall dx. 0 when the overall dx is 0. */
		int32 Reversals = 0;
	};

	/**
	 * A chain is one source output pin paired with one connected knot component.
	 * One pin may feed multiple components; multiple sources may feed one component.
	 * Each chain contains only knots traversed by its own collapsed edges.
	 */
	struct FClaireonLintLayout_KnotChain
	{
		const UEdGraphNode* FromNode = nullptr;
		const UEdGraphPin* FromPin = nullptr;

		/** Precomputed so the sort comparator does no string building. */
		FString FromNodeId;
		FString FromPinName;

		/**
		 * Union of forward-traversed ViaKnots, sorted by GUID for stable identity.
		 * Do not include unrelated knots merely because they share the component.
		 */
		TArray<FString> ViaKnotIds;

		/** Deterministically ordered; see the sort in ClaireonLintLayout_BuildKnotChains. */
		TArray<FClaireonLintLayout_ChainConsumer> Consumers;

		/** Largest Span among Consumers. What long-reroute reports and tests against. */
		double MaxSpan = 0.0;

		/** Kind of the SOURCE pin, which is what picks long-reroute's threshold. */
		bool bExec = false;
	};

	/** Build undirected knot components so merged paths remain in the same visual chain. */
	void ClaireonLintLayout_BuildKnotComponents(
		const UEdGraph* Graph,
		TMap<FGuid, const UEdGraphNode*>& OutKnotById,
		TMap<FGuid, int32>& OutComponentOf)
	{
		if (!IsValid(Graph))
		{
			return;
		}

		TArray<const UEdGraphNode*> Knots;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (ClaireonExecTopology::IsKnot(Node))
			{
				Knots.Add(Node);
				OutKnotById.Add(Node->NodeGuid, Node);
			}
		}

		int32 NextComponent = 0;
		TArray<const UEdGraphNode*> Frontier;
		for (const UEdGraphNode* Seed : Knots)
		{
			if (OutComponentOf.Contains(Seed->NodeGuid))
			{
				continue;
			}

			const int32 Component = NextComponent++;
			OutComponentOf.Add(Seed->NodeGuid, Component);
			Frontier.Reset();
			Frontier.Add(Seed);

			while (Frontier.Num() > 0)
			{
				const UEdGraphNode* Current = Frontier.Pop(EAllowShrinking::No);
				for (const UEdGraphPin* Pin : Current->Pins)
				{
					if (!Pin)
					{
						continue;
					}
					// Walk both pin directions because knot adjacency is undirected.
					for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						const UEdGraphNode* Neighbour = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
						if (!ClaireonExecTopology::IsKnot(Neighbour)
							|| OutComponentOf.Contains(Neighbour->NodeGuid))
						{
							continue;
						}
						OutComponentOf.Add(Neighbour->NodeGuid, Component);
						Frontier.Add(Neighbour);
					}
				}
			}
		}
	}

	/**
	 * Every knot chain in Graph, with per-consumer geometry, in a stable emission order.
	 *
	 * Endpoints come from CollapseEdges and only from there. bIncludeDirect is false because
	 * a knotless link is long-wire's subject, not these rules'.
	 */
	void ClaireonLintLayout_BuildKnotChains(
		const UEdGraph* Graph, TArray<FClaireonLintLayout_KnotChain>& OutChains)
	{
		OutChains.Reset();
		if (!IsValid(Graph))
		{
			return;
		}

		TMap<FGuid, const UEdGraphNode*> KnotById;
		TMap<FGuid, int32> ComponentOf;
		ClaireonLintLayout_BuildKnotComponents(Graph, KnotById, ComponentOf);
		if (ComponentOf.Num() == 0)
		{
			return;
		}

		// Use the map for lookup only; sort chains before emission.
		TMap<TPair<const UEdGraphPin*, int32>, int32> ChainIndexByKey;

		for (const FClaireonCollapsedEdge& Edge : ClaireonExecTopology::CollapseEdges(Graph, /*bIncludeDirect=*/false))
		{
			if (!IsValid(Edge.FromNode) || !IsValid(Edge.ToNode) || Edge.ViaKnots.Num() == 0)
			{
				continue;
			}

			const int32* Component = ComponentOf.Find(Edge.ViaKnots[0]);
			if (!Component)
			{
				continue;
			}

			const FClaireonLintLayout_Anchor Source = ClaireonLintLayout_NodeAnchor(Edge.FromNode);
			const FClaireonLintLayout_Anchor Consumer = ClaireonLintLayout_NodeAnchor(Edge.ToNode);

			TArray<FClaireonLintLayout_Anchor> Path;
			Path.Reserve(Edge.ViaKnots.Num() + 2);
			Path.Add(Source);
			for (const FGuid& Knot : Edge.ViaKnots)
			{
				const UEdGraphNode* const* KnotNode = KnotById.Find(Knot);
				// A missing knot reuses the previous anchor to avoid inventing distance. Copy it
				// before Add: TArray rejects references into itself.
				const FClaireonLintLayout_Anchor KnotAnchor = KnotNode
					? ClaireonLintLayout_NodeAnchor(*KnotNode)
					: Path.Last();
				Path.Add(KnotAnchor);
			}
			Path.Add(Consumer);

			FClaireonLintLayout_ChainConsumer Entry;
			Entry.Node = Edge.ToNode;
			Entry.Pin = Edge.ToPin;
			Entry.Span = ClaireonLintLayout_AnchorDistance(Source, Consumer);

			const double OverallDX = Consumer.X - Source.X;
			double Routed = 0.0;
			for (int32 I = 0; I + 1 < Path.Num(); ++I)
			{
				Routed += ClaireonLintLayout_AnchorDistance(Path[I], Path[I + 1]);
				const double SegmentDX = Path[I + 1].X - Path[I].X;
				// Vertical segments are jogs, not horizontal reversals.
				if (OverallDX != 0.0 && SegmentDX * OverallDX < 0.0)
				{
					++Entry.Reversals;
				}
			}
			Entry.DetourRatio = (Entry.Span > KINDA_SMALL_NUMBER) ? (Routed / Entry.Span) : 0.0;

			const TPair<const UEdGraphPin*, int32> Key(Edge.FromPin, *Component);
			int32* ExistingIndex = ChainIndexByKey.Find(Key);
			if (!ExistingIndex)
			{
				FClaireonLintLayout_KnotChain Chain;
				Chain.FromNode = Edge.FromNode;
				Chain.FromPin = Edge.FromPin;
				Chain.FromNodeId = ClaireonLintLayout_NodeId(Edge.FromNode);
				Chain.FromPinName = Edge.FromPin ? Edge.FromPin->PinName.ToString() : FString(TEXT("<null>"));
				Chain.bExec = Edge.FromPin
					&& Edge.FromPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
				ExistingIndex = &ChainIndexByKey.Add(Key, OutChains.Add(MoveTemp(Chain)));
			}

			FClaireonLintLayout_KnotChain& Chain = OutChains[*ExistingIndex];
			for (const FGuid& Knot : Edge.ViaKnots)
			{
				Chain.ViaKnotIds.AddUnique(Knot.ToString(EGuidFormats::DigitsWithHyphens));
			}

			// Deduplicate consumers by node/pin, retaining the worst route by detour then
			// reversals. Multiple paths to one input still count as one consumer.
			FClaireonLintLayout_ChainConsumer* Duplicate = Chain.Consumers.FindByPredicate(
				[&Entry](const FClaireonLintLayout_ChainConsumer& Candidate)
				{
					return Candidate.Node == Entry.Node && Candidate.Pin == Entry.Pin;
				});
			if (!Duplicate)
			{
				Chain.Consumers.Add(MoveTemp(Entry));
			}
			else if (Entry.DetourRatio > Duplicate->DetourRatio
				|| (Entry.DetourRatio == Duplicate->DetourRatio && Entry.Reversals > Duplicate->Reversals))
			{
				*Duplicate = MoveTemp(Entry);
			}
		}

		for (FClaireonLintLayout_KnotChain& Chain : OutChains)
		{
			Chain.ViaKnotIds.Sort();
			Chain.Consumers.Sort(
				[](const FClaireonLintLayout_ChainConsumer& A, const FClaireonLintLayout_ChainConsumer& B)
				{
					const FString AId = ClaireonLintLayout_NodeId(A.Node);
					const FString BId = ClaireonLintLayout_NodeId(B.Node);
					if (AId != BId)
					{
						return AId < BId;
					}
					const FString APin = A.Pin ? A.Pin->PinName.ToString() : FString();
					const FString BPin = B.Pin ? B.Pin->PinName.ToString() : FString();
					return APin < BPin;
				});
			for (const FClaireonLintLayout_ChainConsumer& Consumer : Chain.Consumers)
			{
				Chain.MaxSpan = FMath::Max(Chain.MaxSpan, Consumer.Span);
			}
		}

		// Sort by source GUID, source pin, then lowest knot GUID for deterministic output.
		OutChains.Sort(
			[](const FClaireonLintLayout_KnotChain& A, const FClaireonLintLayout_KnotChain& B)
			{
				if (A.FromNodeId != B.FromNodeId)
				{
					return A.FromNodeId < B.FromNodeId;
				}
				if (A.FromPinName != B.FromPinName)
				{
					return A.FromPinName < B.FromPinName;
				}
				const FString ALowest = A.ViaKnotIds.Num() > 0 ? A.ViaKnotIds[0] : FString();
				const FString BLowest = B.ViaKnotIds.Num() > 0 ? B.ViaKnotIds[0] : FString();
				return ALowest < BLowest;
			});
	}

	/** Build fresh evidence per finding to avoid aliasing rule-specific fields. */
	void ClaireonLintLayout_FillChainEvidence(
		const FClaireonLintLayout_KnotChain& Chain, const TSharedPtr<FJsonObject>& Evidence)
	{
		if (!Evidence.IsValid())
		{
			return;
		}

		Evidence->SetStringField(TEXT("pin_kind"), Chain.bExec ? TEXT("exec") : TEXT("data"));
		Evidence->SetNumberField(TEXT("knot_count"), Chain.ViaKnotIds.Num());
		Evidence->SetNumberField(TEXT("fan_out"), Chain.Consumers.Num());
		Evidence->SetNumberField(TEXT("max_span"), Chain.MaxSpan);

		TArray<TSharedPtr<FJsonValue>> KnotValues;
		for (const FString& KnotId : Chain.ViaKnotIds)
		{
			KnotValues.Add(MakeShared<FJsonValueString>(KnotId));
		}
		Evidence->SetArrayField(TEXT("via_knots"), KnotValues);

		// Include every consumer to expose distribution trunks as well as serial routes.
		TArray<TSharedPtr<FJsonValue>> ConsumerValues;
		for (const FClaireonLintLayout_ChainConsumer& Consumer : Chain.Consumers)
		{
			TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("node"), ClaireonLintLayout_NodeId(Consumer.Node));
			Object->SetStringField(TEXT("pin"),
				Consumer.Pin ? Consumer.Pin->PinName.ToString() : FString(TEXT("<null>")));
			Object->SetNumberField(TEXT("span"), Consumer.Span);
			Object->SetNumberField(TEXT("detour_ratio"), Consumer.DetourRatio);
			Object->SetNumberField(TEXT("reversals"), Consumer.Reversals);
			ConsumerValues.Add(MakeShared<FJsonValueObject>(Object));
		}
		Evidence->SetArrayField(TEXT("consumers"), ConsumerValues);

		// Record detour and reversals as evidence only; neither affects the predicates.
		Evidence->SetStringField(TEXT("detour_measurement_note"),
			TEXT("detour_ratio and reversals are evidence only and are deliberately absent from every predicate here: measured over the authored-graph corpus, reversals was 0 on all 8 chain-consumer pairs and detour_ratio ran 1.026 median / 1.247 max, so candidate thresholds of 1.5 and 2.0 fire on ZERO pairs. They are recorded so a later corpus sweep needs no re-instrumentation."));
	}
}
using namespace ClaireonLintLayoutInternal;

void RunLayoutRules(const FClaireonLintContext& Context, TArray<FClaireonLintFinding>& OutFindings)
{
	if (!IsValid(Context.Graph))
	{
		return;
	}

	// Measure anchor distances for headless reproducibility, including links to knots.
	// Splitting a long wire reduces each segment naturally without exempting reroutes.
	for (UEdGraphNode* Node : Context.Graph->Nodes)
	{
		if (!IsValid(Node))
		{
			continue;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output)
			{
				continue;
			}

			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				UEdGraphNode* Target = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
				if (!IsValid(Target))
				{
					continue;
				}

				const double DX = static_cast<double>(Target->NodePosX - Node->NodePosX);
				const double DY = static_cast<double>(Target->NodePosY - Node->NodePosY);
				const double Distance = FMath::Sqrt(DX * DX + DY * DY);

				if (Distance > Context.Thresholds.LongWire)
				{
					FClaireonLintFinding Finding;
					Finding.Rule = TEXT("long-wire");
					Finding.Scope = EClaireonLintScope::Layout;
					Finding.Severity = EClaireonLintSeverity::Info;
					Finding.Confidence = EClaireonLintConfidence::High;
					Finding.Target = ClaireonLintLayout_NodeId(Node) + TEXT(".") + Pin->PinName.ToString();
					Finding.Evidence = MakeShared<FJsonObject>();
					Finding.Evidence->SetNumberField(TEXT("distance"), Distance);
					Finding.Evidence->SetNumberField(TEXT("threshold"), Context.Thresholds.LongWire);
					Finding.Evidence->SetStringField(TEXT("to"), ClaireonLintLayout_NodeId(Target));
					Finding.Message = FString::Printf(
						TEXT("Wire from '%s' spans %.0f units (threshold %.0f). Long wires are the single biggest contributor to unreadable graphs; a local re-fetch or a reroute jog reads better."),
						*Pin->PinName.ToString(), Distance, Context.Thresholds.LongWire);
					OutFindings.Add(MoveTemp(Finding));
				}

				if (DX < 0.0)
				{
					FClaireonLintFinding Finding;
					Finding.Rule = TEXT("backward-wire");
					Finding.Scope = EClaireonLintScope::Layout;
					Finding.Severity = EClaireonLintSeverity::Info;
					Finding.Confidence = EClaireonLintConfidence::High;
					Finding.Target = ClaireonLintLayout_NodeId(Node) + TEXT(".") + Pin->PinName.ToString();
					Finding.Evidence = MakeShared<FJsonObject>();
					Finding.Evidence->SetNumberField(TEXT("dx"), DX);
					Finding.Evidence->SetStringField(TEXT("to"), ClaireonLintLayout_NodeId(Target));
					Finding.Message = FString::Printf(
						TEXT("Wire from '%s' runs right-to-left (dx %.0f). Backward wires break the left-to-right reading order a reader relies on."),
						*Pin->PinName.ToString(), DX);
					OutFindings.Add(MoveTemp(Finding));
				}
			}
		}
	}

	// Evaluate end-to-end spans per chain; long-wire handles individual segments.
	// Fan-out is not exempt. Detour ratios and reversals are evidence only.
	// RuleVersion 2 identifies chain-level granularity and target grammar.
	{
		TArray<FClaireonLintLayout_KnotChain> Chains;
		ClaireonLintLayout_BuildKnotChains(Context.Graph, Chains);

		for (const FClaireonLintLayout_KnotChain& Chain : Chains)
		{
			if (Chain.Consumers.Num() == 0 || Chain.ViaKnotIds.Num() == 0)
			{
				continue;
			}

			const double Threshold = Chain.bExec
				? Context.Thresholds.RerouteSpanExec
				: Context.Thresholds.RerouteSpanData;

			// Lowest traversed knot GUID identifies the chain independently of consumer order.
			const FString ChainTarget = Chain.FromNodeId
				+ TEXT(".") + Chain.FromPinName
				+ TEXT("->via:") + Chain.ViaKnotIds[0];

			if (Chain.MaxSpan > Threshold)
			{
				FClaireonLintFinding Finding;
				Finding.Rule = TEXT("long-reroute");
				Finding.RuleVersion = 2;
				Finding.Scope = EClaireonLintScope::Layout;
				Finding.Severity = EClaireonLintSeverity::Info;
				Finding.Confidence = EClaireonLintConfidence::High;
				Finding.Target = ChainTarget;
				Finding.Evidence = MakeShared<FJsonObject>();
				ClaireonLintLayout_FillChainEvidence(Chain, Finding.Evidence);
				Finding.Evidence->SetNumberField(TEXT("threshold"), Threshold);
				Finding.Message = FString::Printf(
					TEXT("%s reroute chain of %d reroute(s) carries this signal up to %.0f units end to end to %d consumer(s) (threshold %.0f). Carrying a value that far is the wire the reroute was meant to avoid; a local re-fetch or a nearer producer reads better."),
					Chain.bExec ? TEXT("An execution") : TEXT("A data"),
					Chain.ViaKnotIds.Num(), Chain.MaxSpan, Chain.Consumers.Num(), Threshold);
				OutFindings.Add(MoveTemp(Finding));
			}

			if (Chain.ViaKnotIds.Num() >= 2)
			{
				FClaireonLintFinding Finding;
				Finding.Rule = TEXT("reroute-chain");
				Finding.RuleVersion = 2;
				Finding.Scope = EClaireonLintScope::Layout;
				Finding.Severity = EClaireonLintSeverity::Info;
				Finding.Confidence = EClaireonLintConfidence::High;
				Finding.Target = ChainTarget;
				Finding.Evidence = MakeShared<FJsonObject>();
				ClaireonLintLayout_FillChainEvidence(Chain, Finding.Evidence);
				Finding.Message = FString::Printf(
					TEXT("%d reroutes carry one signal from this pin to %d consumer(s). Each hop is a place a reader has to follow the wire by eye; one jog conveys the intent, a chain conveys the path."),
					Chain.ViaKnotIds.Num(), Chain.Consumers.Num());
				OutFindings.Add(MoveTemp(Finding));
			}
		}
	}

	// ---- island rules ----------------------------------------------------------
	TArray<FIsland> Islands;
	ClaireonLintLayout_BuildIslands(Context.Graph, Islands);

	// Comment membership uses rectangle coverage of island members.
	TArray<const UEdGraphNode_Comment*> Comments;
	for (const UEdGraphNode* Node : Context.Graph->Nodes)
	{
		if (const UEdGraphNode_Comment* Comment = Cast<const UEdGraphNode_Comment>(Node); IsValid(Comment))
		{
			Comments.Add(Comment);
		}
	}

	for (const FIsland& Island : Islands)
	{
		if (Island.Box.Width() > Context.Thresholds.IslandTooWide)
		{
			FClaireonLintFinding Finding;
			Finding.Rule = TEXT("island-too-wide");
			Finding.Scope = EClaireonLintScope::Layout;
			Finding.Severity = EClaireonLintSeverity::Info;
			Finding.Confidence = EClaireonLintConfidence::High;
			Finding.Target = Island.Representative;
			Finding.Evidence = MakeShared<FJsonObject>();
			Finding.Evidence->SetNumberField(TEXT("width"), Island.Box.Width());
			Finding.Evidence->SetNumberField(TEXT("threshold"), Context.Thresholds.IslandTooWide);
			Finding.Evidence->SetNumberField(TEXT("node_count"), Island.Nodes.Num());
			Finding.Message = FString::Printf(
				TEXT("This connected block spans %.0f units horizontally across %d nodes (threshold %.0f). Nothing that wide can be read without scrolling; extracting a stage into a function shortens the row."),
				Island.Box.Width(), Island.Nodes.Num(), Context.Thresholds.IslandTooWide);
			OutFindings.Add(MoveTemp(Finding));
		}

		if (Island.Nodes.Num() >= Context.Thresholds.UncommentedIslandMin)
		{
			bool bCommented = false;
			for (const UEdGraphNode_Comment* Comment : Comments)
			{
				const double MinX = static_cast<double>(Comment->NodePosX);
				const double MinY = static_cast<double>(Comment->NodePosY);
				const double MaxX = MinX + static_cast<double>(Comment->NodeWidth);
				const double MaxY = MinY + static_cast<double>(Comment->NodeHeight);
				for (const UEdGraphNode* Member : Island.Nodes)
				{
					const double X = static_cast<double>(Member->NodePosX);
					const double Y = static_cast<double>(Member->NodePosY);
					if (X >= MinX && X <= MaxX && Y >= MinY && Y <= MaxY)
					{
						bCommented = true;
						break;
					}
				}
				if (bCommented)
				{
					break;
				}
			}

			if (!bCommented)
			{
				FClaireonLintFinding Finding;
				Finding.Rule = TEXT("uncommented-island");
				Finding.Scope = EClaireonLintScope::Layout;
				Finding.Severity = EClaireonLintSeverity::Info;
				Finding.Confidence = EClaireonLintConfidence::Medium;
				Finding.Target = Island.Representative;
				Finding.Evidence = MakeShared<FJsonObject>();
				Finding.Evidence->SetNumberField(TEXT("node_count"), Island.Nodes.Num());
				Finding.Evidence->SetNumberField(TEXT("threshold"), Context.Thresholds.UncommentedIslandMin);
				Finding.Evidence->SetNumberField(TEXT("comment_nodes_in_graph"), Comments.Num());
				Finding.Message = FString::Printf(
					TEXT("%d connected nodes with no comment box over any of them. A comment naming what the block is for is the cheapest readability win in a graph this size."),
					Island.Nodes.Num());
				OutFindings.Add(MoveTemp(Finding));
			}
		}
	}

	// Align primary entries to the shared ResolveRailX rail; feeders may extend left
	// of an entry, so Box.MinX is not the alignment coordinate. Flag rightward offsets
	// and vertical overlap among islands in the column. Skip singletons to preserve
	// scratch nodes. Anchor boxes can miss visual overlap between node bodies.
	{
		const double RailX = ClaireonGraphIslands::ResolveRailX(Context.Graph);

		// Use the same entry predicate as ResolveRailX to report the rail source.
		bool bRailFromEntryNode = false;
		for (const UEdGraphNode* Node : Context.Graph->Nodes)
		{
			if (ClaireonGraphIslands::IsEntryNode(Node))
			{
				bRailFromEntryNode = true;
				break;
			}
		}
		const TCHAR* RailSource = bRailFromEntryNode ? TEXT("entry_node") : TEXT("leftmost_island");

		// Check vertical overlap only between in-column islands. An off-rail island
		// already has one finding; pairing it with every vertical neighbor duplicates it.
		TArray<bool> bInColumn;
		bInColumn.Reserve(Islands.Num());
		for (const FIsland& Island : Islands)
		{
			bInColumn.Add(
				ClaireonGraphIslands::ResolveIslandEntryX(Island) - RailX
					<= Context.Thresholds.IslandRailTolerance);
		}

		for (int32 I = 0; I < Islands.Num(); ++I)
		{
			if (Islands[I].Nodes.Num() < 2 || !bInColumn[I])
			{
				continue;
			}
			for (int32 J = I + 1; J < Islands.Num(); ++J)
			{
				if (Islands[J].Nodes.Num() < 2 || !bInColumn[J])
				{
					continue;
				}

				const bool bYOverlap = Islands[I].Box.MinY <= Islands[J].Box.MaxY
					&& Islands[J].Box.MinY <= Islands[I].Box.MaxY;
				if (!bYOverlap)
				{
					continue;
				}

				const double YOverlap = FMath::Min(Islands[I].Box.MaxY, Islands[J].Box.MaxY)
					- FMath::Max(Islands[I].Box.MinY, Islands[J].Box.MinY);

				FClaireonLintFinding Finding;
				Finding.Rule = TEXT("island-column");
				Finding.Scope = EClaireonLintScope::Layout;
				Finding.Severity = EClaireonLintSeverity::Warning;
				Finding.Confidence = EClaireonLintConfidence::High;
				Finding.Target = Islands[I].Representative + TEXT("|") + Islands[J].Representative;
				Finding.Evidence = MakeShared<FJsonObject>();
				Finding.Evidence->SetStringField(TEXT("island_a"), Islands[I].Representative);
				Finding.Evidence->SetStringField(TEXT("island_b"), Islands[J].Representative);
				Finding.Evidence->SetNumberField(TEXT("island_a_nodes"), Islands[I].Nodes.Num());
				Finding.Evidence->SetNumberField(TEXT("island_b_nodes"), Islands[J].Nodes.Num());
				Finding.Evidence->SetNumberField(TEXT("y_overlap"), YOverlap);
				Finding.Evidence->SetStringField(TEXT("condition"), TEXT("vertical_overlap"));
				Finding.Evidence->SetStringField(TEXT("measurement"),
					TEXT("Anchor bounding boxes, which understate real node extents; overlap detected here is real, absence is not proof of separation."));
				Finding.Message = FString::Printf(
					TEXT("Two unconnected blocks (%d and %d nodes) occupy overlapping rows, %.0f units of vertical overlap. Interleaved blocks read as one graph that makes no sense; bp_stack_islands translates each island as a rigid unit onto one column, which is what satisfies this rule."),
					Islands[I].Nodes.Num(), Islands[J].Nodes.Num(), YOverlap);
				OutFindings.Add(MoveTemp(Finding));
			}
		}

		for (const FIsland& Island : Islands)
		{
			if (Island.Nodes.Num() < 2)
			{
				continue;
			}

			const double EntryX = ClaireonGraphIslands::ResolveIslandEntryX(Island);
			const double Offset = EntryX - RailX;
			if (Offset <= Context.Thresholds.IslandRailTolerance)
			{
				continue;
			}

			bool bHasEntryNode = false;
			for (const UEdGraphNode* Node : Island.Nodes)
			{
				if (ClaireonGraphIslands::IsEntryNode(Node))
				{
					bHasEntryNode = true;
					break;
				}
			}

			FClaireonLintFinding Finding;
			Finding.Rule = TEXT("island-column");
			Finding.Scope = EClaireonLintScope::Layout;
			Finding.Severity = EClaireonLintSeverity::Info;
			Finding.Confidence = EClaireonLintConfidence::High;
			Finding.Target = Island.Representative;
			Finding.Evidence = MakeShared<FJsonObject>();
			Finding.Evidence->SetNumberField(TEXT("rail_x"), RailX);
			Finding.Evidence->SetStringField(TEXT("rail_source"), RailSource);
			Finding.Evidence->SetNumberField(TEXT("island_entry_x"), EntryX);
			Finding.Evidence->SetStringField(TEXT("entry_source"), bHasEntryNode ? TEXT("entry_node") : TEXT("box_min_x"));
			Finding.Evidence->SetNumberField(TEXT("offset"), Offset);
			Finding.Evidence->SetNumberField(TEXT("tolerance"), Context.Thresholds.IslandRailTolerance);
			Finding.Evidence->SetNumberField(TEXT("node_count"), Island.Nodes.Num());
			Finding.Evidence->SetStringField(TEXT("condition"), TEXT("off_rail"));
			Finding.Message = FString::Printf(
				TEXT("This block's entry sits %.0f units right of the island rail at x=%.0f (tolerance %.0f). bp_stack_islands translates each island as a rigid unit onto one column, which is what satisfies this rule."),
				Offset, RailX, Context.Thresholds.IslandRailTolerance);
			OutFindings.Add(MoveTemp(Finding));
		}
	}

	// Do not compare individual events with the rail: BlueprintAssist intentionally
	// places multiple entries within one island horizontally.
}

}
