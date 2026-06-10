// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonProxyTableTool_RemoveEntry.h"
#include "Tools/ClaireonProxyTableHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ProxyTable.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

FString ClaireonTool_ProxyTableRemoveEntry::GetCategory() const { return TEXT("proxytable"); }
FString ClaireonTool_ProxyTableRemoveEntry::GetOperation() const { return TEXT("remove_entry"); }

FString ClaireonTool_ProxyTableRemoveEntry::GetDescription() const
{
    return TEXT("Remove an entry from a ProxyTable by index. Stateless / non-session: writes the asset directly by path without opening a session.");
}

TSharedPtr<FJsonObject> ClaireonTool_ProxyTableRemoveEntry::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ProxyTable asset"), true);
	S.AddInteger(TEXT("entry_index"), TEXT("Index of the entry to remove"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ProxyTableRemoveEntry::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove ProxyTable Entry")));
	ProxyTable->Modify();

	ProxyTable->Entries.RemoveAt(EntryIndex);

	if (!ClaireonProxyTableHelpers::SaveProxyTable(ProxyTable, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), ProxyTable->GetPathName());
	Data->SetNumberField(TEXT("removed_index"), EntryIndex);
	Data->SetNumberField(TEXT("entry_count"), ProxyTable->Entries.Num());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Removed entry %d (remaining: %d)"),
		EntryIndex, ProxyTable->Entries.Num()));
#else
	return MakeErrorResult(TEXT("ProxyTable editing requires editor data"));
#endif
}
