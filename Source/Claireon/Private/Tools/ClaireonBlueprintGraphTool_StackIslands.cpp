// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintGraphTool_StackIslands.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonGraphIslands.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/MemberReference.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;
using ClaireonGraphIslands::FIsland;

// Prefix helpers to avoid unity-build collisions.
namespace ClaireonStackIslandsInternal
{
	/** Uppercased key so a caller's GUID casing cannot change which island they named. */
	FString StackIslands_Key(const FString& Guid)
	{
		return Guid.ToUpper();
	}

	FString StackIslands_Guid(const FGuid& Value)
	{
		return Value.ToString(EGuidFormats::DigitsWithHyphens);
	}

	/** Use the lexicographically smallest entry name for deterministic ordering of multi-entry islands. */
	FString StackIslands_EntryName(const FIsland& Island)
	{
		FString Best;
		for (const UEdGraphNode* Node : Island.Nodes)
		{
			FString Name;
			if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node); IsValid(Event))
			{
				Name = Event->GetFunctionName().ToString();
			}
			else if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node); IsValid(Entry))
			{
				Name = Entry->CustomGeneratedFunctionName != NAME_None
					? Entry->CustomGeneratedFunctionName.ToString()
					: Entry->FunctionReference.GetMemberName().ToString();

				// Fall back to the graph name when a function entry reference is unresolved.
				if (Name.IsEmpty() && IsValid(Entry->GetGraph()))
				{
					Name = Entry->GetGraph()->GetName();
				}
			}

			if (Name.IsEmpty())
			{
				continue;
			}
			if (Best.IsEmpty() || Name.Compare(Best, ESearchCase::CaseSensitive) < 0)
			{
				Best = Name;
			}
		}
		return Best;
	}

	/** Anchor-space union over a node set. Matches FIslandBox: anchors, never widget bounds. */
	ClaireonStackIslands::FStackBounds StackIslands_Bounds(const TArray<const FIsland*>& Islands)
	{
		ClaireonStackIslands::FStackBounds Bounds;
		for (const FIsland* Island : Islands)
		{
			for (const UEdGraphNode* Node : Island->Nodes)
			{
				if (!IsValid(Node))
				{
					continue;
				}
				const double X = static_cast<double>(Node->NodePosX);
				const double Y = static_cast<double>(Node->NodePosY);
				if (!Bounds.bValid)
				{
					Bounds.bValid = true;
					Bounds.MinX = Bounds.MaxX = X;
					Bounds.MinY = Bounds.MaxY = Y;
					continue;
				}
				Bounds.MinX = FMath::Min(Bounds.MinX, X);
				Bounds.MaxX = FMath::Max(Bounds.MaxX, X);
				Bounds.MinY = FMath::Min(Bounds.MinY, Y);
				Bounds.MaxY = FMath::Max(Bounds.MaxY, Y);
			}
		}
		return Bounds;
	}

	TSharedPtr<FJsonObject> StackIslands_BoundsJson(const ClaireonStackIslands::FStackBounds& Bounds)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("min_x"), Bounds.MinX);
		Obj->SetNumberField(TEXT("min_y"), Bounds.MinY);
		Obj->SetNumberField(TEXT("max_x"), Bounds.MaxX);
		Obj->SetNumberField(TEXT("max_y"), Bounds.MaxY);
		return Obj;
	}

	TArray<TSharedPtr<FJsonValue>> StackIslands_StringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Out.Add(MakeShared<FJsonValueString>(Value));
		}
		return Out;
	}

	/** One refusal shape, so the two argument-validation sites cannot word it differently. */
	bool StackIslands_Refuse(
		ClaireonStackIslands::FStackReport& OutReport,
		const TCHAR* RefusalReason,
		const FString& Message)
	{
		OutReport.Error = Message;
		OutReport.RefusalReason = RefusalReason;
		OutReport.Summary = Message;
		return false;
	}

	/**
	 * Accept string arrays or comma-separated strings.
	 * @return false only when the field is present and unusable.
	 */
	bool StackIslands_ReadStringList(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Field,
		TArray<FString>& OutValues,
		FString& OutError)
	{
		OutValues.Reset();
		if (!Params.IsValid())
		{
			return true;
		}

		// Use TryGetField for compatibility with FString and FSharedString map keys.
		const TSharedPtr<FJsonValue> FoundValue = Params->TryGetField(Field);
		const TSharedPtr<FJsonValue>* Value = FoundValue.IsValid() ? &FoundValue : nullptr;
		if (!Value || !Value->IsValid() || (*Value)->Type == EJson::Null)
		{
			return true;
		}

		if ((*Value)->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Element : (*Value)->AsArray())
			{
				FString AsString;
				if (!Element.IsValid() || !Element->TryGetString(AsString))
				{
					OutError = FString::Printf(
						TEXT("%s must be an array of GUID strings; one element is not a string."), Field);
					return false;
				}
				AsString.TrimStartAndEndInline();
				if (!AsString.IsEmpty())
				{
					OutValues.Add(AsString);
				}
			}
			return true;
		}

		FString Flat;
		if (!(*Value)->TryGetString(Flat))
		{
			OutError = FString::Printf(
				TEXT("%s must be a string array or a comma-separated string."), Field);
			return false;
		}
		Flat.ParseIntoArray(OutValues, TEXT(","), /*InCullEmpty=*/true);
		for (FString& Entry : OutValues)
		{
			Entry.TrimStartAndEndInline();
		}
		return true;
	}
}

