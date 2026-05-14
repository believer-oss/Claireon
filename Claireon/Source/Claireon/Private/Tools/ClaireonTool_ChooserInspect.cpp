// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ChooserInspect.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "Chooser.h"
#include "ChooserPropertyAccess.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_ChooserInspect::GetName() const { return TEXT("claireon.chooser_inspect"); }

FString ClaireonTool_ChooserInspect::GetDescription() const
{
	return TEXT("Inspect a ChooserTable asset. Returns the full structure: result type, output class, "
		"context parameters (input/output structs), all columns with their types and bindings, "
		"all rows with their results and column cell values, and the fallback result.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserInspect::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddEnum(TEXT("detail_level"), TEXT("Verbosity: 'summary' (counts only) or 'full' (all rows/columns)"),
		{TEXT("summary"), TEXT("full")});
	S.AddInteger(TEXT("row_index"), TEXT("Focus on a specific row index (omit to show all rows)"));
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

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
	Data->SetStringField(TEXT("asset_name"), Chooser->GetName());

	// Result type and output class
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

	// Context data (parameters)
	const UChooserTable* RootChooser = Chooser->GetRootChooser();
	const TArray<FInstancedStruct>& ContextData = RootChooser->ContextData;
	TArray<TSharedPtr<FJsonValue>> ParamsArray = ClaireonChooserHelpers::SerializeContextData(ContextData);
	Data->SetArrayField(TEXT("parameters"), ParamsArray);

	// Columns
	int32 ColumnCount = Chooser->ColumnsStructs.Num();
	Data->SetNumberField(TEXT("column_count"), ColumnCount);

#if WITH_EDITORONLY_DATA
	int32 RowCount = Chooser->ResultsStructs.Num();
#else
	int32 RowCount = Chooser->CookedResults.Num();
#endif
	Data->SetNumberField(TEXT("row_count"), RowCount);

	if (DetailLevel != TEXT("summary"))
	{
		// Serialize columns with binding resolution
		TArray<TSharedPtr<FJsonValue>> ColumnsArray;
		for (int32 i = 0; i < ColumnCount; ++i)
		{
			TSharedPtr<FJsonObject> ColObj = ClaireonChooserHelpers::SerializeColumn(Chooser->ColumnsStructs[i], i, &ContextData);
			ColumnsArray.Add(MakeShared<FJsonValueObject>(ColObj));
		}
		Data->SetArrayField(TEXT("columns"), ColumnsArray);

		// Serialize rows
		TArray<TSharedPtr<FJsonValue>> RowsArray;

#if WITH_EDITORONLY_DATA
		int32 StartRow = (FocusRowIndex >= 0) ? FocusRowIndex : 0;
		int32 EndRow = (FocusRowIndex >= 0) ? FMath::Min(FocusRowIndex + 1, RowCount) : RowCount;

		for (int32 i = StartRow; i < EndRow; ++i)
		{
			TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
			RowObj->SetNumberField(TEXT("index"), i);

			// Disabled state
			bool bDisabled = Chooser->IsRowDisabled(i);
			RowObj->SetBoolField(TEXT("disabled"), bDisabled);

			// Result
			if (Chooser->ResultsStructs.IsValidIndex(i))
			{
				TSharedPtr<FJsonObject> ResultObj = ClaireonChooserHelpers::SerializeRowResult(Chooser->ResultsStructs[i]);
				RowObj->SetObjectField(TEXT("result"), ResultObj);
			}

			// Column values for this row
			TSharedPtr<FJsonObject> ColValuesObj = MakeShared<FJsonObject>();
			for (int32 c = 0; c < ColumnCount; ++c)
			{
				TSharedPtr<FJsonValue> CellValue = ClaireonChooserHelpers::SerializeColumnCellValue(Chooser->ColumnsStructs[c], i);
				ColValuesObj->SetField(FString::FromInt(c), CellValue);
			}
			RowObj->SetObjectField(TEXT("column_values"), ColValuesObj);

			RowsArray.Add(MakeShared<FJsonValueObject>(RowObj));
		}
#endif

		Data->SetArrayField(TEXT("rows"), RowsArray);

		// Fallback result
		if (Chooser->FallbackResult.IsValid())
		{
			TSharedPtr<FJsonObject> FallbackObj = ClaireonChooserHelpers::SerializeRowResult(Chooser->FallbackResult);
			Data->SetObjectField(TEXT("fallback_result"), FallbackObj);
		}

#if WITH_EDITORONLY_DATA
		// Recursively serialize nested choosers (full content, not just names)
		if (Chooser->NestedChoosers.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> NestedArray;
			for (const auto& Nested : Chooser->NestedChoosers)
			{
				if (!Nested)
				{
					continue;
				}
				TSharedPtr<FJsonObject> NestedObj = MakeShared<FJsonObject>();
				NestedObj->SetStringField(TEXT("name"), Nested->GetName());
				// Full object path — unique per subobject, required for
				// disambiguating nested choosers that share a short name
				// across asset families. Consumers look this up against
				// the NestedChooser result's `chooser_path` field.
				NestedObj->SetStringField(TEXT("path"), Nested->GetPathName());

				int32 NestedColCount = Nested->ColumnsStructs.Num();
				int32 NestedRowCount = Nested->ResultsStructs.Num();
				NestedObj->SetNumberField(TEXT("column_count"), NestedColCount);
				NestedObj->SetNumberField(TEXT("row_count"), NestedRowCount);

				// Nested chooser columns
				TArray<TSharedPtr<FJsonValue>> NestedCols;
				for (int32 nc = 0; nc < NestedColCount; ++nc)
				{
					TSharedPtr<FJsonObject> NColObj = ClaireonChooserHelpers::SerializeColumn(Nested->ColumnsStructs[nc], nc, &ContextData);
					NestedCols.Add(MakeShared<FJsonValueObject>(NColObj));
				}
				NestedObj->SetArrayField(TEXT("columns"), NestedCols);

				// Nested chooser rows
				TArray<TSharedPtr<FJsonValue>> NestedRows;
				for (int32 nr = 0; nr < NestedRowCount; ++nr)
				{
					TSharedPtr<FJsonObject> NRowObj = MakeShared<FJsonObject>();
					NRowObj->SetNumberField(TEXT("index"), nr);
					NRowObj->SetBoolField(TEXT("disabled"), Nested->IsRowDisabled(nr));

					if (Nested->ResultsStructs.IsValidIndex(nr))
					{
						TSharedPtr<FJsonObject> NResultObj = ClaireonChooserHelpers::SerializeRowResult(Nested->ResultsStructs[nr]);
						NRowObj->SetObjectField(TEXT("result"), NResultObj);
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
			Data->SetArrayField(TEXT("nested_choosers"), NestedArray);
		}
#endif
	}
	else
	{
#if WITH_EDITORONLY_DATA
		// Summary mode: just list nested chooser names
		if (Chooser->NestedChoosers.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> NestedNames;
			for (const auto& Nested : Chooser->NestedChoosers)
			{
				if (Nested)
				{
					NestedNames.Add(MakeShared<FJsonValueString>(Nested->GetName()));
				}
			}
			Data->SetArrayField(TEXT("nested_choosers"), NestedNames);
		}
#endif
	}

	// Root chooser info (if this is a nested chooser)
	if (Chooser->RootChooser && Chooser->RootChooser != Chooser)
	{
		Data->SetStringField(TEXT("root_chooser"), Chooser->RootChooser->GetPathName());
	}

	FString Summary = FString::Printf(TEXT("ChooserTable '%s': %d rows, %d columns, result_type=%s"),
		*Chooser->GetName(), RowCount, ColumnCount,
		*ClaireonChooserHelpers::ResultTypeToString(static_cast<uint8>(Chooser->ResultType)));

	return MakeSuccessResult(Data, Summary);
}
