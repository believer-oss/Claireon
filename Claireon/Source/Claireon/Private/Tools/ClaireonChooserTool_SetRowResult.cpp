// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonChooserTool_SetRowResult.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Chooser.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "StructUtils/InstancedStruct.h"

FString ClaireonTool_ChooserSetRowResult::GetCategory() const { return TEXT("chooser"); }
FString ClaireonTool_ChooserSetRowResult::GetOperation() const { return TEXT("set_row_result"); }

FString ClaireonTool_ChooserSetRowResult::GetDescription() const
{
	return TEXT("Set or change the result for a specific row in a ChooserTable.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserSetRowResult::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddInteger(TEXT("row_index"), TEXT("Index of the row to modify"), true);
	S.AddEnum(TEXT("result_type"), TEXT("Result type"),
		{TEXT("Asset"), TEXT("SoftAsset"), TEXT("EvaluateChooser"), TEXT("LookupProxy")}, true);
	S.AddString(TEXT("result_value"), TEXT("Asset/chooser/proxy path"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserSetRowResult::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString ResultType, ResultValue;
	if (!Arguments->TryGetStringField(TEXT("result_type"), ResultType) || ResultType.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: result_type"));
	}
	if (!Arguments->TryGetStringField(TEXT("result_value"), ResultValue))
	{
		return MakeErrorResult(TEXT("Missing required parameter: result_value"));
	}

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

	FInstancedStruct NewResult;
	if (!ClaireonChooserHelpers::MakeRowResult(ResultType, ResultValue, NewResult, Error))
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set ChooserTable Row Result")));
	Chooser->Modify();

	Chooser->ResultsStructs[RowIndex] = MoveTemp(NewResult);

	if (!ClaireonChooserHelpers::SaveChooserTable(Chooser, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
	Data->SetNumberField(TEXT("row_index"), RowIndex);
	Data->SetStringField(TEXT("result_type"), ResultType);
	Data->SetStringField(TEXT("result_value"), ResultValue);

	return MakeSuccessResult(Data, FString::Printf(TEXT("Set row %d result to %s: %s"),
		RowIndex, *ResultType, *ResultValue));
#else
	return MakeErrorResult(TEXT("Row editing requires editor data"));
#endif
}