using namespace ClaireonStackIslandsInternal;

namespace ClaireonStackIslands
{
	const TCHAR* ToWireString(EStackOrder Order)
	{
		switch (Order)
		{
		case EStackOrder::ByEntryName: return TEXT("by_entry_name");
		case EStackOrder::Explicit:    return TEXT("explicit");
		case EStackOrder::ByCurrentY:
		default:                       return TEXT("by_current_y");
		}
	}

	bool ParseOrder(const FString& Wire, EStackOrder& OutOrder)
	{
		if (Wire.Equals(TEXT("by_current_y"), ESearchCase::IgnoreCase))  { OutOrder = EStackOrder::ByCurrentY;  return true; }
		if (Wire.Equals(TEXT("by_entry_name"), ESearchCase::IgnoreCase)) { OutOrder = EStackOrder::ByEntryName; return true; }
		if (Wire.Equals(TEXT("explicit"), ESearchCase::IgnoreCase))      { OutOrder = EStackOrder::Explicit;    return true; }
		return false;
	}

	const TCHAR* PolicySingleton()         { return TEXT("singleton"); }
	const TCHAR* PolicyExcludedByRequest() { return TEXT("excluded_by_request"); }
	const TCHAR* PolicySpanningComment()   { return TEXT("spanning_comment"); }
	const TCHAR* PolicyNoIslandMember()    { return TEXT("no_island_member"); }
	const TCHAR* PolicyIslandNotStacked()  { return TEXT("island_not_stacked"); }

