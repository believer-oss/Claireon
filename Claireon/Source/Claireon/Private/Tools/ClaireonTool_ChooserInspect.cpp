// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ChooserInspect.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "Chooser.h"
#include "ChooserPropertyAccess.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	bool ShouldEmit(const TSet<FString>& Selected, const TCHAR* Field)
	{
		// Empty selection means emit everything (back-compat). Selection accepts
		// both bare branch names ("rows") and dotted sub-paths ("rows.result");
		// either form opts the branch in. Matching is by prefix on dotted paths
		// so callers can ask for just one column-of-row without the parent name.
		if (Selected.Num() == 0) { return true; }
		if (Selected.Contains(Field)) { return true; }
		const FString Prefix = FString(Field) + TEXT(".");
		for (const FString& S : Selected)
		{
			if (S.StartsWith(Prefix)) { return true; }
		}
		return false;
	}

	int32 CountReachableSubChoosers(const UChooserTable* Chooser)
	{
		if (!Chooser) { return 0; }
		int32 Fanout = 0;
#if WITH_EDITORONLY_DATA
		for (const FInstancedStruct& Result : Chooser->ResultsStructs)
		{
			if (!Result.IsValid()) { continue; }
			if (Result.GetPtr<FNestedChooser>() || Result.GetPtr<FEvaluateChooser>())
			{
				++Fanout;
			}
		}
#endif
		return Fanout;
	}

	// Per-nested-chooser triage payload used by include_subchoosers=="refs".
	// Cheap to produce — no row bodies, no per-cell column values.
	TSharedPtr<FJsonObject> SerializeSubChooserRef(const UChooserTable* Nested)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!Nested) { return Out; }
		Out->SetStringField(TEXT("name"), Nested->GetName());
		Out->SetStringField(TEXT("path"), Nested->GetPathName());
#if WITH_EDITORONLY_DATA
		Out->SetNumberField(TEXT("row_count"), Nested->ResultsStructs.Num());
#else
		Out->SetNumberField(TEXT("row_count"), Nested->CookedResults.Num());
#endif
		Out->SetNumberField(TEXT("column_count"), Nested->ColumnsStructs.Num());
		Out->SetStringField(TEXT("result_type"),
			ClaireonChooserHelpers::ResultTypeToString(static_cast<uint8>(Nested->ResultType)));
		if (Nested->OutputObjectType)
		{
			Out->SetStringField(TEXT("output_object_type"), Nested->OutputObjectType->GetName());
		}
		Out->SetNumberField(TEXT("fanout"), CountReachableSubChoosers(Nested));
		return Out;
	}
}

FString ClaireonTool_ChooserInspect::GetName() const { return TEXT("claireon.chooser_inspect"); }

TArray<FString> ClaireonTool_ChooserInspect::GetSearchKeywords() const
{
	return {TEXT("chooser"), TEXT("inspect"), TEXT("table"), TEXT("result"), TEXT("row"), TEXT("column"), TEXT("context")};
}

