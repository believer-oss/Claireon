// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonChooserTool_RemoveColumn.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Chooser.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "StructUtils/InstancedStruct.h"

FString ClaireonTool_ChooserRemoveColumn::GetCategory() const { return TEXT("chooser"); }
FString ClaireonTool_ChooserRemoveColumn::GetOperation() const { return TEXT("remove_column"); }

FString ClaireonTool_ChooserRemoveColumn::GetDescription() const
{
	return TEXT("Remove a column from a ChooserTable by index.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserRemoveColumn::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddInteger(TEXT("column_index"), TEXT("Index of the column to remove"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserRemoveColumn::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	double ColIdxDouble;
	if (!Arguments->TryGetNumberField(TEXT("column_index"), ColIdxDouble))
	{
		return MakeErrorResult(TEXT("Missing required parameter: column_index"));
	}
	int32 ColIndex = static_cast<int32>(ColIdxDouble);

	FString Error;
	UChooserTable* Chooser = ClaireonChooserHelpers::LoadChooserTableAsset(AssetPath, Error);
	if (!Chooser)
	{
		return MakeErrorResult(Error);
	}

	if (!Chooser->ColumnsStructs.IsValidIndex(ColIndex))
	{
		return MakeErrorResult(FString::Printf(TEXT("Column index %d out of bounds (column count: %d)"),
			ColIndex, Chooser->ColumnsStructs.Num()));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove ChooserTable Column")));
	Chooser->Modify();

	Chooser->ColumnsStructs.RemoveAt(ColIndex);

	if (!ClaireonChooserHelpers::SaveChooserTable(Chooser, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
	Data->SetNumberField(TEXT("removed_index"), ColIndex);
	Data->SetNumberField(TEXT("column_count"), Chooser->ColumnsStructs.Num());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Removed column %d (remaining: %d)"),
		ColIndex, Chooser->ColumnsStructs.Num()));
}
