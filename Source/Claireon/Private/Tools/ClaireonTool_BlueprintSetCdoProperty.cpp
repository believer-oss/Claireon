// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_BlueprintSetCdoProperty.h"
#include "ClaireonLog.h"
#include "ClaireonPathResolver.h"

#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

FString ClaireonTool_BlueprintSetCdoProperty::GetCategory() const  { return TEXT("blueprint"); }
FString ClaireonTool_BlueprintSetCdoProperty::GetOperation() const { return TEXT("set_cdo_property"); }

FString ClaireonTool_BlueprintSetCdoProperty::GetDescription() const
{
	// Companion workaround for the unreal Python TSubclassOf binding bug. The
	// Python bridge raises "Cannot nativize 'BlueprintGeneratedClass' as 'Class'" when
	// set_editor_property hits a TSubclassOf<T> property; this tool sidesteps the bridge
	// by writing through FObjectProperty / FClassProperty directly.
	return TEXT("Write a UPROPERTY on a Blueprint's Class Default Object via FProperty "
				"reflection. Use for TSubclassOf<T> / UClass* properties that intermittently "
				"fail through unreal.set_editor_property's TSubclassOf binding. value_kind "
				"selects auto / class / object / text. Marks the Blueprint dirty and "
				"compiles to propagate to the generated class. Refuses to run while PIE "
				"is active.");
}

TSharedPtr<FJsonObject> ClaireonTool_BlueprintSetCdoProperty::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	auto MkProp = [](const TCHAR* Type, const TCHAR* Desc) {
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), Type);
		P->SetStringField(TEXT("description"), Desc);
		return P;
	};
	Properties->SetObjectField(TEXT("asset_path"),    MkProp(TEXT("string"), TEXT("Path to the UBlueprint asset (e.g. /Game/Foo/BP_Bar).")));
	Properties->SetObjectField(TEXT("property_name"), MkProp(TEXT("string"), TEXT("Property name on the BP's CDO (case-insensitive).")));
	Properties->SetObjectField(TEXT("value"),         MkProp(TEXT("string"), TEXT("New value. Interpretation depends on value_kind.")));
	Properties->SetObjectField(TEXT("value_kind"),    MkProp(TEXT("string"), TEXT("auto (default) | class | object | text.")));

	Schema->SetObjectField(TEXT("properties"), Properties);
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("property_name")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("value")));
	Schema->SetArrayField(TEXT("required"), Required);
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_BlueprintSetCdoProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Missing arguments"));
	}
	FString AssetPath, PropertyName, Value, ValueKind = TEXT("auto");
	if (!Arguments->TryGetStringField(TEXT("asset_path"),    AssetPath)    || AssetPath.IsEmpty())    { return MakeErrorResult(TEXT("Missing: asset_path")); }
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty()) { return MakeErrorResult(TEXT("Missing: property_name")); }
	if (!Arguments->TryGetStringField(TEXT("value"),         Value))                                   { return MakeErrorResult(TEXT("Missing: value")); }
	Arguments->TryGetStringField(TEXT("value_kind"), ValueKind);
	ValueKind = ValueKind.ToLower();

	ClaireonPathResolver::FResolveResult R = ClaireonPathResolver::Resolve(AssetPath);
	if (!R.bSuccess)
	{
		return MakeErrorResult(R.Error);
	}

	UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *R.ResolvedPath.Path);
	if (!BP)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not load UBlueprint: %s"), *R.ResolvedPath.Path));
	}
	UClass* GenClass = BP->GeneratedClass;
	if (!GenClass)
	{
		return MakeErrorResult(TEXT("Blueprint has no GeneratedClass (compile first?)"));
	}
	UObject* CDO = GenClass->GetDefaultObject();
	if (!CDO)
	{
		return MakeErrorResult(TEXT("Blueprint has no CDO"));
	}

	FProperty* Prop = GenClass->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		return MakeErrorResult(FString::Printf(TEXT("Property '%s' not found on %s"),
			*PropertyName, *GenClass->GetName()));
	}

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Blueprint CDO Property")));
	CDO->Modify();

	FString AppliedKind = ValueKind;
	bool bOk = false;

	auto WriteAsClass = [&]() -> bool
	{
		FClassProperty* CP = CastField<FClassProperty>(Prop);
		FObjectProperty* OP = CastField<FObjectProperty>(Prop);
		if (!CP && !OP)
		{
			return false;
		}
		UClass* Resolved = FindObject<UClass>(nullptr, *Value);
		if (!Resolved)
		{
			Resolved = LoadClass<UObject>(nullptr, *Value);
		}
		if (!Resolved)
		{
			return false;
		}
		// FClassProperty is a subclass of FObjectProperty -- shared setter works.
		(CP ? CP : OP)->SetObjectPropertyValue(ValuePtr, Resolved);
		return true;
	};

	auto WriteAsObject = [&]() -> bool
	{
		FObjectProperty* OP = CastField<FObjectProperty>(Prop);
		if (!OP)
		{
			return false;
		}
		UObject* Resolved = FindObject<UObject>(nullptr, *Value);
		if (!Resolved)
		{
			Resolved = LoadObject<UObject>(nullptr, *Value);
		}
		if (!Resolved)
		{
			return false;
		}
		OP->SetObjectPropertyValue(ValuePtr, Resolved);
		return true;
	};

	auto WriteAsText = [&]() -> bool
	{
		const TCHAR* TextBuffer = *Value;
		return Prop->ImportText_Direct(TextBuffer, ValuePtr, CDO, PPF_None) != nullptr;
	};

	if (ValueKind == TEXT("class"))
	{
		bOk = WriteAsClass();
	}
	else if (ValueKind == TEXT("object"))
	{
		bOk = WriteAsObject();
	}
	else if (ValueKind == TEXT("text"))
	{
		bOk = WriteAsText();
	}
	else
	{
		// auto: class > object > text by property type.
		if (CastField<FClassProperty>(Prop))
		{
			bOk = WriteAsClass();
			AppliedKind = TEXT("class");
		}
		else if (FObjectProperty* OP = CastField<FObjectProperty>(Prop))
		{
			// TSubclassOf is implemented as FClassProperty (a child of FObjectProperty
			// whose Class is UClass). If we landed here, the property is a plain object
			// reference; LoadObject is correct.
			if (OP->PropertyClass && OP->PropertyClass->IsChildOf(UClass::StaticClass()))
			{
				bOk = WriteAsClass();
				AppliedKind = TEXT("class");
			}
			else
			{
				bOk = WriteAsObject();
				AppliedKind = TEXT("object");
			}
		}
		else
		{
			bOk = WriteAsText();
			AppliedKind = TEXT("text");
		}
	}

	if (!bOk)
	{
		Transaction.Cancel();
		return MakeErrorResult(FString::Printf(
			TEXT("Failed to assign value '%s' as kind='%s' to property '%s' (type=%s)"),
			*Value, *AppliedKind, *PropertyName, *Prop->GetClass()->GetName()));
	}

	// Propagate to instances and recompile.
	FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), R.ResolvedPath.Path);
	Data->SetStringField(TEXT("property_name"), PropertyName);
	Data->SetStringField(TEXT("value"), Value);
	Data->SetStringField(TEXT("applied_kind"), AppliedKind);

	return MakeSuccessResult(Data, FString::Printf(
		TEXT("Set %s.CDO.%s = %s (kind=%s)"), *BP->GetName(), *PropertyName, *Value, *AppliedKind));
}
