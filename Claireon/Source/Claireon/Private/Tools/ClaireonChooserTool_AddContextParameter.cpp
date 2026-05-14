// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonChooserTool_AddContextParameter.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Chooser.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "StructUtils/InstancedStruct.h"

FString ClaireonTool_ChooserAddContextParameter::GetCategory() const { return TEXT("chooser"); }
FString ClaireonTool_ChooserAddContextParameter::GetOperation() const { return TEXT("add_context_parameter"); }

FString ClaireonTool_ChooserAddContextParameter::GetDescription() const
{
	return TEXT("Add a context data parameter (struct or class) to a ChooserTable. "
		"Direction controls Input / Output / InputOutput. Triggers a recompile of the chooser bindings.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserAddContextParameter::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddEnum(TEXT("type"), TEXT("Parameter kind"), {TEXT("struct"), TEXT("class")}, true);
	S.AddString(TEXT("name"), TEXT("Struct or class name to bind to the new context parameter"), true);
	S.AddEnum(TEXT("direction"), TEXT("Parameter direction"),
		{TEXT("Input"), TEXT("Output"), TEXT("InputOutput")}, true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserAddContextParameter::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString TypeStr;
	if (!Arguments->TryGetStringField(TEXT("type"), TypeStr) || TypeStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: type"));
	}

	FString NameStr;
	if (!Arguments->TryGetStringField(TEXT("name"), NameStr) || NameStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: name"));
	}

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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Chooser Context Parameter")));
	Chooser->Modify();

	UChooserTable* ContextOwner = Chooser->GetRootChooser();
	bool bContextChanged = false;
	if (!ClaireonChooserHelpers::AddContextParameter(ContextOwner->ContextData, TypeStr, NameStr, DirStr, bContextChanged, Error))
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

	return MakeSuccessResult(Data, FString::Printf(TEXT("Added %s context parameter '%s' to ChooserTable '%s'"),
		*TypeStr, *NameStr, *Chooser->GetName()));
}
