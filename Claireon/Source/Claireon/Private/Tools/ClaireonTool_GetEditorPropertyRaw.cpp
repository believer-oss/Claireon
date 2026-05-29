// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_GetEditorPropertyRaw.h"
#include "ClaireonPathResolver.h"

#include "Dom/JsonObject.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

FString ClaireonTool_GetEditorPropertyRaw::GetCategory() const  { return TEXT("editor"); }
FString ClaireonTool_GetEditorPropertyRaw::GetOperation() const { return TEXT("get_editor_property_raw"); }

FString ClaireonTool_GetEditorPropertyRaw::GetDescription() const
{
	// Tolerates stale enum values that would otherwise make
	// unreal.get_editor_property raise "value out of range for enum X". Returns the
	// underlying int / FName / asset-path. No coercion through Python's TSubclassOf
	// or enum-name lookup.
	return TEXT("Read a UPROPERTY value as its raw underlying type, bypassing the Python "
				"enum/TSubclassOf coercion layer. Returns the int / FName / asset-path "
				"directly even when the value would be invalid under the property's UEnum "
				"(post-EnumRedirects state). Stateless / read-only / non-session.");
}

TSharedPtr<FJsonObject> ClaireonTool_GetEditorPropertyRaw::GetInputSchema() const
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
	Properties->SetObjectField(TEXT("object_path"),   MkProp(TEXT("string"), TEXT("UObject path (asset path, CDO path, or world-actor path).")));
	Properties->SetObjectField(TEXT("property_name"), MkProp(TEXT("string"), TEXT("Property name on the resolved object.")));

	Schema->SetObjectField(TEXT("properties"), Properties);
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("object_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("property_name")));
	Schema->SetArrayField(TEXT("required"), Required);
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_GetEditorPropertyRaw::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString ObjectPath, PropertyName;
	if (!Arguments.IsValid()
		|| !Arguments->TryGetStringField(TEXT("object_path"), ObjectPath) || ObjectPath.IsEmpty()
		|| !Arguments->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required params: object_path and property_name"));
	}

	ClaireonPathResolver::FResolveResult R = ClaireonPathResolver::Resolve(ObjectPath);
	if (!R.bSuccess)
	{
		return MakeErrorResult(R.Error);
	}

	UObject* Obj = FindObject<UObject>(nullptr, *R.ResolvedPath.Path);
	if (!Obj)
	{
		Obj = LoadObject<UObject>(nullptr, *R.ResolvedPath.Path);
	}
	if (!Obj)
	{
		// Allow class-path / CDO query.
		if (UClass* AsClass = FindObject<UClass>(nullptr, *R.ResolvedPath.Path))
		{
			Obj = AsClass->GetDefaultObject();
		}
	}
	if (!Obj)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not resolve object: %s"), *R.ResolvedPath.Path));
	}

	FProperty* Prop = Obj->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		return MakeErrorResult(FString::Printf(TEXT("Property '%s' not found on class %s"),
			*PropertyName, *Obj->GetClass()->GetName()));
	}

	const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("object_path"), R.ResolvedPath.Path);
	Data->SetStringField(TEXT("property_name"), PropertyName);
	Data->SetStringField(TEXT("type"), Prop->GetClass()->GetName());

	// Raw read: byte/int/enum -> raw integer; FName -> string; object -> path; default -> ExportText.
	if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
	{
		const uint8 V = ByteProp->GetPropertyValue(ValuePtr);
		Data->SetNumberField(TEXT("raw_value"), V);
		if (ByteProp->Enum)
		{
			Data->SetStringField(TEXT("enum_path"), ByteProp->Enum->GetPathName());
			// Try to map back to a name; on stale values this returns NAME_None or "Invalid".
			const FString Name = ByteProp->Enum->GetNameStringByValue(V);
			Data->SetStringField(TEXT("enum_name_or_invalid"), Name.IsEmpty() ? TEXT("(stale-value)") : Name);
		}
	}
	else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
	{
		const int64 V = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
		Data->SetNumberField(TEXT("raw_value"), static_cast<double>(V));
		if (UEnum* E = EnumProp->GetEnum())
		{
			Data->SetStringField(TEXT("enum_path"), E->GetPathName());
			const FString Name = E->GetNameStringByValue(V);
			Data->SetStringField(TEXT("enum_name_or_invalid"), Name.IsEmpty() ? TEXT("(stale-value)") : Name);
		}
	}
	else if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
	{
		Data->SetStringField(TEXT("raw_value"), NameProp->GetPropertyValue(ValuePtr).ToString());
	}
	else if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
	{
		UObject* Linked = ObjProp->GetObjectPropertyValue(ValuePtr);
		Data->SetStringField(TEXT("raw_value"), Linked ? Linked->GetPathName() : FString(TEXT("None")));
	}
	else if (Prop->IsA<FIntProperty>() || Prop->IsA<FInt64Property>() || Prop->IsA<FUInt32Property>())
	{
		FString S;
		Prop->ExportText_Direct(S, ValuePtr, ValuePtr, nullptr, PPF_None);
		Data->SetNumberField(TEXT("raw_value"), FCString::Atod(*S));
	}
	else
	{
		FString S;
		Prop->ExportText_Direct(S, ValuePtr, ValuePtr, nullptr, PPF_None);
		Data->SetStringField(TEXT("raw_value"), S);
	}

	return MakeSuccessResult(Data, FString::Printf(TEXT("Read %s.%s (raw)"), *Obj->GetName(), *PropertyName));
}
