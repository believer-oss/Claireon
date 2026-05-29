// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonChooserTool_SetResultType.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Chooser.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

FString ClaireonTool_ChooserSetResultType::GetCategory() const { return TEXT("chooser"); }
FString ClaireonTool_ChooserSetResultType::GetOperation() const { return TEXT("set_result_type"); }

FString ClaireonTool_ChooserSetResultType::GetDescription() const
{
    return TEXT("Set the result type (ObjectResult or ClassResult) on a ChooserTable. Stateless / non-session: writes the asset directly by path, no open session required.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserSetResultType::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddEnum(TEXT("result_type"), TEXT("Result type"),
		{TEXT("ObjectResult"), TEXT("ClassResult")}, true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserSetResultType::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString ResultTypeStr;
	if (!Arguments->TryGetStringField(TEXT("result_type"), ResultTypeStr) || ResultTypeStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: result_type"));
	}

	FString Error;
	UChooserTable* Chooser = ClaireonChooserHelpers::LoadChooserTableAsset(AssetPath, Error);
	if (!Chooser)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set ChooserTable Result Type")));
	Chooser->Modify();

	if (ResultTypeStr == TEXT("ClassResult"))
	{
		Chooser->ResultType = EObjectChooserResultType::ClassResult;
	}
	else
	{
		Chooser->ResultType = EObjectChooserResultType::ObjectResult;
	}

	if (!ClaireonChooserHelpers::SaveChooserTable(Chooser, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
	Data->SetStringField(TEXT("result_type"),
		ClaireonChooserHelpers::ResultTypeToString(static_cast<uint8>(Chooser->ResultType)));

	return MakeSuccessResult(Data, FString::Printf(TEXT("Set ChooserTable '%s' result_type to %s"),
		*Chooser->GetName(), *ResultTypeStr));
}