	bool Apply(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const FStackRequest& Request,
		FStackReport& OutReport)
	{
		OutReport = FStackReport();
		OutReport.Gutter = Request.Gutter;
		OutReport.Order = Request.Order;
		OutReport.bCarryComments = Request.bCarryComments;
		OutReport.bIncludeSingletons = Request.bIncludeSingletons;

		if (!IsValid(Blueprint) || !IsValid(Graph))
		{
			return StackIslands_Refuse(OutReport, TEXT("bad_argument"),
				TEXT("Blueprint or Graph is no longer valid."));
		}

		// Validate arguments before reading or moving the graph.
		if (Request.Gutter < 0.0)
		{
			// Negative gutters would deliberately overlap islands.
			return StackIslands_Refuse(OutReport, TEXT("bad_argument"),
				FString::Printf(TEXT("gutter must be >= 0; got %g. A negative gutter overlaps the islands this tool exists to separate."), Request.Gutter));
		}
		if (Request.Order == EStackOrder::Explicit && Request.IslandOrder.Num() == 0)
		{
			return StackIslands_Refuse(OutReport, TEXT("bad_argument"),
				TEXT("order='explicit' requires island_order; pass the island representative GUIDs in the order to stack."));
		}
		if (Request.Order != EStackOrder::Explicit && Request.IslandOrder.Num() > 0)
		{
			return StackIslands_Refuse(OutReport, TEXT("bad_argument"),
				FString::Printf(TEXT("island_order is only valid with order='explicit'; got order='%s'. It would otherwise be silently ignored."),
					ToWireString(Request.Order)));
		}

		TArray<FIsland> Islands;
		ClaireonGraphIslands::Build(Graph, Islands);
		OutReport.IslandsTotal = Islands.Num();

		OutReport.RailX = Request.bRailXExplicit
			? Request.RailX
			: ClaireonGraphIslands::ResolveRailX(Graph);
		OutReport.RailNextDefault = OutReport.RailX;

		if (Islands.Num() == 0)
		{
			return StackIslands_Refuse(OutReport, TEXT("no_islands"),
				FString::Printf(TEXT("Graph '%s' has no islands to stack."), *Graph->GetName()));
		}

		TMap<FString, int32> RepToIndex;
		RepToIndex.Reserve(Islands.Num());
		for (int32 Index = 0; Index < Islands.Num(); ++Index)
		{
			RepToIndex.Add(StackIslands_Key(Islands[Index].Representative), Index);
		}

		// Unknown exclusions must fail; silently ignoring one would widen the mutation.
		TSet<int32> Excluded;
		TArray<FString> UnknownExcludes;
		for (const FString& Entry : Request.ExcludeIslands)
		{
			if (const int32* Index = RepToIndex.Find(StackIslands_Key(Entry)))
			{
				Excluded.Add(*Index);
			}
			else
			{
				UnknownExcludes.Add(Entry);
			}
		}
		if (UnknownExcludes.Num() > 0)
		{
			return StackIslands_Refuse(OutReport, TEXT("bad_argument"),
				FString::Printf(TEXT("exclude_islands names %d GUID(s) that are not island representatives in graph '%s': %s. Representatives come from the islands_* rows of bp_lint or a prior bp_stack_islands call."),
					UnknownExcludes.Num(), *Graph->GetName(), *FString::Join(UnknownExcludes, TEXT(", "))));
		}

		// Record every policy exclusion.
		TArray<int32> ToStack;
		TMap<int32, FString> SkipReasonByIndex;
		for (int32 Index = 0; Index < Islands.Num(); ++Index)
		{
			const TCHAR* Reason = nullptr;
			if (Excluded.Contains(Index))
			{
				Reason = PolicyExcludedByRequest();
			}
			else if (!Request.bIncludeSingletons && Islands[Index].Nodes.Num() == 1)
			{
				Reason = PolicySingleton();
			}

			if (Reason)
			{
				SkipReasonByIndex.Add(Index, Reason);
				FSkippedIsland& Row = OutReport.IslandsSkipped.AddDefaulted_GetRef();
				Row.Representative = Islands[Index].Representative;
				Row.NodeCount = Islands[Index].Nodes.Num();
				Row.Reason = Reason;
			}
			else
			{
				ToStack.Add(Index);
			}
		}

		// Choose a deterministic island order.
		TArray<int32> Ordered;
		if (Request.Order == EStackOrder::Explicit)
		{
			// Require island_order to be a permutation of the to-stack set.
			TArray<FString> Unknown;
			TArray<FString> Duplicated;
			TArray<FString> NamedButSkipped;
			TSet<int32> Seen;
			for (const FString& Entry : Request.IslandOrder)
			{
				const int32* Index = RepToIndex.Find(StackIslands_Key(Entry));
				if (!Index)
				{
					Unknown.Add(Entry);
					continue;
				}
				if (const FString* Reason = SkipReasonByIndex.Find(*Index))
				{
					// Reject explicit ordering of an island excluded by another policy.
					NamedButSkipped.Add(FString::Printf(TEXT("%s (%s)"), *Entry, **Reason));
					continue;
				}
				if (Seen.Contains(*Index))
				{
					Duplicated.Add(Entry);
					continue;
				}
				Seen.Add(*Index);
				Ordered.Add(*Index);
			}

			TArray<FString> Missing;
			for (const int32 Index : ToStack)
			{
				if (!Seen.Contains(Index))
				{
					Missing.Add(Islands[Index].Representative);
				}
			}

			if (Unknown.Num() > 0 || Duplicated.Num() > 0 || NamedButSkipped.Num() > 0 || Missing.Num() > 0)
			{
				TArray<FString> Parts;
				if (Missing.Num() > 0)
				{
					Parts.Add(FString::Printf(TEXT("omits %d island(s) that would be stacked: %s"),
						Missing.Num(), *FString::Join(Missing, TEXT(", "))));
				}
				if (Unknown.Num() > 0)
				{
					Parts.Add(FString::Printf(TEXT("names %d GUID(s) that are not island representatives in this graph: %s"),
						Unknown.Num(), *FString::Join(Unknown, TEXT(", "))));
				}
				if (Duplicated.Num() > 0)
				{
					Parts.Add(FString::Printf(TEXT("repeats %s"), *FString::Join(Duplicated, TEXT(", "))));
				}
				if (NamedButSkipped.Num() > 0)
				{
					Parts.Add(FString::Printf(TEXT("names %s, which policy skips -- drop the entry, or pass include_singletons/adjust exclude_islands"),
						*FString::Join(NamedButSkipped, TEXT(", "))));
				}
				return StackIslands_Refuse(OutReport, TEXT("bad_argument"),
					FString::Printf(TEXT("island_order must be a permutation of the islands this call would stack; it %s. Nothing was moved."),
						*FString::Join(Parts, TEXT("; "))));
			}
		}
		else if (Request.Order == EStackOrder::ByEntryName)
		{
			// Islands without entries sort last, by representative.
			TMap<int32, FString> Names;
			for (const int32 Index : ToStack)
			{
				Names.Add(Index, StackIslands_EntryName(Islands[Index]));
			}
			Ordered = ToStack;
			Ordered.Sort([&Islands, &Names](int32 A, int32 B)
			{
				const FString& NameA = Names[A];
				const FString& NameB = Names[B];
				if (NameA.IsEmpty() != NameB.IsEmpty())
				{
					return NameB.IsEmpty();
				}
				const int32 Compare = NameA.Compare(NameB, ESearchCase::CaseSensitive);
				return Compare != 0
					? Compare < 0
					: Islands[A].Representative < Islands[B].Representative;
			});
		}
		else
		{
			// Preserve current vertical order and break ties by representative.
			Ordered = ToStack;
			Ordered.Sort([&Islands](int32 A, int32 B)
			{
				return Islands[A].Box.MinY != Islands[B].Box.MinY
					? Islands[A].Box.MinY < Islands[B].Box.MinY
					: Islands[A].Representative < Islands[B].Representative;
			});
		}

		if (Ordered.Num() == 0)
		{
			OutReport.RailNextDefault = ClaireonGraphIslands::ResolveRailX(Graph);
			OutReport.bRailStable = FMath::IsNearlyEqual(OutReport.RailNextDefault, OutReport.RailX);
			OutReport.Summary = FString::Printf(
				TEXT("Stacked 0 of %d island(s) in '%s': every island was skipped by policy (%d skipped). Nothing was moved. See islands_skipped for the reason on each; include_singletons=true admits one-node islands."),
				OutReport.IslandsTotal, *Graph->GetName(), OutReport.IslandsSkipped.Num());
			return true;
		}

		// Round one integer delta per island to preserve internal offsets.
		// Align primary entries horizontally and pack extents vertically.
		// Skipped islands consume no slots, so the new column can overlap parked content.
		struct FPlan
		{
			int32 IslandIndex = 0;
			int32 DeltaX = 0;
			int32 DeltaY = 0;
			double EntryXBefore = 0.0;
		};

		double CursorY = Islands[Ordered[0]].Box.MinY;
		for (const int32 Index : Ordered)
		{
			// Start from the existing content top.
			CursorY = FMath::Min(CursorY, Islands[Index].Box.MinY);
		}

		TArray<FPlan> Plans;
		Plans.Reserve(Ordered.Num());
		for (const int32 Index : Ordered)
		{
			const FIsland& Island = Islands[Index];
			FPlan& Plan = Plans.AddDefaulted_GetRef();
			Plan.IslandIndex = Index;
			Plan.EntryXBefore = ClaireonGraphIslands::ResolveIslandEntryX(Island);
			Plan.DeltaX = FMath::RoundToInt32(OutReport.RailX - Plan.EntryXBefore);
			Plan.DeltaY = FMath::RoundToInt32(CursorY - Island.Box.MinY);
			CursorY += Island.Box.Height() + Request.Gutter;
		}

		// Classify comments against all islands, including skipped ones.
		// Use both declared membership and anchor geometry so shared comments are never carried with just one island.
		TMap<int32, int32> IslandIndexToPlan;
		for (int32 PlanIndex = 0; PlanIndex < Plans.Num(); ++PlanIndex)
		{
			IslandIndexToPlan.Add(Plans[PlanIndex].IslandIndex, PlanIndex);
		}

		struct FCommentMove
		{
			UEdGraphNode_Comment* Comment = nullptr;
			int32 DeltaX = 0;
			int32 DeltaY = 0;
		};
		TArray<FCommentMove> CommentMoves;

		if (Request.bCarryComments)
		{
			TMap<FGuid, int32> NodeToIsland;
			for (int32 Index = 0; Index < Islands.Num(); ++Index)
			{
				for (const UEdGraphNode* Node : Islands[Index].Nodes)
				{
					if (IsValid(Node))
					{
						NodeToIsland.Add(Node->NodeGuid, Index);
					}
				}
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node);
				if (!IsValid(Comment))
				{
					continue;
				}

				const FIntRect CommentBounds(
					Comment->NodePosX,
					Comment->NodePosY,
					Comment->NodePosX + Comment->NodeWidth,
					Comment->NodePosY + Comment->NodeHeight);

				TSet<int32> Held;
				for (const UObject* Under : Comment->GetNodesUnderComment())
				{
					const UEdGraphNode* UnderNode = Cast<UEdGraphNode>(Under);
					if (IsValid(UnderNode))
					{
						if (const int32* Index = NodeToIsland.Find(UnderNode->NodeGuid))
						{
							Held.Add(*Index);
						}
					}
				}
				for (const UEdGraphNode* Candidate : Graph->Nodes)
				{
					if (!IsValid(Candidate) || Candidate->IsA<UEdGraphNode_Comment>())
					{
						continue;
					}
					if (!CommentBounds.Contains(FIntPoint(Candidate->NodePosX, Candidate->NodePosY)))
					{
						continue;
					}
					if (const int32* Index = NodeToIsland.Find(Candidate->NodeGuid))
					{
						Held.Add(*Index);
					}
				}

				if (Held.Num() == 0)
				{
					FSkippedComment& Row = OutReport.CommentsSkipped.AddDefaulted_GetRef();
					Row.CommentGuid = StackIslands_Guid(Comment->NodeGuid);
					Row.PolicyReason = PolicyNoIslandMember();
					continue;
				}

				TArray<int32> HeldSorted = Held.Array();
				HeldSorted.Sort();

				if (HeldSorted.Num() >= 2)
				{
					// Leave spanning comments in place and report every island they cover.
					FSkippedComment& Row = OutReport.CommentsSkipped.AddDefaulted_GetRef();
					Row.CommentGuid = StackIslands_Guid(Comment->NodeGuid);
					Row.PolicyReason = PolicySpanningComment();
					for (const int32 Index : HeldSorted)
					{
						Row.SpansIslands.Add(Islands[Index].Representative);
					}
					continue;
				}

				const int32 HeldIsland = HeldSorted[0];
				const int32* PlanIndex = IslandIndexToPlan.Find(HeldIsland);
				if (!PlanIndex)
				{
					FSkippedComment& Row = OutReport.CommentsSkipped.AddDefaulted_GetRef();
					Row.CommentGuid = StackIslands_Guid(Comment->NodeGuid);
					Row.PolicyReason = PolicyIslandNotStacked();
					Row.SpansIslands.Add(Islands[HeldIsland].Representative);
					continue;
				}

				FCommentMove& Move = CommentMoves.AddDefaulted_GetRef();
				Move.Comment = Comment;
				Move.DeltaX = Plans[*PlanIndex].DeltaX;
				Move.DeltaY = Plans[*PlanIndex].DeltaY;

				// Carried describes policy even when displacement is zero.
				FCarriedComment& Row = OutReport.CommentsCarried.AddDefaulted_GetRef();
				Row.CommentGuid = StackIslands_Guid(Comment->NodeGuid);
				Row.Island = Islands[HeldIsland].Representative;
			}
		}