FString ClaireonTool_ChooserInspect::GetDescription() const
{
	return TEXT("Inspect a ChooserTable asset. Returns result type, output class, context parameters, "
		"columns, rows (results + cell values), and the fallback result. Supports field projection "
		"(fields=[...]) and row paging (row_offset/row_limit) to keep payloads small. Sub-chooser "
		"handling is controlled by include_subchoosers: 'none' omits, 'refs' (default) returns "
		"compact per-nested triage info (name/path/row_count/etc.) without recursing into bodies, "
		"'recursive' inlines the full nested tree. Use claireon.chooser_walk for tree traversal "
		"instead of include_subchoosers='recursive' on deep trees.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserInspect::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddEnum(TEXT("detail_level"), TEXT("Verbosity: 'summary' (counts only) or 'full' (default; rows/columns)"),
		{TEXT("summary"), TEXT("full")});
	S.AddInteger(TEXT("row_index"), TEXT("Focus on a single row index (omit to show paged rows). Takes precedence over row_offset/row_limit."));
	S.AddInteger(TEXT("row_offset"), TEXT("First row to include (default 0). Only meaningful when row_index is omitted."));
	S.AddInteger(TEXT("row_limit"), TEXT("Maximum number of rows to include (default unlimited). Only meaningful when row_index is omitted."));
	S.AddEnum(TEXT("include_subchoosers"),
		TEXT("Sub-chooser handling: 'none' (omit), 'refs' (default; name/path/counts only), 'recursive' (inline full sub-chooser bodies)"),
		{TEXT("none"), TEXT("refs"), TEXT("recursive")});
	// fields: array of branch names; absent or empty == emit all (back-compat).
	// Recognised: parameters, columns, rows, nested_choosers, fallback_result.
	// Schema builder doesn't have an array helper, so document via description.
	S.AddString(TEXT("fields"),
		TEXT("Comma-separated branches to include. Recognised: parameters, columns, rows, nested_choosers, fallback_result. Absent/empty = emit all (back-compat)."));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString Error;
	UChooserTable* Chooser = ClaireonChooserHelpers::LoadChooserTableAsset(AssetPath, Error);
	if (!Chooser)
	{
		return MakeErrorResult(Error);
	}

	FString DetailLevel = TEXT("full");
	Arguments->TryGetStringField(TEXT("detail_level"), DetailLevel);

	int32 FocusRowIndex = -1;
	double RowIdxDouble;
	if (Arguments->TryGetNumberField(TEXT("row_index"), RowIdxDouble))
	{
		FocusRowIndex = static_cast<int32>(RowIdxDouble);
	}

	int32 RowOffset = 0;
	double Offs;
	if (Arguments->TryGetNumberField(TEXT("row_offset"), Offs))
	{
		RowOffset = FMath::Max(0, static_cast<int32>(Offs));
	}

	int32 RowLimit = INT32_MAX;
	double Lim;
	if (Arguments->TryGetNumberField(TEXT("row_limit"), Lim))
	{
		RowLimit = FMath::Max(0, static_cast<int32>(Lim));
	}

	FString IncludeSubChoosers = TEXT("refs");
	Arguments->TryGetStringField(TEXT("include_subchoosers"), IncludeSubChoosers);
	const bool bSubNone = IncludeSubChoosers == TEXT("none");
	const bool bSubRefs = IncludeSubChoosers == TEXT("refs");
	const bool bSubRecursive = IncludeSubChoosers == TEXT("recursive");

	// Field projection. Empty set = emit all (back-compat).
	TSet<FString> SelectedFields;
	FString FieldsStr;
	if (Arguments->TryGetStringField(TEXT("fields"), FieldsStr) && !FieldsStr.IsEmpty())
	{
		TArray<FString> Parts;
		FieldsStr.ParseIntoArray(Parts, TEXT(","), true);
		for (FString& P : Parts)
		{
			P.TrimStartAndEndInline();
			if (!P.IsEmpty()) { SelectedFields.Add(P); }
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
	Data->SetStringField(TEXT("asset_name"), Chooser->GetName());
	Data->SetStringField(TEXT("result_type"), ClaireonChooserHelpers::ResultTypeToString(static_cast<uint8>(Chooser->ResultType)));
	if (Chooser->OutputObjectType)
	{
		Data->SetStringField(TEXT("output_object_type"), Chooser->OutputObjectType->GetName());
		Data->SetStringField(TEXT("output_object_type_path"), Chooser->OutputObjectType->GetPathName());
	}
	else
	{
		Data->SetStringField(TEXT("output_object_type"), TEXT("None"));
	}

	const UChooserTable* RootChooser = Chooser->GetRootChooser();
	const TArray<FInstancedStruct>& ContextData = RootChooser->ContextData;

	if (ShouldEmit(SelectedFields, TEXT("parameters")))
	{
		Data->SetArrayField(TEXT("parameters"), ClaireonChooserHelpers::SerializeContextData(ContextData));
	}

	const int32 ColumnCount = Chooser->ColumnsStructs.Num();
	Data->SetNumberField(TEXT("column_count"), ColumnCount);

#if WITH_EDITORONLY_DATA
	const int32 RowCount = Chooser->ResultsStructs.Num();
#else
	const int32 RowCount = Chooser->CookedResults.Num();
#endif
	Data->SetNumberField(TEXT("row_count"), RowCount);

	if (DetailLevel != TEXT("summary"))
	{
		// Columns
		if (ShouldEmit(SelectedFields, TEXT("columns")))
		{
			TArray<TSharedPtr<FJsonValue>> ColumnsArray;
			for (int32 i = 0; i < ColumnCount; ++i)
			{
				TSharedPtr<FJsonObject> ColObj = ClaireonChooserHelpers::SerializeColumn(Chooser->ColumnsStructs[i], i, &ContextData);
				ColumnsArray.Add(MakeShared<FJsonValueObject>(ColObj));
			}
			Data->SetArrayField(TEXT("columns"), ColumnsArray);
		}

		// Rows (paged or focused)
		if (ShouldEmit(SelectedFields, TEXT("rows")))
		{
#if WITH_EDITORONLY_DATA
			int32 StartRow;
			int32 EndRow;
			if (FocusRowIndex >= 0)
			{
				StartRow = FMath::Clamp(FocusRowIndex, 0, RowCount);
				EndRow = FMath::Min(FocusRowIndex + 1, RowCount);
			}
			else
			{
				StartRow = FMath::Min(RowOffset, RowCount);
				const int64 EndCandidate = static_cast<int64>(StartRow) + static_cast<int64>(RowLimit);
				EndRow = static_cast<int32>(FMath::Min<int64>(EndCandidate, RowCount));
			}

			TArray<TSharedPtr<FJsonValue>> RowsArray;
			for (int32 i = StartRow; i < EndRow; ++i)
			{
				TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
				RowObj->SetNumberField(TEXT("index"), i);
				RowObj->SetBoolField(TEXT("disabled"), Chooser->IsRowDisabled(i));

				if (Chooser->ResultsStructs.IsValidIndex(i))
				{
					RowObj->SetObjectField(TEXT("result"),
						ClaireonChooserHelpers::SerializeRowResult(Chooser->ResultsStructs[i]));
				}

				TSharedPtr<FJsonObject> ColValuesObj = MakeShared<FJsonObject>();
				for (int32 c = 0; c < ColumnCount; ++c)
				{
					TSharedPtr<FJsonValue> CellValue = ClaireonChooserHelpers::SerializeColumnCellValue(Chooser->ColumnsStructs[c], i);
					ColValuesObj->SetField(FString::FromInt(c), CellValue);
				}
				RowObj->SetObjectField(TEXT("column_values"), ColValuesObj);

				RowsArray.Add(MakeShared<FJsonValueObject>(RowObj));
			}
			Data->SetArrayField(TEXT("rows"), RowsArray);

			TSharedPtr<FJsonObject> Paging = MakeShared<FJsonObject>();
			Paging->SetNumberField(TEXT("offset"), StartRow);
			Paging->SetNumberField(TEXT("returned"), EndRow - StartRow);
			Paging->SetNumberField(TEXT("total"), RowCount);
			Paging->SetBoolField(TEXT("focused"), FocusRowIndex >= 0);
			Data->SetObjectField(TEXT("_paging"), Paging);
#endif
		}

		// Fallback result
		if (ShouldEmit(SelectedFields, TEXT("fallback_result")) && Chooser->FallbackResult.IsValid())
		{
			Data->SetObjectField(TEXT("fallback_result"),
				ClaireonChooserHelpers::SerializeRowResult(Chooser->FallbackResult));
		}

		// Sub-choosers
		if (!bSubNone && ShouldEmit(SelectedFields, TEXT("nested_choosers")))
		{
#if WITH_EDITORONLY_DATA
			if (Chooser->NestedChoosers.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> NestedArray;

				if (bSubRefs || (!bSubRecursive))
				{
					// Default: refs only — name/path/counts/result_type/fanout per nested.
					// Use claireon.chooser_walk for tree traversal; targeted
					// claireon.chooser_inspect on a specific nested for full body.
					for (const auto& Nested : Chooser->NestedChoosers)
					{
						if (!Nested) { continue; }
						NestedArray.Add(MakeShared<FJsonValueObject>(SerializeSubChooserRef(Nested)));
					}
				}
				else
				{
					// "recursive" — preserves prior behaviour (full inline of nested rows + columns + results).
					for (const auto& Nested : Chooser->NestedChoosers)
					{
						if (!Nested) { continue; }
						TSharedPtr<FJsonObject> NestedObj = MakeShared<FJsonObject>();
						NestedObj->SetStringField(TEXT("name"), Nested->GetName());
						NestedObj->SetStringField(TEXT("path"), Nested->GetPathName());

						const int32 NestedColCount = Nested->ColumnsStructs.Num();
						const int32 NestedRowCount = Nested->ResultsStructs.Num();
						NestedObj->SetNumberField(TEXT("column_count"), NestedColCount);
						NestedObj->SetNumberField(TEXT("row_count"), NestedRowCount);

						TArray<TSharedPtr<FJsonValue>> NestedCols;
						for (int32 nc = 0; nc < NestedColCount; ++nc)
						{
							TSharedPtr<FJsonObject> NColObj = ClaireonChooserHelpers::SerializeColumn(Nested->ColumnsStructs[nc], nc, &ContextData);
							NestedCols.Add(MakeShared<FJsonValueObject>(NColObj));
						}
						NestedObj->SetArrayField(TEXT("columns"), NestedCols);

						TArray<TSharedPtr<FJsonValue>> NestedRows;
						for (int32 nr = 0; nr < NestedRowCount; ++nr)
						{
							TSharedPtr<FJsonObject> NRowObj = MakeShared<FJsonObject>();
							NRowObj->SetNumberField(TEXT("index"), nr);
							NRowObj->SetBoolField(TEXT("disabled"), Nested->IsRowDisabled(nr));

							if (Nested->ResultsStructs.IsValidIndex(nr))
							{
								NRowObj->SetObjectField(TEXT("result"),
									ClaireonChooserHelpers::SerializeRowResult(Nested->ResultsStructs[nr]));
							}

							TSharedPtr<FJsonObject> NColVals = MakeShared<FJsonObject>();
							for (int32 nc = 0; nc < NestedColCount; ++nc)
							{
								TSharedPtr<FJsonValue> NCellVal = ClaireonChooserHelpers::SerializeColumnCellValue(Nested->ColumnsStructs[nc], nr);
								NColVals->SetField(FString::FromInt(nc), NCellVal);
							}
							NRowObj->SetObjectField(TEXT("column_values"), NColVals);

							NestedRows.Add(MakeShared<FJsonValueObject>(NRowObj));
						}
						NestedObj->SetArrayField(TEXT("rows"), NestedRows);

						NestedArray.Add(MakeShared<FJsonValueObject>(NestedObj));
					}
				}

				Data->SetArrayField(TEXT("nested_choosers"), NestedArray);
			}
#endif
		}
	}
	else if (!bSubNone && ShouldEmit(SelectedFields, TEXT("nested_choosers")))
	{
#if WITH_EDITORONLY_DATA
		// Summary mode: just nested chooser short names.
		if (Chooser->NestedChoosers.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> NestedNames;
			for (const auto& Nested : Chooser->NestedChoosers)
			{
				if (Nested) { NestedNames.Add(MakeShared<FJsonValueString>(Nested->GetName())); }
			}
			Data->SetArrayField(TEXT("nested_choosers"), NestedNames);
		}
#endif
	}

	if (Chooser->RootChooser && Chooser->RootChooser != Chooser)
	{
		Data->SetStringField(TEXT("root_chooser"), Chooser->RootChooser->GetPathName());
	}

	const FString Summary = FString::Printf(TEXT("ChooserTable '%s': %d rows, %d columns, result_type=%s, sub_choosers=%s"),
		*Chooser->GetName(), RowCount, ColumnCount,
		*ClaireonChooserHelpers::ResultTypeToString(static_cast<uint8>(Chooser->ResultType)),
		*IncludeSubChoosers);

	return MakeSuccessResult(Data, Summary);
}
