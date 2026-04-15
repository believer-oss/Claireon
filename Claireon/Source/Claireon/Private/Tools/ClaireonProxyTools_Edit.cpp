// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonProxyTools_Edit.h"
#include "Tools/ClaireonProxyTableHelpers.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "ProxyTable.h"
#include "ProxyAsset.h"
#include "ChooserPropertyAccess.h"
#include "ObjectChooser_Asset.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "StructUtils/InstancedStruct.h"

// ============================================================================
// claireon.proxytable_edit — Edit structural properties of a ProxyTable
// ============================================================================

FString ClaireonTool_ProxyTableEdit::GetName() const { return TEXT("claireon.proxytable_edit"); }

FString ClaireonTool_ProxyTableEdit::GetDescription() const
{
	return TEXT("Edit structural properties of a ProxyTable: add or remove tables from the inheritance chain.");
}

TSharedPtr<FJsonObject> ClaireonTool_ProxyTableEdit::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ProxyTable asset"), true);
	S.AddString(TEXT("add_inherit"), TEXT("Path to a ProxyTable to add to InheritEntriesFrom"));
	S.AddInteger(TEXT("remove_inherit"), TEXT("Index in InheritEntriesFrom to remove"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ProxyTableEdit::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

#if WITH_EDITORONLY_DATA
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Edit ProxyTable")));
	ProxyTable->Modify();
	bool bChanged = false;

	// Add inherit
	FString AddInheritPath;
	if (Arguments->TryGetStringField(TEXT("add_inherit"), AddInheritPath) && !AddInheritPath.IsEmpty())
	{
		UProxyTable* ParentTable = ClaireonProxyTableHelpers::LoadProxyTableAsset(AddInheritPath, Error);
		if (!ParentTable)
		{
			return MakeErrorResult(Error);
		}
		ProxyTable->InheritEntriesFrom.Add(ParentTable);
		bChanged = true;
	}

	// Remove inherit
	double RemoveIdx;
	if (Arguments->TryGetNumberField(TEXT("remove_inherit"), RemoveIdx))
	{
		int32 Idx = static_cast<int32>(RemoveIdx);
		if (!ProxyTable->InheritEntriesFrom.IsValidIndex(Idx))
		{
			return MakeErrorResult(FString::Printf(TEXT("Inherit index %d out of bounds (count: %d)"),
				Idx, ProxyTable->InheritEntriesFrom.Num()));
		}
		ProxyTable->InheritEntriesFrom.RemoveAt(Idx);
		bChanged = true;
	}

	if (bChanged)
	{
		if (!ClaireonProxyTableHelpers::SaveProxyTable(ProxyTable, Error))
		{
			return MakeErrorResult(Error);
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), ProxyTable->GetPathName());
	Data->SetBoolField(TEXT("changed"), bChanged);
	Data->SetNumberField(TEXT("inherit_count"), ProxyTable->InheritEntriesFrom.Num());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Edited ProxyTable '%s'"), *ProxyTable->GetName()));
#else
	return MakeErrorResult(TEXT("ProxyTable editing requires editor data"));
#endif
}

// ============================================================================
// claireon.proxyasset_edit — Edit structural properties of a ProxyAsset
// ============================================================================

FString ClaireonTool_ProxyAssetEdit::GetName() const { return TEXT("claireon.proxyasset_edit"); }

FString ClaireonTool_ProxyAssetEdit::GetDescription() const
{
	return TEXT("Edit structural properties of a ProxyAsset: type, result type, "
		"and context data parameters (add/remove input/output structs and classes).");
}

TSharedPtr<FJsonObject> ClaireonTool_ProxyAssetEdit::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ProxyAsset"), true);
	S.AddString(TEXT("type"), TEXT("Class path for the proxy's Type"));
	S.AddEnum(TEXT("result_type"), TEXT("Set result type"),
		{TEXT("ObjectResult"), TEXT("ClassResult")});
	S.AddObject(TEXT("add_parameter"), TEXT("Add context parameter: {\"type\": \"struct\"/\"class\", \"name\": \"...\", \"direction\": \"Input\"/\"Output\"/\"InputOutput\"}"));
	S.AddInteger(TEXT("remove_parameter"), TEXT("Remove context parameter at this index"));
	S.AddObject(TEXT("set_parameter_direction"), TEXT("Change parameter direction: {\"index\": N, \"direction\": \"...\"}"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ProxyAssetEdit::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Edit ProxyAsset")));
	ProxyAsset->Modify();
	bool bChanged = false;

	// Type
	FString TypeStr;
	if (Arguments->TryGetStringField(TEXT("type"), TypeStr) && !TypeStr.IsEmpty())
	{
		UClass* TypeClass = FindObject<UClass>(nullptr, *TypeStr);
		if (!TypeClass) TypeClass = LoadObject<UClass>(nullptr, *TypeStr);
		if (TypeClass)
		{
			ProxyAsset->Type = TypeClass;
			bChanged = true;
		}
		else
		{
			return MakeErrorResult(FString::Printf(TEXT("Could not find class: %s"), *TypeStr));
		}
	}

	// Result type
	FString ResultTypeStr;
	if (Arguments->TryGetStringField(TEXT("result_type"), ResultTypeStr))
	{
		if (ResultTypeStr == TEXT("ClassResult"))
			ProxyAsset->ResultType = EObjectChooserResultType::ClassResult;
		else
			ProxyAsset->ResultType = EObjectChooserResultType::ObjectResult;
		bChanged = true;
	}

	// Add parameter
	const TSharedPtr<FJsonObject>* AddParamObj;
	if (Arguments->TryGetObjectField(TEXT("add_parameter"), AddParamObj))
	{
		FString ParamType, ParamName, DirStr;
		(*AddParamObj)->TryGetStringField(TEXT("type"), ParamType);
		(*AddParamObj)->TryGetStringField(TEXT("name"), ParamName);
		(*AddParamObj)->TryGetStringField(TEXT("direction"), DirStr);
		EContextObjectDirection Dir = static_cast<EContextObjectDirection>(ClaireonChooserHelpers::ParseDirection(DirStr));

		if (ParamType.Equals(TEXT("struct"), ESearchCase::IgnoreCase))
		{
			UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *ParamName);
			if (!Struct) Struct = LoadObject<UScriptStruct>(nullptr, *ParamName);
			if (!Struct)
			{
				return MakeErrorResult(FString::Printf(TEXT("Could not find struct: %s"), *ParamName));
			}

			FInstancedStruct NewParam;
			NewParam.InitializeAs<FContextObjectTypeStruct>();
			FContextObjectTypeStruct& StructParam = NewParam.GetMutable<FContextObjectTypeStruct>();
			StructParam.Struct = Struct;
			StructParam.Direction = Dir;
			ProxyAsset->ContextData.Add(MoveTemp(NewParam));
			bChanged = true;
		}
		else if (ParamType.Equals(TEXT("class"), ESearchCase::IgnoreCase))
		{
			UClass* Class = FindObject<UClass>(nullptr, *ParamName);
			if (!Class) Class = LoadObject<UClass>(nullptr, *ParamName);
			if (!Class)
			{
				return MakeErrorResult(FString::Printf(TEXT("Could not find class: %s"), *ParamName));
			}

			FInstancedStruct NewParam;
			NewParam.InitializeAs<FContextObjectTypeClass>();
			FContextObjectTypeClass& ClassParam = NewParam.GetMutable<FContextObjectTypeClass>();
			ClassParam.Class = Class;
			ClassParam.Direction = Dir;
			ProxyAsset->ContextData.Add(MoveTemp(NewParam));
			bChanged = true;
		}
		else
		{
			return MakeErrorResult(TEXT("add_parameter.type must be 'struct' or 'class'"));
		}
	}

	// Remove parameter
	double RemoveIdx;
	if (Arguments->TryGetNumberField(TEXT("remove_parameter"), RemoveIdx))
	{
		int32 Idx = static_cast<int32>(RemoveIdx);
		if (!ProxyAsset->ContextData.IsValidIndex(Idx))
		{
			return MakeErrorResult(FString::Printf(TEXT("Parameter index %d out of bounds (count: %d)"),
				Idx, ProxyAsset->ContextData.Num()));
		}
		ProxyAsset->ContextData.RemoveAt(Idx);
		bChanged = true;
	}

	// Set parameter direction
	const TSharedPtr<FJsonObject>* SetDirObj;
	if (Arguments->TryGetObjectField(TEXT("set_parameter_direction"), SetDirObj))
	{
		double IdxVal;
		FString DirStr;
		(*SetDirObj)->TryGetNumberField(TEXT("index"), IdxVal);
		(*SetDirObj)->TryGetStringField(TEXT("direction"), DirStr);
		int32 Idx = static_cast<int32>(IdxVal);

		if (!ProxyAsset->ContextData.IsValidIndex(Idx))
		{
			return MakeErrorResult(FString::Printf(TEXT("Parameter index %d out of bounds"), Idx));
		}

		FInstancedStruct& Param = ProxyAsset->ContextData[Idx];
		if (FContextObjectTypeBase* Base = Param.GetMutablePtr<FContextObjectTypeBase>())
		{
			Base->Direction = static_cast<EContextObjectDirection>(ClaireonChooserHelpers::ParseDirection(DirStr));
			bChanged = true;
		}
	}

	if (bChanged)
	{
		if (!ClaireonProxyTableHelpers::SaveProxyAsset(ProxyAsset, Error))
		{
			return MakeErrorResult(Error);
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), ProxyAsset->GetPathName());
	Data->SetBoolField(TEXT("changed"), bChanged);
	Data->SetStringField(TEXT("type"), ProxyAsset->Type ? ProxyAsset->Type->GetName() : TEXT("None"));
	Data->SetNumberField(TEXT("parameter_count"), ProxyAsset->ContextData.Num());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Edited ProxyAsset '%s'"), *ProxyAsset->GetName()));
}

// ============================================================================
// claireon.proxytable_add_entry
// ============================================================================

FString ClaireonTool_ProxyTableAddEntry::GetName() const { return TEXT("claireon.proxytable_add_entry"); }

FString ClaireonTool_ProxyTableAddEntry::GetDescription() const
{
	return TEXT("Add a new entry to a ProxyTable, mapping a ProxyAsset to a value.");
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

// ============================================================================
// claireon.proxytable_remove_entry
// ============================================================================

FString ClaireonTool_ProxyTableRemoveEntry::GetName() const { return TEXT("claireon.proxytable_remove_entry"); }

FString ClaireonTool_ProxyTableRemoveEntry::GetDescription() const
{
	return TEXT("Remove an entry from a ProxyTable by index.");
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

// ============================================================================
// claireon.proxytable_set_entry_value
// ============================================================================

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