		// Measure before/after bounds on the same to-stack members, excluding comments.
		TArray<const FIsland*> StackedIslands;
		StackedIslands.Reserve(Plans.Num());
		for (const FPlan& Plan : Plans)
		{
			StackedIslands.Add(&Islands[Plan.IslandIndex]);
		}
		OutReport.BoundsBefore = StackIslands_Bounds(StackedIslands);

		// Open a transaction only when something moves; an idempotent run must not dirty the graph.
		bool bAnyDisplacement = false;
		for (const FPlan& Plan : Plans)
		{
			bAnyDisplacement |= (Plan.DeltaX != 0 || Plan.DeltaY != 0);
		}
		for (const FCommentMove& Move : CommentMoves)
		{
			bAnyDisplacement |= (Move.DeltaX != 0 || Move.DeltaY != 0);
		}

		if (bAnyDisplacement)
		{
			FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Stack Blueprint Islands")));
			Blueprint->Modify();
			Graph->Modify();

			for (const FPlan& Plan : Plans)
			{
				if (Plan.DeltaX == 0 && Plan.DeltaY == 0)
				{
					continue;
				}
				for (UEdGraphNode* Node : Islands[Plan.IslandIndex].Nodes)
				{
					if (!IsValid(Node))
					{
						continue;
					}
					Node->Modify();
					Node->NodePosX += Plan.DeltaX;
					Node->NodePosY += Plan.DeltaY;
					++OutReport.NodesMoved;
				}
			}

			for (const FCommentMove& Move : CommentMoves)
			{
				if ((Move.DeltaX == 0 && Move.DeltaY == 0) || !IsValid(Move.Comment))
				{
					continue;
				}
				Move.Comment->Modify();
				Move.Comment->NodePosX += Move.DeltaX;
				Move.Comment->NodePosY += Move.DeltaY;
			}

			Graph->NotifyGraphChanged();
		}

