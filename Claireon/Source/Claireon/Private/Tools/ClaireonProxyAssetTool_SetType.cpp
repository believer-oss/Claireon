// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonProxyAssetTool_SetType.h"
#include "Tools/ClaireonProxyTableHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ProxyAsset.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

FString ClaireonTool_ProxyAssetSetType::GetCategory() const { return TEXT("proxyasset"); }
FString ClaireonTool_ProxyAssetSetType::GetOperation() const { return TEXT("set_type"); }

FString ClaireonTool_ProxyAssetSetType::GetDescription() const
{
    return TEXT("Set the Type class on a ProxyAsset by full class path. Stateless / non-session: writes the asset directly by path without opening a session.");
}

TSharedPtr<FJsonObject> ClaireonTool_ProxyAssetSetType::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ProxyAsset"), true);
	S.AddString(TEXT("type"), TEXT("Class path for the proxy's Type"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ProxyAssetSetType::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString Error;
	UProxyAsset* ProxyAsset = ClaireonProxyTableHelpers::LoadProxyAsset(AssetPath, Error);
	if (!ProxyAsset)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set ProxyAsset Type")));
	ProxyAsset->Modify();

	UClass* TypeClass = FindObject<UClass>(nullptr, *TypeStr);
	if (!TypeClass) TypeClass = LoadObject<UClass>(nullptr, *TypeStr);
	if (!TypeClass)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not find class: %s"), *TypeStr));
	}

	ProxyAsset->Type = TypeClass;

	if (!ClaireonProxyTableHelpers::SaveProxyAsset(ProxyAsset, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), ProxyAsset->GetPathName());
	Data->SetStringField(TEXT("type"), TypeClass->GetPathName());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Set ProxyAsset '%s' type to %s"),
		*ProxyAsset->GetName(), *TypeStr));
}
