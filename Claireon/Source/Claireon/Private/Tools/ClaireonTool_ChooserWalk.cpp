// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ChooserWalk.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/ClaireonChooserGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Chooser.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	int32 CountRowSubChoosers(UChooserTable* Chooser)
	{
		if (!Chooser) { return 0; }
		int32 Fanout = 0;
#if WITH_EDITORONLY_DATA
		for (int32 i = 0; i < Chooser->ResultsStructs.Num(); ++i)
		{
			if (ClaireonChooserGraphHelpers::GetRowSubChooser(Chooser, i)) { ++Fanout; }
		}
#endif
		return Fanout;
	}

	TSharedPtr<FJsonObject> NodeJson(UChooserTable* Chooser, int32 Depth, const FString& ParentPath, int32 ParentRowIndex)
	{
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("path"), Chooser->GetPathName());
		Node->SetStringField(TEXT("name"), Chooser->GetName());
		// outer_path / outer_name disambiguate same-named sub-objects with
		// different parents. UE permits FName collisions across outers, so
		// "Stand Stopped" can exist as a direct child of root AND as a child
		// of "Stand Sprints" — same GetName(), different GetOuter()->GetName().
		if (UObject* Outer = Chooser->GetOuter())
		{
			Node->SetStringField(TEXT("outer_path"), Outer->GetPathName());
			Node->SetStringField(TEXT("outer_name"), Outer->GetName());
		}
		Node->SetStringField(TEXT("parent_path"), ParentPath);
		Node->SetNumberField(TEXT("parent_row_index"), ParentRowIndex);
		Node->SetNumberField(TEXT("depth"), Depth);
#if WITH_EDITORONLY_DATA
		Node->SetNumberField(TEXT("row_count"), Chooser->ResultsStructs.Num());
#else
		Node->SetNumberField(TEXT("row_count"), Chooser->CookedResults.Num());
#endif
		Node->SetNumberField(TEXT("column_count"), Chooser->ColumnsStructs.Num());
		Node->SetStringField(TEXT("result_type"),
			ClaireonChooserHelpers::ResultTypeToString(static_cast<uint8>(Chooser->ResultType)));
		if (Chooser->OutputObjectType)
		{
			Node->SetStringField(TEXT("output_object_type"), Chooser->OutputObjectType->GetName());
		}
		else
		{
			Node->SetStringField(TEXT("output_object_type"), TEXT("None"));
		}
		Node->SetNumberField(TEXT("fanout"), CountRowSubChoosers(Chooser));
		return Node;
	}
}

FString ClaireonTool_ChooserWalk::GetName() const { return TEXT("claireon.chooser_walk"); }

FString ClaireonTool_ChooserWalk::GetDescription() const
{
	return TEXT("Walk a ChooserTable's reachable sub-chooser graph in a single call. Discovery is "
		"via row-result references (FNestedChooser / FEvaluateChooser) — this is the true asset "
		"structure, so depth and parent_path/parent_row_index are meaningful. Each node carries "
		"outer_path/outer_name to disambiguate same-named sub-objects with different parents. "
		"Sub-objects in the asset's flat NestedChoosers registry that are NOT reached via row-result "
		"traversal are emitted in a separate 'orphans' array — these are dead-code sub-choosers. "
		"Use claireon.chooser_traverse for row-by-row inspection of the dispatcher chain.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserWalk::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("root"), TEXT("Root ChooserTable asset path"), true);
	S.AddInteger(TEXT("max_depth"), TEXT("Maximum traversal depth (-1 = unbounded, default -1)"));
	S.AddBoolean(TEXT("include_orphans"), TEXT("Emit orphans array (sub-objects not reached via row-results). Default true."));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserWalk::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString RootPath;
	if (!Arguments->TryGetStringField(TEXT("root"), RootPath) || RootPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: root"));
	}

	int32 MaxDepth = -1;
	double MaxDepthD;
	if (Arguments->TryGetNumberField(TEXT("max_depth"), MaxDepthD))
	{
		MaxDepth = static_cast<int32>(MaxDepthD);
	}

	bool bIncludeOrphans = true;
	Arguments->TryGetBoolField(TEXT("include_orphans"), bIncludeOrphans);

	FString Error;
	UChooserTable* Root = ClaireonChooserHelpers::LoadChooserTableAsset(RootPath, Error);
	if (!Root)
	{
		return MakeErrorResult(Error);
	}

	TArray<TSharedPtr<FJsonValue>> Nodes;
	TSet<FString> ReachablePaths;

	ClaireonChooserGraphHelpers::EnumerateChoosersBFS(Root,
		[&Nodes, &ReachablePaths](UChooserTable* C, int32 Depth, const FString& ParentPath, int32 ParentRowIndex) -> bool
		{
			ReachablePaths.Add(C->GetPathName());
			Nodes.Add(MakeShared<FJsonValueObject>(NodeJson(C, Depth, ParentPath, ParentRowIndex)));
			return true;
		},
		MaxDepth);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("root"), Root->GetPathName());
	Data->SetNumberField(TEXT("total_reachable"), Nodes.Num());
	Data->SetArrayField(TEXT("nodes"), Nodes);

	int32 OrphanCount = 0;
	if (bIncludeOrphans)
	{
		TArray<UChooserTable*> Orphans = ClaireonChooserGraphHelpers::CollectOrphans(Root, ReachablePaths);
		OrphanCount = Orphans.Num();
		TArray<TSharedPtr<FJsonValue>> OrphanArr;
		for (UChooserTable* O : Orphans)
		{
			TSharedPtr<FJsonObject> Obj = NodeJson(O, /*Depth*/ -1, /*ParentPath*/ FString(), /*ParentRowIndex*/ INDEX_NONE);
			OrphanArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
		Data->SetArrayField(TEXT("orphans"), OrphanArr);
		Data->SetNumberField(TEXT("orphan_count"), OrphanCount);
	}

	const FString Summary = FString::Printf(TEXT("Walked '%s': %d reachable, %d orphan%s (max_depth=%d)"),
		*Root->GetName(), Nodes.Num(), OrphanCount, OrphanCount == 1 ? TEXT("") : TEXT("s"), MaxDepth);

	return MakeSuccessResult(Data, Summary);
}