		// Read final positions from the graph.
		OutReport.BoundsAfter = StackIslands_Bounds(StackedIslands);

		for (const FPlan& Plan : Plans)
		{
			const FIsland& Island = Islands[Plan.IslandIndex];
			FPlacedIsland& Row = OutReport.IslandsPlaced.AddDefaulted_GetRef();
			Row.Representative = Island.Representative;
			Row.NodeCount = Island.Nodes.Num();
			Row.DeltaX = Plan.DeltaX;
			Row.DeltaY = Plan.DeltaY;
			Row.EntryXBefore = Plan.EntryXBefore;
			Row.EntryXAfter = ClaireonGraphIslands::ResolveIslandEntryX(Island);
		}

		OutReport.RailNextDefault = ClaireonGraphIslands::ResolveRailX(Graph);
		OutReport.bRailStable = FMath::IsNearlyEqual(OutReport.RailNextDefault, OutReport.RailX);

		OutReport.Summary = FString::Printf(
			TEXT("Stacked %d of %d island(s) in '%s' onto x=%g with gutter %g (order=%s): %d node(s) moved, %d comment(s) carried, %d comment(s) left in place, %d island(s) skipped."),
			OutReport.IslandsPlaced.Num(), OutReport.IslandsTotal, *Graph->GetName(),
			OutReport.RailX, OutReport.Gutter, ToWireString(OutReport.Order),
			OutReport.NodesMoved, OutReport.CommentsCarried.Num(),
			OutReport.CommentsSkipped.Num(), OutReport.IslandsSkipped.Num());

