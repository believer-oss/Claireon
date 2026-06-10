// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonChooserGraphHelpers.h"
#include "Chooser.h"
#include "StructUtils/InstancedStruct.h"

namespace ClaireonChooserGraphHelpers
{

UChooserTable* GetRowSubChooser(UChooserTable* Chooser, int32 RowIndex)
{
	if (!Chooser) { return nullptr; }
#if WITH_EDITORONLY_DATA
	if (!Chooser->ResultsStructs.IsValidIndex(RowIndex)) { return nullptr; }
	const FInstancedStruct& Result = Chooser->ResultsStructs[RowIndex];
	if (!Result.IsValid()) { return nullptr; }
	if (const FNestedChooser* NC = Result.GetPtr<FNestedChooser>())
	{
		return NC->Chooser;
	}
	if (const FEvaluateChooser* EC = Result.GetPtr<FEvaluateChooser>())
	{
		return EC->Chooser;
	}
#endif
	return nullptr;
}

void EnumerateChoosersBFS(UChooserTable* Root, FNodeVisitor Visitor, int32 MaxDepth)
{
	if (!Root || !Visitor) { return; }

	struct FQueueEntry
	{
		UChooserTable* Chooser;
		int32 Depth;
		FString ParentPath;
		int32 ParentRowIndex;
	};

	TArray<FQueueEntry> Queue;
	TSet<FString> Visited;
	Queue.Add({ Root, 0, FString(), INDEX_NONE });

	int32 Head = 0;
	while (Head < Queue.Num())
	{
		FQueueEntry Entry = Queue[Head++];
		if (!Entry.Chooser) { continue; }

		const FString ThisPath = Entry.Chooser->GetPathName();
		if (Visited.Contains(ThisPath)) { continue; }
		Visited.Add(ThisPath);

		if (MaxDepth >= 0 && Entry.Depth > MaxDepth) { continue; }

		if (!Visitor(Entry.Chooser, Entry.Depth, Entry.ParentPath, Entry.ParentRowIndex))
		{
			return;
		}

#if WITH_EDITORONLY_DATA
		const int32 RowCount = Entry.Chooser->ResultsStructs.Num();
		for (int32 i = 0; i < RowCount; ++i)
		{
			UChooserTable* Child = GetRowSubChooser(Entry.Chooser, i);
			if (Child && !Visited.Contains(Child->GetPathName()))
			{
				Queue.Add({ Child, Entry.Depth + 1, ThisPath, i });
			}
		}
#endif
	}
}

void TraverseRowsDFS(UChooserTable* Root, FRowVisitor Visitor, int32 MaxDepth)
{
	if (!Root || !Visitor) { return; }

	// DFS via explicit stack so the per-row recursion stays bounded by asset
	// shape, not C++ stack depth. Path-based visited set guards against the
	// (pathological but possible) self-referential dispatcher.
	struct FFrame
	{
		UChooserTable* Chooser;
		int32 Depth;
		FString ParentPath;
		int32 ParentRowIndex;
	};

	TArray<FFrame> Stack;
	TSet<FString> VisitedChoosers;
	Stack.Add({ Root, 0, FString(), INDEX_NONE });

	while (Stack.Num() > 0)
	{
		FFrame Frame = Stack.Pop(EAllowShrinking::No);
		if (!Frame.Chooser) { continue; }

		const FString ThisPath = Frame.Chooser->GetPathName();
		if (VisitedChoosers.Contains(ThisPath)) { continue; }
		VisitedChoosers.Add(ThisPath);

		if (MaxDepth >= 0 && Frame.Depth > MaxDepth) { continue; }

#if WITH_EDITORONLY_DATA
		const int32 RowCount = Frame.Chooser->ResultsStructs.Num();
		for (int32 i = 0; i < RowCount; ++i)
		{
			if (!Visitor(Frame.Chooser, i, Frame.Depth, Frame.ParentPath, Frame.ParentRowIndex))
			{
				return;
			}
		}

		// Push children in reverse order so the leftmost (lowest-index) row's
		// sub-chooser is visited next on the next pop. Combined with row-by-row
		// emission above, this gives the agent a depth-first reading order:
		// row 0 of root -> row 0 of root's row-0 sub-chooser -> ... -> row 1 of root.
		// Note: the sub-chooser's rows are emitted on the next loop iteration,
		// so dispatcher rows are followed by their target's rows in the output.
		for (int32 i = RowCount - 1; i >= 0; --i)
		{
			UChooserTable* Child = GetRowSubChooser(Frame.Chooser, i);
			if (Child && !VisitedChoosers.Contains(Child->GetPathName()))
			{
				Stack.Add({ Child, Frame.Depth + 1, ThisPath, i });
			}
		}
#endif
	}
}

TArray<UChooserTable*> CollectOrphans(UChooserTable* Root, const TSet<FString>& ReachablePaths)
{
	TArray<UChooserTable*> Orphans;
	if (!Root) { return Orphans; }

#if WITH_EDITORONLY_DATA
	// NestedChoosers is the asset's flat editor-only registry of all
	// sub-objects regardless of nesting depth. Anything in there that wasn't
	// reached via row-result traversal is dead code.
	for (const auto& Nested : Root->NestedChoosers)
	{
		if (!Nested) { continue; }
		const FString NestedPath = Nested->GetPathName();
		if (!ReachablePaths.Contains(NestedPath))
		{
			Orphans.Add(Nested);
		}
	}
#endif
	return Orphans;
}

} // namespace ClaireonChooserGraphHelpers
