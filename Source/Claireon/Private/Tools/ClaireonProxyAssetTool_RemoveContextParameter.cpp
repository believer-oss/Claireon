// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonProxyAssetTool_RemoveContextParameter.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/ClaireonProxyTableHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ProxyAsset.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "StructUtils/InstancedStruct.h"

FString ClaireonTool_ProxyAssetRemoveContextParameter::GetCategory() const { return TEXT("proxyasset"); }
FString ClaireonTool_ProxyAssetRemoveContextParameter::GetOperation() const { return TEXT("remove_context_parameter"); }

FString ClaireonTool_ProxyAssetRemoveContextParameter::GetDescription() const
{
    return TEXT("Remove a context data parameter at the given index from a ProxyAsset. Stateless / non-session: writes the asset directly by path.");
}

TSharedPtr<FJsonObject> ClaireonTool_ProxyAssetRemoveContextParameter::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ProxyAsset"), true);
	S.AddInteger(TEXT("index"), TEXT("Zero-based index of the context parameter to remove"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ProxyAssetRemoveContextParameter::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	UProxyAsset* ProxyAsset = ClaireonProxyTableHelpers::LoadProxyAsset(AssetPath, Error);
	if (!ProxyAsset)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove ProxyAsset Context Parameter")));
	ProxyAsset->Modify();

	// No Compile on UProxyAsset.
	bool bContextChanged = false;
	if (!ClaireonChooserHelpers::RemoveContextParameter(ProxyAsset->ContextData, Index, bContextChanged, Error))
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

	return MakeSuccessResult(Data, FString::Printf(TEXT("Removed context parameter %d from ProxyAsset '%s'"),
		Index, *ProxyAsset->GetName()));
}