		if (OutReport.NodesMoved == 0)
		{
			OutReport.Summary += TEXT(" Every delta was zero -- the graph was already stacked on this rail, so no transaction was opened.");
		}
		if (!OutReport.bRailStable)
		{
			OutReport.Summary += FString::Printf(
				TEXT(" WARNING: rail_x=%g is not what the graph now resolves (%g), so a repeat call with the DEFAULT rail would shift the column; pass rail_x=%g to pin it."),
				OutReport.RailX, OutReport.RailNextDefault, OutReport.RailX);
		}

		return true;
	}

	TSharedPtr<FJsonObject> ToJson(const FStackReport& Report)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();

		Json->SetNumberField(TEXT("rail_x"), Report.RailX);
		Json->SetNumberField(TEXT("gutter"), Report.Gutter);
		Json->SetStringField(TEXT("order"), ToWireString(Report.Order));
		Json->SetBoolField(TEXT("carry_comments"), Report.bCarryComments);
		Json->SetBoolField(TEXT("include_singletons"), Report.bIncludeSingletons);

		Json->SetNumberField(TEXT("islands_total"), Report.IslandsTotal);
		Json->SetNumberField(TEXT("islands_stacked"), Report.IslandsPlaced.Num());
		Json->SetNumberField(TEXT("nodes_moved"), Report.NodesMoved);

		{
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FPlacedIsland& Island : Report.IslandsPlaced)
			{
				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("representative"), Island.Representative);
				Row->SetNumberField(TEXT("node_count"), Island.NodeCount);
				Row->SetNumberField(TEXT("delta_x"), Island.DeltaX);
				Row->SetNumberField(TEXT("delta_y"), Island.DeltaY);
				Row->SetNumberField(TEXT("entry_x_before"), Island.EntryXBefore);
				Row->SetNumberField(TEXT("entry_x_after"), Island.EntryXAfter);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			Json->SetArrayField(TEXT("islands_placed"), Rows);
		}

		{
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FSkippedIsland& Island : Report.IslandsSkipped)
			{
				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("representative"), Island.Representative);
				Row->SetNumberField(TEXT("node_count"), Island.NodeCount);
				Row->SetStringField(TEXT("reason"), Island.Reason);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			Json->SetArrayField(TEXT("islands_skipped"), Rows);
		}

		{
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FCarriedComment& Comment : Report.CommentsCarried)
			{
				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("comment_guid"), Comment.CommentGuid);
				Row->SetStringField(TEXT("island"), Comment.Island);
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			Json->SetArrayField(TEXT("comments_carried"), Rows);
		}

		{
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FSkippedComment& Comment : Report.CommentsSkipped)
			{
				TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("comment_guid"), Comment.CommentGuid);
				Row->SetStringField(TEXT("policy_reason"), Comment.PolicyReason);
				Row->SetArrayField(TEXT("spans_islands"), StackIslands_StringArray(Comment.SpansIslands));
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			Json->SetArrayField(TEXT("comments_skipped"), Rows);
		}

		// Omit bounds for empty content rather than inventing a zero box.
		if (Report.BoundsBefore.bValid)
		{
			Json->SetObjectField(TEXT("bounds_before"), StackIslands_BoundsJson(Report.BoundsBefore));
		}
		if (Report.BoundsAfter.bValid)
		{
			Json->SetObjectField(TEXT("bounds_after"), StackIslands_BoundsJson(Report.BoundsAfter));
		}

		Json->SetBoolField(TEXT("rail_stable"), Report.bRailStable);
		Json->SetNumberField(TEXT("rail_next_default"), Report.RailNextDefault);
		Json->SetStringField(TEXT("summary"), Report.Summary);

		if (!Report.RefusalReason.IsEmpty())
		{
			Json->SetStringField(TEXT("refusal_reason"), Report.RefusalReason);
		}
		return Json;
	}
}


FString ClaireonBlueprintGraphTool_StackIslands::GetOperation() const { return TEXT("stack_islands"); }

TArray<FString> ClaireonBlueprintGraphTool_StackIslands::GetSearchKeywords() const
{
	return {TEXT("bp"), TEXT("stack"), TEXT("island"), TEXT("column"), TEXT("rail"),
	        TEXT("layout"), TEXT("overlap"), TEXT("arrange"), TEXT("align"), TEXT("tidy"),
	        TEXT("graph")};
}

