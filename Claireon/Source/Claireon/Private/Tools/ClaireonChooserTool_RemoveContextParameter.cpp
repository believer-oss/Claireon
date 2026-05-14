// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonChooserTool_RemoveContextParameter.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Chooser.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "StructUtils/InstancedStruct.h"

FString ClaireonTool_ChooserRemoveContextParameter::GetCategory() const { return TEXT("chooser"); }
FString ClaireonTool_ChooserRemoveContextParameter::GetOperation() const { return TEXT("remove_context_parameter"); }

FString ClaireonTool_ChooserRemoveContextParameter::GetDescription() const
{
	return TEXT("Remove a context data parameter at the given index from a ChooserTable. "
		"Triggers a recompile of the chooser bindings.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserRemoveContextParameter::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddInteger(TEXT("index"), TEXT("Zero-based index of the context parameter to remove"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserRemoveContextParameter::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString Error;
	UChooserTable* Chooser = ClaireonChooserHelpers::LoadChooserTableAsset(AssetPath, Error);
	if (!Chooser)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Chooser Context Parameter")));
	Chooser->Modify();

	UChooserTable* ContextOwner = Chooser->GetRootChooser();
	bool bContextChanged = false;
	if (!ClaireonChooserHelpers::RemoveContextParameter(ContextOwner->ContextData, Index, bContextChanged, Error))
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
	Data->SetNumberField(TEXT("parameter_count"), ContextOwner->ContextData.Num());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Removed context parameter %d from ChooserTable '%s'"),
		Index, *Chooser->GetName()));
}
