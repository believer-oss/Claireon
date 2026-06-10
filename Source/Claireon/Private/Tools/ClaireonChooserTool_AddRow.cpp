// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonChooserTool_AddRow.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Chooser.h"
#include "IChooserColumn.h"
#include "ObjectChooser_Asset.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "StructUtils/InstancedStruct.h"

FString ClaireonTool_ChooserAddRow::GetCategory() const { return TEXT("chooser"); }
FString ClaireonTool_ChooserAddRow::GetOperation() const { return TEXT("add_row"); }

FString ClaireonTool_ChooserAddRow::GetDescription() const
{
    return TEXT("Add a new row to a ChooserTable, optionally setting the result and an insert position. Stateless / non-session: writes the asset directly by path, no open session required.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserAddRow::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddEnum(TEXT("result_type"), TEXT("Row result type"),
		{TEXT("Asset"), TEXT("SoftAsset"), TEXT("EvaluateChooser"), TEXT("LookupProxy")});
	S.AddString(TEXT("result_value"), TEXT("Asset/chooser/proxy path for the result"));
	S.AddInteger(TEXT("insert_index"), TEXT("Insert position (default: append at end)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserAddRow::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

#if WITH_EDITORONLY_DATA
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add ChooserTable Row")));
	Chooser->Modify();

	int32 CurrentRowCount = Chooser->ResultsStructs.Num();
	int32 InsertIndex = CurrentRowCount; // default: append

	double InsertIdxDouble;
	if (Arguments->TryGetNumberField(TEXT("insert_index"), InsertIdxDouble))
	{
		InsertIndex = FMath::Clamp(static_cast<int32>(InsertIdxDouble), 0, CurrentRowCount);
	}

	// Build the result FInstancedStruct
	FInstancedStruct NewResult;
	FString ResultType, ResultValue;
	Arguments->TryGetStringField(TEXT("result_type"), ResultType);
	Arguments->TryGetStringField(TEXT("result_value"), ResultValue);

	if (!ResultType.IsEmpty())
	{
		if (!ClaireonChooserHelpers::MakeRowResult(ResultType, ResultValue, NewResult, Error))
		{
			return MakeErrorResult(Error);
		}
	}
	else
	{
		// Default: empty asset chooser
		NewResult.InitializeAs<FAssetChooser>();
	}

	// Insert into ResultsStructs
	if (InsertIndex >= CurrentRowCount)
	{
		Chooser->ResultsStructs.Add(MoveTemp(NewResult));
	}
	else
	{
		Chooser->ResultsStructs.Insert(MoveTemp(NewResult), InsertIndex);
	}

	// Update DisabledRows
	if (InsertIndex >= Chooser->DisabledRows.Num())
	{
		Chooser->DisabledRows.Add(false);
	}
	else
	{
		Chooser->DisabledRows.Insert(false, InsertIndex);
	}

	// Update each column's RowValues via InsertRows
	for (FInstancedStruct& ColStruct : Chooser->ColumnsStructs)
	{
		if (ColStruct.IsValid())
		{
			FChooserColumnBase* Col = ColStruct.GetMutablePtr<FChooserColumnBase>();
			if (Col)
			{
				Col->InsertRows(InsertIndex, 1);
			}
		}
	}

	if (!ClaireonChooserHelpers::SaveChooserTable(Chooser, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
	Data->SetNumberField(TEXT("row_index"), InsertIndex);
	Data->SetNumberField(TEXT("row_count"), Chooser->ResultsStructs.Num());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Added row at index %d (total: %d)"),
		InsertIndex, Chooser->ResultsStructs.Num()));
#else
	return MakeErrorResult(TEXT("Row editing requires editor data"));
#endif
}