FString ClaireonBlueprintGraphTool_StackIslands::GetDescription() const
{
	return TEXT("Move each island of a Blueprint graph onto one vertical column as a RIGID unit, so "
		"unconnected blocks stop overlapping and entry points read as a column. One delta per island: "
		"intra-island layout is untouched. Singletons are skipped, and a comment spanning two islands is "
		"never moved; both are reported. Needs no editor window. Accepts session_id or asset_path (stateless).");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_StackIslands::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior bp_open (or use asset_path for stateless mode)."), false);
	Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (stateless mode, or alternative to session_id)."), false);
	Builder.AddString(TEXT("graph_name"), TEXT("Graph name (required in stateless mode, where it defaults to EventGraph; with session_id it overrides the session cursor's graph)."), false);
	Builder.AddNumber(TEXT("rail_x"), TEXT("Column x. Omit for the shared rail: the minimum NodePosX among entry nodes, falling back to the leftmost island content. An explicit value is used verbatim, and rail_stable in the response says whether a later default-rail call would agree with it."), false);
	Builder.AddNumber(TEXT("gutter"), TEXT("Vertical gap between successive islands, in graph units. Default 512 (chosen, not measured). Must be >= 0: a negative gutter would overlap the islands this tool exists to separate."), false);
	Builder.AddEnum(TEXT("order"), TEXT("Order islands are laid down the column. by_current_y (default) preserves the vertical reading order; by_entry_name sorts by the island's primary entry-node name, islands without one last; explicit takes island_order. Ties always break by representative GUID, so two runs produce the identical column."),
		{TEXT("by_current_y"), TEXT("by_entry_name"), TEXT("explicit")}, false);
	Builder.AddStringOrStringArray(TEXT("island_order"), TEXT("Island representative GUIDs in the order to stack. REQUIRED with order='explicit' and rejected without it. Must be a permutation of the islands this call would stack: an omission, an unknown GUID, a repeat, or a GUID policy skips is an error naming the offenders, never a silent reorder."), false);
	Builder.AddStringOrStringArray(TEXT("exclude_islands"), TEXT("Island representative GUIDs that must NOT move -- parked clusters are real. An entry that names no island in this graph is an error, not a no-op: a mistyped GUID protecting nothing would stack the cluster it was guarding."), false);
	Builder.AddBoolean(TEXT("include_singletons"), TEXT("Admit one-node islands. Default false: a single disconnected node is a stray, and the parked scratch cluster this tool was written against is four of them. Excluded singletons are still reported in islands_skipped."), false);
	Builder.AddBoolean(TEXT("carry_comments"), TEXT("Carry a comment box with the island it holds, using that island's delta. Default true. A comment holding members of TWO OR MORE islands is never moved and is reported with spans_islands."), false);
	Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'status'). The stacking report is returned in every mode."));
	return Builder.Build();
}

namespace ClaireonStackIslandsInternal
{
	/** Parse shared request defaults for session and stateless paths. */
	bool StackIslands_ParseRequest(
		const TSharedPtr<FJsonObject>& Params,
		ClaireonStackIslands::FStackRequest& OutRequest,
		FString& OutError)
	{
		if (!Params.IsValid())
		{
			return true;
		}

		double Number = 0.0;
		if (Params->TryGetNumberField(TEXT("rail_x"), Number))
		{
			OutRequest.RailX = Number;
			OutRequest.bRailXExplicit = true;
		}
		if (Params->TryGetNumberField(TEXT("gutter"), Number))
		{
			OutRequest.Gutter = Number;
		}

		FString OrderText;
		if (Params->TryGetStringField(TEXT("order"), OrderText) && !OrderText.IsEmpty())
		{
			// Reject unknown order modes rather than falling back.
			if (!ClaireonStackIslands::ParseOrder(OrderText, OutRequest.Order))
			{
				OutError = FString::Printf(
					TEXT("Unknown order '%s'; expected by_current_y, by_entry_name or explicit."), *OrderText);
				return false;
			}
		}

		if (!StackIslands_ReadStringList(Params, TEXT("island_order"), OutRequest.IslandOrder, OutError))
		{
			return false;
		}
		if (!StackIslands_ReadStringList(Params, TEXT("exclude_islands"), OutRequest.ExcludeIslands, OutError))
		{
			return false;
		}

		bool bFlag = false;
		if (Params->TryGetBoolField(TEXT("include_singletons"), bFlag))
		{
			OutRequest.bIncludeSingletons = bFlag;
		}
		if (Params->TryGetBoolField(TEXT("carry_comments"), bFlag))
		{
			OutRequest.bCarryComments = bFlag;
		}
		return true;
	}

