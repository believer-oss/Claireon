// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonProxyTableHelpers.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "ClaireonSafeExec.h"
#include "FileHelpers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "ProxyTable.h"
#include "ProxyAsset.h"
#include "ChooserPropertyAccess.h"

namespace ClaireonProxyTableHelpers
{

// ============================================================================
// Asset Loading & Saving
// ============================================================================

UProxyTable* LoadProxyTableAsset(const FString& AssetPath, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return nullptr;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	UObject* LoadedObj = FSoftObjectPath(ResolvedPath).TryLoad();
	if (!LoadedObj)
	{
		OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *ResolvedPath);
		return nullptr;
	}

	UProxyTable* ProxyTable = Cast<UProxyTable>(LoadedObj);
	if (!ProxyTable)
	{
		OutError = FString::Printf(TEXT("Asset at %s is not a ProxyTable (actual type: %s)"),
			*ResolvedPath, *LoadedObj->GetClass()->GetName());
		return nullptr;
	}

	return ProxyTable;
}

UProxyAsset* LoadProxyAsset(const FString& AssetPath, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return nullptr;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	UObject* LoadedObj = FSoftObjectPath(ResolvedPath).TryLoad();
	if (!LoadedObj)
	{
		OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *ResolvedPath);
		return nullptr;
	}

	UProxyAsset* ProxyAsset = Cast<UProxyAsset>(LoadedObj);
	if (!ProxyAsset)
	{
		OutError = FString::Printf(TEXT("Asset at %s is not a ProxyAsset (actual type: %s)"),
			*ResolvedPath, *LoadedObj->GetClass()->GetName());
		return nullptr;
	}

	return ProxyAsset;
}

bool SaveProxyTable(UProxyTable* ProxyTable, FString& OutError)
{
	if (!ProxyTable)
	{
		OutError = TEXT("Proxy table is null");
		return false;
	}

	// Notify the engine of the change so dependent systems update
	ProxyTable->PostEditChange();
	ProxyTable->MarkPackageDirty();

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(ProxyTable->GetPackage());
	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		OutError = TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor.");
		return false;
	}
	bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);
	if (!bSaved)
	{
		OutError = FString::Printf(TEXT("Failed to save package: %s"), *ProxyTable->GetPackage()->GetName());
	}

	// Refresh the asset editor if it's open
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(ProxyTable);

	return bSaved;
}

bool SaveProxyAsset(UProxyAsset* ProxyAsset, FString& OutError)
{
	if (!ProxyAsset)
	{
		OutError = TEXT("Proxy asset is null");
		return false;
	}

	// Notify the engine of the change so dependent systems update
	ProxyAsset->PostEditChange();
	ProxyAsset->MarkPackageDirty();

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(ProxyAsset->GetPackage());
	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		OutError = TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor.");
		return false;
	}
	bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);
	if (!bSaved)
	{
		OutError = FString::Printf(TEXT("Failed to save package: %s"), *ProxyAsset->GetPackage()->GetName());
	}

	// Refresh the asset editor if it's open
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(ProxyAsset);

	return bSaved;
}

// ============================================================================
// Proxy Entry Serialization
// ============================================================================

TArray<TSharedPtr<FJsonValue>> SerializeProxyStructOutputs(const TArray<FProxyStructOutput>& Outputs)
{
	TArray<TSharedPtr<FJsonValue>> Result;

	for (int32 i = 0; i < Outputs.Num(); ++i)
	{
		const FProxyStructOutput& Output = Outputs[i];
		TSharedPtr<FJsonObject> OutObj = MakeShared<FJsonObject>();
		OutObj->SetNumberField(TEXT("index"), i);

		// Serialize binding info
		TSharedPtr<FJsonObject> BindingObj = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ChainArray;
		for (const FName& Name : Output.Binding.PropertyBindingChain)
		{
			ChainArray.Add(MakeShared<FJsonValueString>(Name.ToString()));
		}
		BindingObj->SetArrayField(TEXT("property_chain"), ChainArray);
		BindingObj->SetNumberField(TEXT("context_index"), Output.Binding.ContextIndex);
		OutObj->SetObjectField(TEXT("binding"), BindingObj);

		// Serialize the struct value
		if (Output.Value.IsValid())
		{
			TSharedPtr<FJsonObject> ValueObj = ClaireonChooserHelpers::SerializeInstancedStructToJson(Output.Value);
			OutObj->SetObjectField(TEXT("value"), ValueObj);
		}

		Result.Add(MakeShared<FJsonValueObject>(OutObj));
	}

	return Result;
}

TSharedPtr<FJsonObject> SerializeProxyEntry(const FProxyEntry& Entry, int32 Index)
{
	TSharedPtr<FJsonObject> EntryObj = MakeShared<FJsonObject>();
	EntryObj->SetNumberField(TEXT("index"), Index);

	// Proxy asset info
	if (Entry.Proxy)
	{
		EntryObj->SetStringField(TEXT("proxy_name"), Entry.Proxy->GetName());
		EntryObj->SetStringField(TEXT("proxy_path"), Entry.Proxy->GetPathName());
		EntryObj->SetStringField(TEXT("proxy_guid"), Entry.Proxy->Guid.ToString());

		if (Entry.Proxy->Type)
		{
			EntryObj->SetStringField(TEXT("proxy_type"), Entry.Proxy->Type->GetName());
		}
	}
	else
	{
		EntryObj->SetStringField(TEXT("proxy_name"), TEXT("None"));
	}

	// Legacy key
	if (!Entry.Key.IsNone())
	{
		EntryObj->SetStringField(TEXT("key"), Entry.Key.ToString());
	}

	// Value (FInstancedStruct wrapping FObjectChooserBase)
	if (Entry.ValueStruct.IsValid())
	{
		TSharedPtr<FJsonObject> ValueObj = ClaireonChooserHelpers::SerializeRowResult(Entry.ValueStruct);
		EntryObj->SetObjectField(TEXT("value"), ValueObj);
	}

	// Output struct data
	if (Entry.OutputStructData.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> OutputArray = SerializeProxyStructOutputs(Entry.OutputStructData);
		EntryObj->SetArrayField(TEXT("output_struct_data"), OutputArray);
	}

	return EntryObj;
}

// ============================================================================
// Proxy Asset Serialization
// ============================================================================

TSharedPtr<FJsonObject> SerializeProxyAssetInfo(const UProxyAsset* ProxyAsset)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	if (!ProxyAsset)
	{
		return Result;
	}

	Result->SetStringField(TEXT("asset_name"), ProxyAsset->GetName());
	Result->SetStringField(TEXT("asset_path"), ProxyAsset->GetPathName());
	Result->SetStringField(TEXT("guid"), ProxyAsset->Guid.ToString());
	Result->SetStringField(TEXT("result_type"), ClaireonChooserHelpers::ResultTypeToString(static_cast<uint8>(ProxyAsset->ResultType)));

	if (ProxyAsset->Type)
	{
		Result->SetStringField(TEXT("type_name"), ProxyAsset->Type->GetName());
		Result->SetStringField(TEXT("type_path"), ProxyAsset->Type->GetPathName());
	}
	else
	{
		Result->SetStringField(TEXT("type_name"), TEXT("None"));
	}

	// Context data
	TArray<TSharedPtr<FJsonValue>> ContextArray = ClaireonChooserHelpers::SerializeContextData(ProxyAsset->ContextData);
	Result->SetArrayField(TEXT("context_data"), ContextArray);

	return Result;
}

} // namespace ClaireonProxyTableHelpers
