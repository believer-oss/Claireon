// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonChooserTool_SetContextParameterDirection.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Chooser.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "StructUtils/InstancedStruct.h"

FString ClaireonTool_ChooserSetContextParameterDirection::GetCategory() const { return TEXT("chooser"); }
FString ClaireonTool_ChooserSetContextParameterDirection::GetOperation() const { return TEXT("set_context_parameter_direction"); }

FString ClaireonTool_ChooserSetContextParameterDirection::GetDescription() const
{
	return TEXT("Set the direction (Input / Output / InputOutput) on an existing ChooserTable context parameter. "
		"Triggers a recompile of the chooser bindings.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserSetContextParameterDirection::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddInteger(TEXT("index"), TEXT("Zero-based index of the context parameter to modify"), true);
	S.AddEnum(TEXT("direction"), TEXT("New direction"),
		{TEXT("Input"), TEXT("Output"), TEXT("InputOutput")}, true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserSetContextParameterDirection::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	double IdxDouble;
	if (!Arguments->TryGetNumberField(TEXT("index"), IdxDouble))
	{
		return MakeErrorResult(TEXT("Missing required parameter: index"));
	}
	const int32 Index = static_cast<int32>(IdxDouble);

	FString DirStr;
	if (!Arguments->TryGetStringField(TEXT("direction"), DirStr) || DirStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: direction"));
	}

	FString Error;
	UChooserTable* Chooser = ClaireonChooserHelpers::LoadChooserTableAsset(AssetPath, Error);
	if (!Chooser)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Chooser Context Parameter Direction")));
	Chooser->Modify();

	UChooserTable* ContextOwner = Chooser->GetRootChooser();
	bool bContextChanged = false;
	if (!ClaireonChooserHelpers::SetContextParameterDirection(ContextOwner->ContextData, Index, DirStr, bContextChanged, Error))
	{
		return MakeErrorResult(Error);
	}

	if (bContextChanged)
	{
		Chooser->Compile(true);
	}

	if (!ClaireonChooserHelpers::SaveChooserTable(Chooser, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
	Data->SetNumberField(TEXT("index"), Index);
	Data->SetStringField(TEXT("direction"), DirStr);
	Data->SetNumberField(TEXT("parameter_count"), ContextOwner->ContextData.Num());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Set direction of context parameter %d to %s on ChooserTable '%s'"),
		Index, *DirStr, *Chooser->GetName()));
}