	/** Refusal + payload, in bp_format's shape: the reason rides on Data.refusal_reason. */
	FToolResult StackIslands_RefusalResult(const ClaireonStackIslands::FStackReport& Report)
	{
		FToolResult Result = IClaireonTool::MakeErrorResult(Report.Error);
		Result.Data = ClaireonStackIslands::ToJson(Report);
		return Result;
	}

	FToolResult StackIslands_BadArgument(const FString& Message)
	{
		ClaireonStackIslands::FStackReport Report;
		Report.Error = Message;
		Report.Summary = Message;
		Report.RefusalReason = TEXT("bad_argument");
		return StackIslands_RefusalResult(Report);
	}
}

FToolResult ClaireonBlueprintGraphTool_StackIslands::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	using namespace ClaireonStackIslands;

	TSharedPtr<FJsonObject> Params = Arguments.IsValid() ? Arguments : MakeShared<FJsonObject>();
	if (Params->HasField(TEXT("params")))
	{
		const TSharedPtr<FJsonObject>* NestedObj = nullptr;
		if (Params->TryGetObjectField(TEXT("params"), NestedObj) && NestedObj && NestedObj->IsValid())
		{
			Params = *NestedObj;
		}
	}

	// Parse before mode selection so argument errors precede asset or session access.
	FStackRequest Request;
	FString ParseError;
	if (!StackIslands_ParseRequest(Params, Request, ParseError))
	{
		return StackIslands_BadArgument(ParseError);
	}

	FString SessionId;
	if (Params->TryGetStringField(TEXT("session_id"), SessionId) && !SessionId.IsEmpty())
	{
		TSharedPtr<FJsonObject> SessionParams;
		FBlueprintEditToolData* Data = nullptr;
		FToolResult Error;
		if (!BeginSessionOp(Arguments, TEXT("stack_islands"), SessionParams, SessionId, Data, Error))
		{
			return Error;
		}

		UBlueprint* Blueprint = Data->Blueprint.Get();
		UEdGraph* Graph = Data->Graph.Get();
		if (!IsValid(Blueprint) || !IsValid(Graph))
		{
			return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));
		}

		// Unknown graph_name must fail instead of stacking the current graph.
		FString RequestedGraphName;
		if (SessionParams.IsValid()
			&& SessionParams->TryGetStringField(TEXT("graph_name"), RequestedGraphName)
			&& !RequestedGraphName.IsEmpty()
			&& !Graph->GetName().Equals(RequestedGraphName, ESearchCase::IgnoreCase))
		{
			UEdGraph* Found = ClaireonBlueprintHelpers::FindGraphByName(Blueprint, RequestedGraphName);
			if (!IsValid(Found))
			{
				return MakeErrorResult(FString::Printf(
					TEXT("No graph named '%s' on this Blueprint."), *RequestedGraphName));
			}
			Graph = Found;
		}

		FStackReport Report;
		if (!ClaireonStackIslands::Apply(Blueprint, Graph, Request, Report))
		{
			return StackIslands_RefusalResult(Report);
		}

		Data->Cursor.LastOperationStatus = Report.Summary;

		// Leave pin-diff affected nodes empty: rigid movement changes no pins. Report per-island deltas instead.
		FToolResult Result = BuildStateResponse(SessionId, Data);
		if (Result.Data.IsValid())
		{
			// Merge report fields at the same level in both paths.
			const TSharedPtr<FJsonObject> Payload = ClaireonStackIslands::ToJson(Report);
			for (const auto& Pair : Payload->Values)
			{
				Result.Data->SetField(Pair.Key, Pair.Value);
			}
		}
		return Result;
	}

	FString AssetPath, GraphName;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return StackIslands_BadArgument(
			TEXT("Missing required field: asset_path (or session_id for session-based mode)"));
	}
	if (!Params->TryGetStringField(TEXT("graph_name"), GraphName) || GraphName.IsEmpty())
	{
		GraphName = TEXT("EventGraph");
	}

	FString ValidationError;
	if (!ClaireonBlueprintHelpers::ValidateAssetPath(AssetPath, ValidationError))
	{
		return StackIslands_BadArgument(ValidationError);
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!IsValid(Blueprint))
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath));
	}

	UEdGraph* Graph = ClaireonBlueprintHelpers::FindGraphByName(Blueprint, GraphName);
	if (!IsValid(Graph))
	{
		return MakeErrorResult(FString::Printf(TEXT("Graph '%s' not found in %s"), *GraphName, *AssetPath));
	}

	FStackReport Report;
	if (!ClaireonStackIslands::Apply(Blueprint, Graph, Request, Report))
	{
		return StackIslands_RefusalResult(Report);
	}

	return MakeSuccessResult(ClaireonStackIslands::ToJson(Report), Report.Summary);
}

#undef LOCTEXT_NAMESPACE
