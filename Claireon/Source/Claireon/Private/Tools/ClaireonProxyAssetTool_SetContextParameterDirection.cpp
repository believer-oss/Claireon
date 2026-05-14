// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonProxyAssetTool_SetContextParameterDirection.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/ClaireonProxyTableHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ProxyAsset.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "StructUtils/InstancedStruct.h"

FString ClaireonTool_ProxyAssetSetContextParameterDirection::GetName() const { return TEXT("claireon.proxyasset_set_context_parameter_direction"); }

FString ClaireonTool_ProxyAssetSetContextParameterDirection::GetDescription() const
{
	return TEXT("Set the direction (Input / Output / InputOutput) on an existing ProxyAsset context parameter.");
}

TSharedPtr<FJsonObject> ClaireonTool_ProxyAssetSetContextParameterDirection::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ProxyAsset"), true);
	S.AddInteger(TEXT("index"), TEXT("Zero-based index of the context parameter to modify"), true);
	S.AddEnum(TEXT("direction"), TEXT("New direction"),
		{TEXT("Input"), TEXT("Output"), TEXT("InputOutput")}, true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ProxyAssetSetContextParameterDirection::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	UProxyAsset* ProxyAsset = ClaireonProxyTableHelpers::LoadProxyAsset(AssetPath, Error);
	if (!ProxyAsset)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set ProxyAsset Context Parameter Direction")));
	ProxyAsset->Modify();

	// No Compile on UProxyAsset.
	bool bContextChanged = false;
	if (!ClaireonChooserHelpers::SetContextParameterDirection(ProxyAsset->ContextData, Index, DirStr, bContextChanged, Error))
	{
		return MakeErrorResult(Error);
	}

	if (!ClaireonProxyTableHelpers::SaveProxyAsset(ProxyAsset, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), ProxyAsset->GetPathName());
	Data->SetNumberField(TEXT("index"), Index);
	Data->SetStringField(TEXT("direction"), DirStr);
	Data->SetNumberField(TEXT("parameter_count"), ProxyAsset->ContextData.Num());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Set direction of context parameter %d to %s on ProxyAsset '%s'"),
		Index, *DirStr, *ProxyAsset->GetName()));
}
