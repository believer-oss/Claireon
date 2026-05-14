// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ChooserRowDescend.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Chooser.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_ChooserRowDescend::GetCategory() const { return TEXT("chooser"); }
FString ClaireonTool_ChooserRowDescend::GetOperation() const { return TEXT("row_descend"); }

FString ClaireonTool_ChooserRowDescend::GetDescription() const
{
	return TEXT("Given a parent chooser and a row index, follow that row's result reference one hop "
		"into the target chooser (FNestedChooser or FEvaluateChooser). Returns identifying info on the "
		"hop and a compact summary of the target chooser. Composes with chooser_walk: walk first, "
		"then descend specific rows of interest. Use chooser_inspect for the full body of the target.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserRowDescend::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Parent ChooserTable asset path"), true);
	S.AddInteger(TEXT("row_index"), TEXT("Row in the parent chooser whose result to follow"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserRowDescend::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	double RowIdxD;
	if (!Arguments->TryGetNumberField(TEXT("row_index"), RowIdxD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: row_index"));
	}
	const int32 RowIndex = static_cast<int32>(RowIdxD);

	FString Error;
	UChooserTable* Parent = ClaireonChooserHelpers::LoadChooserTableAsset(AssetPath, Error);
	if (!Parent)
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
	Source->SetStringField(TEXT("asset_path"), Parent->GetPathName());
	Source->SetNumberField(TEXT("row_index"), RowIndex);
	Data->SetObjectField(TEXT("source"), Source);

#if WITH_EDITORONLY_DATA
	if (!Parent->ResultsStructs.IsValidIndex(RowIndex))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Row %d out of range (parent has %d rows)"),
			RowIndex, Parent->ResultsStructs.Num()));
	}

	const FInstancedStruct& Result = Parent->ResultsStructs[RowIndex];
	UChooserTable* Target = nullptr;
	FString HopType;

	if (const FNestedChooser* NC = Result.GetPtr<FNestedChooser>())
	{
		Target = NC->Chooser;
		HopType = TEXT("NestedChooser");
	}
	else if (const FEvaluateChooser* EC = Result.GetPtr<FEvaluateChooser>())
	{
		Target = EC->Chooser;
		HopType = TEXT("EvaluateChooser");
	}

	if (!Target)
	{
		Data->SetField(TEXT("follows_to"), MakeShared<FJsonValueNull>());
		const FString Why = Result.IsValid()
			? FString::Printf(TEXT("Row %d result is not a chooser ref (struct_type=%s)"),
				RowIndex, *Result.GetScriptStruct()->GetName())
			: FString::Printf(TEXT("Row %d has no result"), RowIndex);
		Data->SetStringField(TEXT("note"), Why);
		return MakeSuccessResult(Data, Why);
	}

	TSharedPtr<FJsonObject> FollowsTo = MakeShared<FJsonObject>();
	FollowsTo->SetStringField(TEXT("type"), HopType);
	FollowsTo->SetStringField(TEXT("path"), Target->GetPathName());
	FollowsTo->SetStringField(TEXT("name"), Target->GetName());
	FollowsTo->SetNumberField(TEXT("row_count"), Target->ResultsStructs.Num());
	FollowsTo->SetNumberField(TEXT("column_count"), Target->ColumnsStructs.Num());
	FollowsTo->SetStringField(TEXT("result_type"),
		ClaireonChooserHelpers::ResultTypeToString(static_cast<uint8>(Target->ResultType)));
	if (Target->OutputObjectType)
	{
		FollowsTo->SetStringField(TEXT("output_object_type"), Target->OutputObjectType->GetName());
	}
	Data->SetObjectField(TEXT("follows_to"), FollowsTo);

	const FString Summary = FString::Printf(TEXT("Row %d -> %s '%s' (%d rows)"),
		RowIndex, *HopType, *Target->GetName(), Target->ResultsStructs.Num());
	return MakeSuccessResult(Data, Summary);
#else
	return MakeErrorResult(TEXT("chooser_row_descend requires editor-only data"));
#endif
}
