// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonProxyTableTool_RemoveInherit.h"
#include "Tools/ClaireonProxyTableHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ProxyTable.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

FString ClaireonTool_ProxyTableRemoveInherit::GetName() const { return TEXT("claireon.proxytable_remove_inherit"); }

FString ClaireonTool_ProxyTableRemoveInherit::GetDescription() const
{
	return TEXT("Remove the entry at the given index from a ProxyTable's InheritEntriesFrom chain.");
}

TSharedPtr<FJsonObject> ClaireonTool_ProxyTableRemoveInherit::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ProxyTable asset"), true);
	S.AddInteger(TEXT("index"), TEXT("Zero-based index into InheritEntriesFrom to remove"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ProxyTableRemoveInherit::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	double IndexDouble;
	if (!Arguments->TryGetNumberField(TEXT("index"), IndexDouble))
	{
		return MakeErrorResult(TEXT("Missing required parameter: index"));
	}
	const int32 Index = static_cast<int32>(IndexDouble);

	FString Error;
	UProxyTable* ProxyTable = ClaireonProxyTableHelpers::LoadProxyTableAsset(AssetPath, Error);
	if (!ProxyTable)
	{
		return MakeErrorResult(Error);
	}

#if WITH_EDITORONLY_DATA
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove ProxyTable Inherit")));
	ProxyTable->Modify();

	if (!ProxyTable->InheritEntriesFrom.IsValidIndex(Index))
	{
		return MakeErrorResult(FString::Printf(TEXT("Inherit index %d out of bounds (count: %d)"),
			Index, ProxyTable->InheritEntriesFrom.Num()));
	}

	ProxyTable->InheritEntriesFrom.RemoveAt(Index);

	if (!ClaireonProxyTableHelpers::SaveProxyTable(ProxyTable, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), ProxyTable->GetPathName());
	Data->SetNumberField(TEXT("inherit_count"), ProxyTable->InheritEntriesFrom.Num());
	return MakeSuccessResult(Data, FString::Printf(TEXT("Removed inherit %d"), Index));
#else
	return MakeErrorResult(TEXT("ProxyTable editing requires editor data"));
#endif
}
