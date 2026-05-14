// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ChooserTraverse.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/ClaireonChooserGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Chooser.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_ChooserTraverse::GetCategory() const { return TEXT("chooser"); }
FString ClaireonTool_ChooserTraverse::GetOperation() const { return TEXT("traverse"); }

FString ClaireonTool_ChooserTraverse::GetDescription() const
{
	return TEXT("Row-by-row depth-first traversal of a ChooserTable's dispatcher chain. For each "
		"chooser visited (root first, then sub-choosers via row-result references in row order), "
		"emits one entry per row with {chooser_path, row_index, depth, parent_chooser_path, "
		"parent_row_index, disabled, result, follows_to, [column_values]}. When a row's result is "
		"a sub-chooser ref, the next entries in the output are that sub-chooser's rows — so the "
		"agent can read the dispatcher chain top-to-bottom without N inspect calls. "
		"mode='compact' (default) omits column_values; 'full' includes them.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserTraverse::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("root"), TEXT("Root ChooserTable asset path"), true);
	S.AddInteger(TEXT("max_depth"), TEXT("Maximum recursion depth (-1 = unbounded, default -1)"));
	S.AddEnum(TEXT("mode"),
		TEXT("'compact' (default; result + follows_to per row) or 'full' (also include column_values)"),
		{TEXT("compact"), TEXT("full")});
	S.AddBoolean(TEXT("include_disabled"),
		TEXT("Emit disabled rows (default true). When false, disabled rows are skipped AND the traversal does not descend through them."));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserTraverse::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString Mode = TEXT("compact");
	Arguments->TryGetStringField(TEXT("mode"), Mode);
	const bool bFull = (Mode == TEXT("full"));

	bool bIncludeDisabled = true;
	Arguments->TryGetBoolField(TEXT("include_disabled"), bIncludeDisabled);

	FString Error;
	UChooserTable* Root = ClaireonChooserHelpers::LoadChooserTableAsset(RootPath, Error);
	if (!Root)
	{
		return MakeErrorResult(Error);
	}

	TArray<TSharedPtr<FJsonValue>> RowEntries;
	int32 ColumnCountTotal = 0;

	auto Visit = [&](UChooserTable* Cur, int32 RowIndex, int32 Depth,
		const FString& ParentPath, int32 ParentRowIndex) -> bool
	{
		if (!Cur) { return true; }
#if WITH_EDITORONLY_DATA
		const bool bDisabled = Cur->IsRowDisabled(RowIndex);
		if (!bIncludeDisabled && bDisabled) { return true; }

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("chooser_path"), Cur->GetPathName());
		Entry->SetStringField(TEXT("chooser_name"), Cur->GetName());
		Entry->SetNumberField(TEXT("row_index"), RowIndex);
		Entry->SetNumberField(TEXT("depth"), Depth);
		if (!ParentPath.IsEmpty())
		{
			Entry->SetStringField(TEXT("parent_chooser_path"), ParentPath);
			Entry->SetNumberField(TEXT("parent_row_index"), ParentRowIndex);
		}
		Entry->SetBoolField(TEXT("disabled"), bDisabled);

		if (Cur->ResultsStructs.IsValidIndex(RowIndex))
		{
			Entry->SetObjectField(TEXT("result"),
				ClaireonChooserHelpers::SerializeRowResult(Cur->ResultsStructs[RowIndex]));
		}

		// follows_to: when this row's result targets a sub-chooser, expose
		// the target path inline so a reader can pre-read the connection
		// without parsing the result struct.
		if (UChooserTable* Target = ClaireonChooserGraphHelpers::GetRowSubChooser(Cur, RowIndex))
		{
			TSharedPtr<FJsonObject> Follow = MakeShared<FJsonObject>();
			Follow->SetStringField(TEXT("path"), Target->GetPathName());
			Follow->SetStringField(TEXT("name"), Target->GetName());
			Follow->SetNumberField(TEXT("row_count"), Target->ResultsStructs.Num());
			Entry->SetObjectField(TEXT("follows_to"), Follow);
		}

		if (bFull)
		{
			TSharedPtr<FJsonObject> ColValues = MakeShared<FJsonObject>();
			const int32 ColumnCount = Cur->ColumnsStructs.Num();
			for (int32 c = 0; c < ColumnCount; ++c)
			{
				ColValues->SetField(FString::FromInt(c),
					ClaireonChooserHelpers::SerializeColumnCellValue(Cur->ColumnsStructs[c], RowIndex));
			}
			Entry->SetObjectField(TEXT("column_values"), ColValues);
		}

		RowEntries.Add(MakeShared<FJsonValueObject>(Entry));
		ColumnCountTotal = FMath::Max(ColumnCountTotal, Cur->ColumnsStructs.Num());
#endif
		return true;
	};

	ClaireonChooserGraphHelpers::TraverseRowsDFS(Root, Visit, MaxDepth);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("root"), Root->GetPathName());
	Data->SetStringField(TEXT("mode"), Mode);
	Data->SetNumberField(TEXT("total_rows"), RowEntries.Num());
	Data->SetArrayField(TEXT("rows"), RowEntries);

	const FString Summary = FString::Printf(TEXT("Traversed '%s': %d row(s) (mode=%s, max_depth=%d)"),
		*Root->GetName(), RowEntries.Num(), *Mode, MaxDepth);
	return MakeSuccessResult(Data, Summary);
}
