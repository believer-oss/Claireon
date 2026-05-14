// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonChooserTool_SetFallbackResult.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Chooser.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "StructUtils/InstancedStruct.h"

FString ClaireonTool_ChooserSetFallbackResult::GetName() const { return TEXT("claireon.chooser_set_fallback_result"); }

FString ClaireonTool_ChooserSetFallbackResult::GetDescription() const
{
	return TEXT("Set the fallback result on a ChooserTable. The fallback is returned when no row matches. "
		"Supported types: Asset, SoftAsset, EvaluateChooser, LookupProxy.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserSetFallbackResult::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddEnum(TEXT("fallback_result_type"), TEXT("Fallback result type"),
		{TEXT("Asset"), TEXT("SoftAsset"), TEXT("EvaluateChooser"), TEXT("LookupProxy")}, true);
	S.AddString(TEXT("fallback_result_value"), TEXT("Fallback result value (asset / chooser / proxy path)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserSetFallbackResult::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString FallbackType;
	if (!Arguments->TryGetStringField(TEXT("fallback_result_type"), FallbackType) || FallbackType.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: fallback_result_type"));
	}

	FString FallbackValue;
	if (!Arguments->TryGetStringField(TEXT("fallback_result_value"), FallbackValue))
	{
		return MakeErrorResult(TEXT("Missing required parameter: fallback_result_value"));
	}

	FString Error;
	UChooserTable* Chooser = ClaireonChooserHelpers::LoadChooserTableAsset(AssetPath, Error);
	if (!Chooser)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Chooser Fallback Result")));
	Chooser->Modify();

	FInstancedStruct NewFallback;
	if (!ClaireonChooserHelpers::MakeRowResult(FallbackType, FallbackValue, NewFallback, Error))
	{
		return MakeErrorResult(Error);
	}
	Chooser->FallbackResult = MoveTemp(NewFallback);

	if (!ClaireonChooserHelpers::SaveChooserTable(Chooser, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
	Data->SetStringField(TEXT("fallback_result_type"), FallbackType);
	Data->SetStringField(TEXT("fallback_result_value"), FallbackValue);

	return MakeSuccessResult(Data, FString::Printf(TEXT("Set fallback result on ChooserTable '%s' to %s: %s"),
		*Chooser->GetName(), *FallbackType, *FallbackValue));
}
