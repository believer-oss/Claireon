// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonStructReflection.h"
#include "ClaireonBlueprintHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/Field.h"
#include "UObject/Package.h"

// NOTE: deliberately avoiding #include "Engine/UserDefinedStruct.h" here — unity-build
// grouping can invalidate the transitive resolution. The "is user-defined" check uses
// UClass::GetName() string comparison instead, which doesn't need the full type.

namespace ClaireonStructReflection
{
	UScriptStruct* ResolveStructPath(const FString& Path, FString& OutError)
	{
		if (Path.IsEmpty())
		{
			OutError = TEXT("Empty struct path");
			return nullptr;
		}

		// 1) Direct lookup as a loaded/native struct
		if (UScriptStruct* Found = FindObject<UScriptStruct>(nullptr, *Path))
		{
			return Found;
		}

		// 2) Try loading the asset (BP user-defined structs are UUserDefinedStruct, a subclass of UScriptStruct)
		if (UScriptStruct* Loaded = LoadObject<UScriptStruct>(nullptr, *Path))
		{
			return Loaded;
		}

		// 3) BP asset path without `.AssetName` suffix — infer asset name from last path segment
		if (Path.StartsWith(TEXT("/Game/")) && !Path.Contains(TEXT(".")))
		{
			int32 LastSlash = INDEX_NONE;
			if (Path.FindLastChar(TEXT('/'), LastSlash))
			{
				const FString AssetName = Path.Mid(LastSlash + 1);
				const FString Fully = Path + TEXT(".") + AssetName;
				if (UScriptStruct* Loaded2 = LoadObject<UScriptStruct>(nullptr, *Fully))
				{
					return Loaded2;
				}
			}
		}

		// 4) Bare struct name (best-effort) — must be already loaded
		if (!Path.Contains(TEXT("/")) && !Path.Contains(TEXT(".")))
		{
			if (UScriptStruct* Bare = FindFirstObject<UScriptStruct>(*Path, EFindFirstObjectOptions::NativeFirst))
			{
				return Bare;
			}
		}

		OutError = FString::Printf(TEXT("Could not resolve struct path '%s' — tried direct find, LoadObject, BP asset name inference, and bare-name lookup"), *Path);
		return nullptr;
	}

	FString ClassifyProperty(const FProperty* Property)
	{
		if (!Property) return TEXT("Unknown");

		if (CastField<FBoolProperty>(Property)) return TEXT("Bool");
		if (CastField<FByteProperty>(Property))
		{
			const FByteProperty* BP = CastField<FByteProperty>(Property);
			return BP && BP->Enum ? TEXT("Enum") : TEXT("Int");
		}
		if (CastField<FEnumProperty>(Property)) return TEXT("Enum");
		if (CastField<FIntProperty>(Property)) return TEXT("Int");
		if (CastField<FInt64Property>(Property)) return TEXT("Int");
		if (CastField<FUInt32Property>(Property)) return TEXT("Int");
		if (CastField<FFloatProperty>(Property)) return TEXT("Float");
		if (CastField<FDoubleProperty>(Property)) return TEXT("Double");
		if (CastField<FStrProperty>(Property)) return TEXT("String");
		if (CastField<FNameProperty>(Property)) return TEXT("Name");
		if (CastField<FTextProperty>(Property)) return TEXT("Text");

		if (CastField<FArrayProperty>(Property)) return TEXT("Array");
		if (CastField<FSetProperty>(Property)) return TEXT("Set");
		if (CastField<FMapProperty>(Property)) return TEXT("Map");

		if (CastField<FSoftObjectProperty>(Property)) return TEXT("SoftObject");
		if (CastField<FSoftClassProperty>(Property)) return TEXT("SoftClass");
		if (CastField<FClassProperty>(Property)) return TEXT("Class");
		if (CastField<FObjectProperty>(Property)) return TEXT("Object");
		if (CastField<FStructProperty>(Property)) return TEXT("Struct");

		return TEXT("Unknown");
	}

	FString GetPropertySubtypePath(const FProperty* Property)
	{
		if (!Property) return FString();

		if (const FStructProperty* SP = CastField<FStructProperty>(Property))
		{
			return SP->Struct ? SP->Struct->GetPathName() : FString();
		}
		if (const FObjectProperty* OP = CastField<FObjectProperty>(Property))
		{
			return OP->PropertyClass ? OP->PropertyClass->GetPathName() : FString();
		}
		if (const FClassProperty* CP = CastField<FClassProperty>(Property))
		{
			return CP->MetaClass ? CP->MetaClass->GetPathName() : FString();
		}
		if (const FSoftObjectProperty* SOP = CastField<FSoftObjectProperty>(Property))
		{
			return SOP->PropertyClass ? SOP->PropertyClass->GetPathName() : FString();
		}
		if (const FSoftClassProperty* SCP = CastField<FSoftClassProperty>(Property))
		{
			return SCP->MetaClass ? SCP->MetaClass->GetPathName() : FString();
		}
		if (const FEnumProperty* EP = CastField<FEnumProperty>(Property))
		{
			return EP->GetEnum() ? EP->GetEnum()->GetPathName() : FString();
		}
		if (const FByteProperty* BP = CastField<FByteProperty>(Property))
		{
			return BP->Enum ? BP->Enum->GetPathName() : FString();
		}
		return FString();
	}

	FString GetPropertyInnerTypePath(const FProperty* Property)
	{
		if (const FArrayProperty* AP = CastField<FArrayProperty>(Property))
		{
			return GetPropertySubtypePath(AP->Inner);
		}
		if (const FSetProperty* SP = CastField<FSetProperty>(Property))
		{
			return GetPropertySubtypePath(SP->ElementProp);
		}
		if (const FMapProperty* MP = CastField<FMapProperty>(Property))
		{
			// Map inner type = value type (for schema inspection); key type is reported in subtype path.
			return GetPropertySubtypePath(MP->ValueProp);
		}
		return FString();
	}

