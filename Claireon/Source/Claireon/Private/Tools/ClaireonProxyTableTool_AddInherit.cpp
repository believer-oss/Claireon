// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonProxyTableTool_AddInherit.h"
#include "Tools/ClaireonProxyTableHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ProxyTable.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

FString ClaireonTool_ProxyTableAddInherit::GetCategory() const { return TEXT("proxytable"); }
FString ClaireonTool_ProxyTableAddInherit::GetOperation() const { return TEXT("add_inherit"); }

FString ClaireonTool_ProxyTableAddInherit::GetDescription() const
{
    return TEXT("Add a parent ProxyTable to the InheritEntriesFrom chain of a ProxyTable by appending it at the end. Stateless / non-session: writes the asset directly by path.");
}

TSharedPtr<FJsonObject> ClaireonTool_ProxyTableAddInherit::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ProxyTable asset"), true);
	S.AddString(TEXT("parent_path"), TEXT("Path to the parent ProxyTable to add to InheritEntriesFrom"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ProxyTableAddInherit::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString ParentPath;
	if (!Arguments->TryGetStringField(TEXT("parent_path"), ParentPath) || ParentPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: parent_path"));
	}

	FString Error;
	UProxyTable* ProxyTable = ClaireonProxyTableHelpers::LoadProxyTableAsset(AssetPath, Error);
	if (!ProxyTable)
	{
		return MakeErrorResult(Error);
	}

#if WITH_EDITORONLY_DATA
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add ProxyTable Inherit")));
	ProxyTable->Modify();

	UProxyTable* ParentTable = ClaireonProxyTableHelpers::LoadProxyTableAsset(ParentPath, Error);
	if (!ParentTable)
	{
		return MakeErrorResult(Error);
	}

	ProxyTable->InheritEntriesFrom.Add(ParentTable);

	if (!ClaireonProxyTableHelpers::SaveProxyTable(ProxyTable, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), ProxyTable->GetPathName());
	Data->SetNumberField(TEXT("inherit_count"), ProxyTable->InheritEntriesFrom.Num());
	return MakeSuccessResult(Data, FString::Printf(TEXT("Added inherit for '%s'"), *ProxyTable->GetName()));
#else
	return MakeErrorResult(TEXT("ProxyTable editing requires editor data"));
#endif
}
