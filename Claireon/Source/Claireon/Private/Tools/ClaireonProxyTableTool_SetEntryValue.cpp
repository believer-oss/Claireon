// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonProxyTableTool_SetEntryValue.h"
#include "Tools/ClaireonProxyTableHelpers.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ProxyTable.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "StructUtils/InstancedStruct.h"

FString ClaireonTool_ProxyTableSetEntryValue::GetName() const { return TEXT("claireon.proxytable_set_entry_value"); }

FString ClaireonTool_ProxyTableSetEntryValue::GetDescription() const
{
	return TEXT("Set or change the value (resolved asset) for an entry in a ProxyTable.");
}

TSharedPtr<FJsonObject> ClaireonTool_ProxyTableSetEntryValue::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ProxyTable asset"), true);
	S.AddInteger(TEXT("entry_index"), TEXT("Index of the entry to modify"), true);
	S.AddEnum(TEXT("value_type"), TEXT("Value result type"),
		{TEXT("Asset"), TEXT("SoftAsset"), TEXT("EvaluateChooser"), TEXT("LookupProxy")}, true);
	S.AddString(TEXT("value"), TEXT("Asset/chooser/proxy path"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ProxyTableSetEntryValue::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	double EntryIdxDouble;
	if (!Arguments->TryGetNumberField(TEXT("entry_index"), EntryIdxDouble))
	{
		return MakeErrorResult(TEXT("Missing required parameter: entry_index"));
	}
	int32 EntryIndex = static_cast<int32>(EntryIdxDouble);

	FString ValueType, ValuePath;
	if (!Arguments->TryGetStringField(TEXT("value_type"), ValueType) || ValueType.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: value_type"));
	}
	if (!Arguments->TryGetStringField(TEXT("value"), ValuePath))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	FString Error;
	UProxyTable* ProxyTable = ClaireonProxyTableHelpers::LoadProxyTableAsset(AssetPath, Error);
	if (!ProxyTable)
	{
		return MakeErrorResult(Error);
	}

#if WITH_EDITORONLY_DATA
	if (!ProxyTable->Entries.IsValidIndex(EntryIndex))
	{
		return MakeErrorResult(FString::Printf(TEXT("Entry index %d out of bounds (entry count: %d)"),
			EntryIndex, ProxyTable->Entries.Num()));
	}

	FInstancedStruct NewValue;
	if (!ClaireonChooserHelpers::MakeRowResult(ValueType, ValuePath, NewValue, Error))
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set ProxyTable Entry Value")));
	ProxyTable->Modify();

	ProxyTable->Entries[EntryIndex].ValueStruct = MoveTemp(NewValue);

	if (!ClaireonProxyTableHelpers::SaveProxyTable(ProxyTable, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), ProxyTable->GetPathName());
	Data->SetNumberField(TEXT("entry_index"), EntryIndex);
	Data->SetStringField(TEXT("value_type"), ValueType);
	Data->SetStringField(TEXT("value"), ValuePath);

	return MakeSuccessResult(Data, FString::Printf(TEXT("Set entry %d value to %s: %s"),
		EntryIndex, *ValueType, *ValuePath));
#else
	return MakeErrorResult(TEXT("ProxyTable editing requires editor data"));
#endif
}
