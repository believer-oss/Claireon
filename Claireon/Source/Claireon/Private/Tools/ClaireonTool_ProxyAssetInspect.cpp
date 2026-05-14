// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ProxyAssetInspect.h"
#include "Tools/ClaireonProxyTableHelpers.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "ProxyAsset.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_ProxyAssetInspect::GetName() const { return TEXT("claireon.proxyasset_inspect"); }

TArray<FString> ClaireonTool_ProxyAssetInspect::GetSearchKeywords() const
{
	return {TEXT("proxyasset"), TEXT("proxy"), TEXT("asset"), TEXT("inspect"), TEXT("result"), TEXT("context"), TEXT("parameter")};
}

FString ClaireonTool_ProxyAssetInspect::GetDescription() const
{
	return TEXT("Inspect a ProxyAsset. Returns the proxy's type, result type, GUID, "
		"and context data parameters (input/output structs and classes).");
}

TSharedPtr<FJsonObject> ClaireonTool_ProxyAssetInspect::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ProxyAsset"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ProxyAssetInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString Error;
	UProxyAsset* ProxyAsset = ClaireonProxyTableHelpers::LoadProxyAsset(AssetPath, Error);
	if (!ProxyAsset)
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = ClaireonProxyTableHelpers::SerializeProxyAssetInfo(ProxyAsset);

	FString Summary = FString::Printf(TEXT("ProxyAsset '%s': type=%s, guid=%s"),
		*ProxyAsset->GetName(),
		ProxyAsset->Type ? *ProxyAsset->Type->GetName() : TEXT("None"),
		*ProxyAsset->Guid.ToString());

	return MakeSuccessResult(Data, Summary);
}
