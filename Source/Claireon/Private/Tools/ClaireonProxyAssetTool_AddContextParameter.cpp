// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonProxyAssetTool_AddContextParameter.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/ClaireonProxyTableHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ProxyAsset.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "StructUtils/InstancedStruct.h"

FString ClaireonTool_ProxyAssetAddContextParameter::GetCategory() const { return TEXT("proxyasset"); }
FString ClaireonTool_ProxyAssetAddContextParameter::GetOperation() const { return TEXT("add_context_parameter"); }

FString ClaireonTool_ProxyAssetAddContextParameter::GetDescription() const
{
	return TEXT("Add a context data parameter (struct or class) to a ProxyAsset. "
		"Direction controls Input / Output / InputOutput.");
}

TSharedPtr<FJsonObject> ClaireonTool_ProxyAssetAddContextParameter::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ProxyAsset"), true);
	S.AddEnum(TEXT("type"), TEXT("Parameter kind"), {TEXT("struct"), TEXT("class")}, true);
	S.AddString(TEXT("name"), TEXT("Struct or class name to bind to the new context parameter"), true);
	S.AddEnum(TEXT("direction"), TEXT("Parameter direction"),
		{TEXT("Input"), TEXT("Output"), TEXT("InputOutput")}, true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ProxyAssetAddContextParameter::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	UProxyAsset* ProxyAsset = ClaireonProxyTableHelpers::LoadProxyAsset(AssetPath, Error);
	if (!ProxyAsset)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add ProxyAsset Context Parameter")));
	ProxyAsset->Modify();

	// UProxyAsset has no Compile method. Ignore bContextChanged; just add + save.
	bool bContextChanged = false;
	if (!ClaireonChooserHelpers::AddContextParameter(ProxyAsset->ContextData, TypeStr, NameStr, DirStr, bContextChanged, Error))
	{
		return MakeErrorResult(Error);
	}

	if (!ClaireonProxyTableHelpers::SaveProxyAsset(ProxyAsset, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), ProxyAsset->GetPathName());
	Data->SetNumberField(TEXT("parameter_count"), ProxyAsset->ContextData.Num());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Added %s context parameter '%s' to ProxyAsset '%s'"),
		*TypeStr, *NameStr, *ProxyAsset->GetName()));
}
