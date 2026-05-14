// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonChooserTool_SetOutputClass.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Chooser.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

FString ClaireonTool_ChooserSetOutputClass::GetName() const { return TEXT("claireon.chooser_set_output_class"); }

FString ClaireonTool_ChooserSetOutputClass::GetDescription() const
{
	return TEXT("Set the output object class (OutputObjectType) on a ChooserTable.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserSetOutputClass::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddString(TEXT("output_class"), TEXT("Class path for OutputObjectType"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserSetOutputClass::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString OutputClassStr;
	if (!Arguments->TryGetStringField(TEXT("output_class"), OutputClassStr) || OutputClassStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: output_class"));
	}

	FString Error;
	UChooserTable* Chooser = ClaireonChooserHelpers::LoadChooserTableAsset(AssetPath, Error);
	if (!Chooser)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set ChooserTable Output Class")));
	Chooser->Modify();

	UClass* OutputClass = FindObject<UClass>(nullptr, *OutputClassStr);
	if (!OutputClass) OutputClass = LoadObject<UClass>(nullptr, *OutputClassStr);
	if (!OutputClass)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not find class: %s"), *OutputClassStr));
	}

	Chooser->OutputObjectType = OutputClass;

	if (!ClaireonChooserHelpers::SaveChooserTable(Chooser, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
	Data->SetStringField(TEXT("output_class"), OutputClass->GetPathName());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Set ChooserTable '%s' output_class to %s"),
		*Chooser->GetName(), *OutputClassStr));
}
