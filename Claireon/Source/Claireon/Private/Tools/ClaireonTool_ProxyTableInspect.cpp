// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ProxyTableInspect.h"
#include "Tools/ClaireonProxyTableHelpers.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "ProxyTable.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_ProxyTableInspect::GetName() const { return TEXT("claireon.proxytable_inspect"); }

FString ClaireonTool_ProxyTableInspect::GetDescription() const
{
	return TEXT("Inspect a ProxyTable asset. Returns all entries with their proxy asset references, "
		"resolved values, output struct data, and the inheritance chain.");
}

TSharedPtr<FJsonObject> ClaireonTool_ProxyTableInspect::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ProxyTable asset"), true);
	S.AddEnum(TEXT("detail_level"), TEXT("Verbosity: 'summary' or 'full'"),
		{TEXT("summary"), TEXT("full")});
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ProxyTableInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString Error;
	UProxyTable* ProxyTable = ClaireonProxyTableHelpers::LoadProxyTableAsset(AssetPath, Error);
	if (!ProxyTable)
	{
		return MakeErrorResult(Error);
	}

	FString DetailLevel = TEXT("full");
	Arguments->TryGetStringField(TEXT("detail_level"), DetailLevel);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), ProxyTable->GetPathName());
	Data->SetStringField(TEXT("asset_name"), ProxyTable->GetName());

#if WITH_EDITORONLY_DATA
	int32 EntryCount = ProxyTable->Entries.Num();
	Data->SetNumberField(TEXT("entry_count"), EntryCount);

	// Inheritance chain
	if (ProxyTable->InheritEntriesFrom.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> InheritArray;
		for (const auto& Parent : ProxyTable->InheritEntriesFrom)
		{
			if (Parent)
			{
				TSharedPtr<FJsonObject> ParentObj = MakeShared<FJsonObject>();
				ParentObj->SetStringField(TEXT("name"), Parent->GetName());
				ParentObj->SetStringField(TEXT("path"), Parent->GetPathName());
				InheritArray.Add(MakeShared<FJsonValueObject>(ParentObj));
			}
		}
		Data->SetArrayField(TEXT("inherits_from"), InheritArray);
	}

	if (DetailLevel != TEXT("summary"))
	{
		// Serialize all entries
		TArray<TSharedPtr<FJsonValue>> EntriesArray;
		for (int32 i = 0; i < EntryCount; ++i)
		{
			TSharedPtr<FJsonObject> EntryObj = ClaireonProxyTableHelpers::SerializeProxyEntry(ProxyTable->Entries[i], i);
			EntriesArray.Add(MakeShared<FJsonValueObject>(EntryObj));
		}
		Data->SetArrayField(TEXT("entries"), EntriesArray);
	}
#else
	Data->SetNumberField(TEXT("entry_count"), ProxyTable->Keys.Num());
#endif

	FString Summary = FString::Printf(TEXT("ProxyTable '%s': %d entries"),
		*ProxyTable->GetName(),
#if WITH_EDITORONLY_DATA
		EntryCount
#else
		ProxyTable->Keys.Num()
#endif
	);

	return MakeSuccessResult(Data, Summary);
}
