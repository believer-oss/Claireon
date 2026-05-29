// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonProxyTableTool_AddEntry.h"
#include "Tools/ClaireonProxyTableHelpers.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ProxyTable.h"
#include "ProxyAsset.h"
#include "ObjectChooser_Asset.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "StructUtils/InstancedStruct.h"

FString ClaireonTool_ProxyTableAddEntry::GetCategory() const { return TEXT("proxytable"); }
FString ClaireonTool_ProxyTableAddEntry::GetOperation() const { return TEXT("add_entry"); }

FString ClaireonTool_ProxyTableAddEntry::GetDescription() const
{
    return TEXT("Add a new entry to a ProxyTable, mapping a ProxyAsset to a resolved value. Stateless / non-session: writes the asset directly by path.");
}

TSharedPtr<FJsonObject> ClaireonTool_ProxyTableAddEntry::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ProxyTable asset"), true);
	S.AddString(TEXT("proxy_asset"), TEXT("Path to the ProxyAsset to add"), true);
	S.AddEnum(TEXT("value_type"), TEXT("Value result type"),
		{TEXT("Asset"), TEXT("SoftAsset"), TEXT("EvaluateChooser"), TEXT("LookupProxy")});
	S.AddString(TEXT("value"), TEXT("Asset/chooser/proxy path for the entry value"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ProxyTableAddEntry::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString ProxyAssetPath;
	if (!Arguments->TryGetStringField(TEXT("proxy_asset"), ProxyAssetPath) || ProxyAssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: proxy_asset"));
	}

	FString Error;
	UProxyTable* ProxyTable = ClaireonProxyTableHelpers::LoadProxyTableAsset(AssetPath, Error);
	if (!ProxyTable)
	{
		return MakeErrorResult(Error);
	}

	UProxyAsset* Proxy = ClaireonProxyTableHelpers::LoadProxyAsset(ProxyAssetPath, Error);
	if (!Proxy)
	{
		return MakeErrorResult(Error);
	}

#if WITH_EDITORONLY_DATA
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add ProxyTable Entry")));
	ProxyTable->Modify();

	FProxyEntry NewEntry;
	NewEntry.Proxy = Proxy;

	// Set value if provided
	FString ValueType, ValuePath;
	Arguments->TryGetStringField(TEXT("value_type"), ValueType);
	Arguments->TryGetStringField(TEXT("value"), ValuePath);

	if (!ValueType.IsEmpty())
	{
		if (!ClaireonChooserHelpers::MakeRowResult(ValueType, ValuePath, NewEntry.ValueStruct, Error))
		{
			return MakeErrorResult(Error);
		}
	}
	else
	{
		// Default: empty asset chooser
		NewEntry.ValueStruct.InitializeAs<FAssetChooser>();
	}

	int32 NewIndex = ProxyTable->Entries.Add(MoveTemp(NewEntry));

	if (!ClaireonProxyTableHelpers::SaveProxyTable(ProxyTable, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), ProxyTable->GetPathName());
	Data->SetNumberField(TEXT("entry_index"), NewIndex);
	Data->SetNumberField(TEXT("entry_count"), ProxyTable->Entries.Num());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Added entry at index %d (total: %d)"),
		NewIndex, ProxyTable->Entries.Num()));
#else
	return MakeErrorResult(TEXT("ProxyTable editing requires editor data"));
#endif
}