	FString GetFriendlyPropertyName(const FProperty* Property)
	{
		if (!Property) return FString();

		// GetAuthoredName strips the BP user-defined struct's GUID suffix automatically
		const FString Authored = Property->GetAuthoredName();
		return Authored.IsEmpty() ? Property->GetName() : Authored;
	}

	bool GetPropertyDefaultValue(UScriptStruct* OwnerStruct, const FProperty* Property, FString& OutValue)
	{
		if (!OwnerStruct || !Property)
		{
			return false;
		}

		const int32 StructSize = OwnerStruct->GetStructureSize();
		if (StructSize <= 0) return false;

		TArray<uint8> Temp;
		Temp.SetNumUninitialized(StructSize);
		OwnerStruct->InitializeStruct(Temp.GetData());

		const void* PropAddr = Property->ContainerPtrToValuePtr<void>(Temp.GetData());
		if (!PropAddr)
		{
			OwnerStruct->DestroyStruct(Temp.GetData());
			return false;
		}

		Property->ExportText_Direct(OutValue, PropAddr, PropAddr, nullptr, PPF_None);
		OwnerStruct->DestroyStruct(Temp.GetData());
		return true;
	}

	TArray<FString> FormatPropertyFlags(uint64 PropertyFlags)
	{
		// Reuse the blueprint helper's implementation so flag naming stays consistent across tools.
		return ClaireonBlueprintHelpers::FormatPropertyFlags(PropertyFlags);
	}

	TSharedPtr<FJsonObject> SerializeProperty(
		UScriptStruct* OwnerStruct,
		const FProperty* Property,
		bool bIncludeDefaults,
		bool bIncludeMetadata)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		if (!Property) return Entry;

		Entry->SetStringField(TEXT("name"), Property->GetName());
		Entry->SetStringField(TEXT("friendly_name"), GetFriendlyPropertyName(Property));
		Entry->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
		Entry->SetStringField(TEXT("kind"), ClassifyProperty(Property));

		const FString SubtypePath = GetPropertySubtypePath(Property);
		if (!SubtypePath.IsEmpty())
		{
			Entry->SetStringField(TEXT("subtype_path"), SubtypePath);
		}

		const FString InnerPath = GetPropertyInnerTypePath(Property);
		if (!InnerPath.IsEmpty())
		{
			Entry->SetStringField(TEXT("inner_type_path"), InnerPath);
		}

		// Flags
		{
			const TArray<FString> Flags = FormatPropertyFlags(Property->PropertyFlags);
			TArray<TSharedPtr<FJsonValue>> FlagJson;
			FlagJson.Reserve(Flags.Num());
			for (const FString& F : Flags)
			{
				FlagJson.Add(MakeShared<FJsonValueString>(F));
			}
			Entry->SetArrayField(TEXT("flags"), FlagJson);
		}

		if (bIncludeMetadata)
		{
			const TMap<FName, FString>* MetaMap = Property->GetMetaDataMap();
			if (MetaMap && MetaMap->Num() > 0)
			{
				TSharedPtr<FJsonObject> MetaJson = MakeShared<FJsonObject>();
				for (const auto& Pair : *MetaMap)
				{
					MetaJson->SetStringField(Pair.Key.ToString(), Pair.Value);
				}
				Entry->SetObjectField(TEXT("metadata"), MetaJson);
			}
		}

		if (bIncludeDefaults && OwnerStruct)
		{
			FString DefaultValue;
			if (GetPropertyDefaultValue(OwnerStruct, Property, DefaultValue))
			{
				Entry->SetStringField(TEXT("default_value"), DefaultValue);
			}
		}

		return Entry;
	}

	TSharedPtr<FJsonObject> SerializeStructSchema(
		UScriptStruct* ScriptStruct,
		bool bIncludeDefaults,
		bool bIncludeMetadata)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!ScriptStruct) return Out;

		// Identify BP user-defined structs by class name (avoids header dep; see include block note)
		const bool bIsUserDefined = ScriptStruct->GetClass() && ScriptStruct->GetClass()->GetName() == TEXT("UserDefinedStruct");
		Out->SetStringField(TEXT("kind"), bIsUserDefined ? TEXT("Blueprint") : TEXT("Native"));
		Out->SetStringField(TEXT("name"), ScriptStruct->GetName());
		Out->SetStringField(TEXT("path"), ScriptStruct->GetPathName());
		Out->SetStringField(TEXT("cpp_type"), FString::Printf(TEXT("F%s"), *ScriptStruct->GetName()));

		if (UPackage* Pkg = ScriptStruct->GetPackage())
		{
			Out->SetStringField(TEXT("module"), Pkg->GetName());
		}

		if (UScriptStruct* Super = Cast<UScriptStruct>(ScriptStruct->GetSuperStruct()))
		{
			Out->SetStringField(TEXT("super_path"), Super->GetPathName());
		}

		Out->SetNumberField(TEXT("size_bytes"), ScriptStruct->GetStructureSize());

		TArray<TSharedPtr<FJsonValue>> Fields;
		for (TFieldIterator<FProperty> It(ScriptStruct); It; ++It)
		{
			TSharedPtr<FJsonObject> FieldObj = SerializeProperty(ScriptStruct, *It, bIncludeDefaults, bIncludeMetadata);
			Fields.Add(MakeShared<FJsonValueObject>(FieldObj));
		}
		Out->SetArrayField(TEXT("fields"), Fields);
		Out->SetNumberField(TEXT("field_count"), Fields.Num());

		return Out;
	}
}
