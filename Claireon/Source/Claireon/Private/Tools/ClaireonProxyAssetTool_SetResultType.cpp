// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonProxyAssetTool_SetResultType.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/ClaireonProxyTableHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ProxyAsset.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

FString ClaireonTool_ProxyAssetSetResultType::GetCategory() const { return TEXT("proxyasset"); }
FString ClaireonTool_ProxyAssetSetResultType::GetOperation() const { return TEXT("set_result_type"); }

FString ClaireonTool_ProxyAssetSetResultType::GetDescription() const
{
    return TEXT("Set the result type (ObjectResult or ClassResult) on a ProxyAsset. Stateless / non-session: writes the asset directly by path.");
}

TSharedPtr<FJsonObject> ClaireonTool_ProxyAssetSetResultType::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ProxyAsset"), true);
	S.AddEnum(TEXT("result_type"), TEXT("Result type"),
		{TEXT("ObjectResult"), TEXT("ClassResult")}, true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ProxyAssetSetResultType::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	UProxyAsset* ProxyAsset = ClaireonProxyTableHelpers::LoadProxyAsset(AssetPath, Error);
	if (!ProxyAsset)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set ProxyAsset Result Type")));
	ProxyAsset->Modify();

	if (ResultTypeStr == TEXT("ClassResult"))
	{
		ProxyAsset->ResultType = EObjectChooserResultType::ClassResult;
	}
	else
	{
		ProxyAsset->ResultType = EObjectChooserResultType::ObjectResult;
	}

	if (!ClaireonProxyTableHelpers::SaveProxyAsset(ProxyAsset, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), ProxyAsset->GetPathName());
	Data->SetStringField(TEXT("result_type"),
		ClaireonChooserHelpers::ResultTypeToString(static_cast<uint8>(ProxyAsset->ResultType)));

	return MakeSuccessResult(Data, FString::Printf(TEXT("Set ProxyAsset '%s' result_type to %s"),
		*ProxyAsset->GetName(), *ResultTypeStr));
}
