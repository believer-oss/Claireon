// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonChooserTool_RemoveRow.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Chooser.h"
#include "IChooserColumn.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "StructUtils/InstancedStruct.h"

FString ClaireonTool_ChooserRemoveRow::GetCategory() const { return TEXT("chooser"); }
FString ClaireonTool_ChooserRemoveRow::GetOperation() const { return TEXT("remove_row"); }

FString ClaireonTool_ChooserRemoveRow::GetDescription() const
{
	return TEXT("Remove a row from a ChooserTable by index.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserRemoveRow::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddInteger(TEXT("row_index"), TEXT("Index of the row to remove"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserRemoveRow::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	double RowIdxDouble;
	if (!Arguments->TryGetNumberField(TEXT("row_index"), RowIdxDouble))
	{
		return MakeErrorResult(TEXT("Missing required parameter: row_index"));
	}
	int32 RowIndex = static_cast<int32>(RowIdxDouble);

	FString Error;
	UChooserTable* Chooser = ClaireonChooserHelpers::LoadChooserTableAsset(AssetPath, Error);
	if (!Chooser)
	{
		return MakeErrorResult(Error);
	}

#if WITH_EDITORONLY_DATA
	if (!Chooser->ResultsStructs.IsValidIndex(RowIndex))
	{
		return MakeErrorResult(FString::Printf(TEXT("Row index %d out of bounds (row count: %d)"),
			RowIndex, Chooser->ResultsStructs.Num()));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove ChooserTable Row")));
	Chooser->Modify();

	// Remove from ResultsStructs
	Chooser->ResultsStructs.RemoveAt(RowIndex);

	// Remove from DisabledRows
	if (Chooser->DisabledRows.IsValidIndex(RowIndex))
	{
		Chooser->DisabledRows.RemoveAt(RowIndex);
	}

	// Remove from each column's RowValues
	TArray<uint32> RowIndices;
	RowIndices.Add(static_cast<uint32>(RowIndex));
	for (FInstancedStruct& ColStruct : Chooser->ColumnsStructs)
	{
		if (ColStruct.IsValid())
		{
			FChooserColumnBase* Col = ColStruct.GetMutablePtr<FChooserColumnBase>();
			if (Col)
			{
				Col->DeleteRows(RowIndices);
			}
		}
	}

	if (!ClaireonChooserHelpers::SaveChooserTable(Chooser, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
	Data->SetNumberField(TEXT("removed_index"), RowIndex);
	Data->SetNumberField(TEXT("row_count"), Chooser->ResultsStructs.Num());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Removed row %d (remaining: %d)"),
		RowIndex, Chooser->ResultsStructs.Num()));
#else
	return MakeErrorResult(TEXT("Row editing requires editor data"));
#endif
}
