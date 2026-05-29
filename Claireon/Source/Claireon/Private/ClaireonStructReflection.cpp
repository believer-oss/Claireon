// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonStructReflection.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Text.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/Field.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/TextProperty.h"

// NOTE: deliberately avoiding #include "Engine/UserDefinedStruct.h" here — unity-build
// grouping can invalidate the transitive resolution. The "is user-defined" check uses
// UClass::GetName() string comparison instead, which doesn't need the full type.

namespace
{
	// Maximum recursion depth for value serialization. Cyclic struct references and very
	// deeply nested rows hit this cap; truncation emits the sentinel below.
	// Anon-namespace + per-file discriminator suffix to avoid unity-batched
	// collisions; do NOT use `static constexpr` at namespace scope (clang
	// strict can fire -Wunused-const-variable on partial-include TUs).
	constexpr int32 ClaireonStructReflection_MaxDepth = 32;

	// Forward declare the recursive impls so the public API can dispatch into them.
	TSharedPtr<FJsonValue>  SerializePropertyValueImpl_DataTableStructured(const FProperty* Property, const void* ValuePtr, int32 Depth, TArray<FString>& OutWarnings);
	TSharedPtr<FJsonObject> SerializeStructInstanceImpl_DataTableStructured(const UScriptStruct* Struct, const void* InstancePtr, int32 Depth, TArray<FString>& OutWarnings);

	/** Returns the shared { "_truncated": true, "depth": Depth } sentinel object used at every truncation site. */
	TSharedPtr<FJsonObject> MakeTruncatedSentinel_DataTableStructured(int32 Depth)
	{
		TSharedPtr<FJsonObject> Sentinel = MakeShared<FJsonObject>();
		Sentinel->SetBoolField(TEXT("_truncated"), true);
		Sentinel->SetNumberField(TEXT("depth"), Depth);
		return Sentinel;
	}

	/** Serialize an FText into either { text, namespace, key } or a partial form. Omits fields whose source is null/unset/empty. */
	TSharedPtr<FJsonValue> SerializeTextValue_DataTableStructured(const FText& Text)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();

		// Source string -- may be nullptr for runtime-built FText / empty culture-invariant FText.
		if (const FString* Source = FTextInspector::GetSourceString(Text))
		{
			Out->SetStringField(TEXT("text"), *Source);
		}

		// Culture-invariant FText has no namespace/key by construction; skip both lookups.
		if (!Text.IsCultureInvariant())
		{
			const TOptional<FString> NS = FTextInspector::GetNamespace(Text);
			if (NS.IsSet() && !NS.GetValue().IsEmpty())
			{
				Out->SetStringField(TEXT("namespace"), NS.GetValue());
			}

			const TOptional<FString> Key = FTextInspector::GetKey(Text);
			if (Key.IsSet() && !Key.GetValue().IsEmpty())
			{
				Out->SetStringField(TEXT("key"), Key.GetValue());
			}
		}

		return MakeShared<FJsonValueObject>(Out);
	}

	/** Helper to push one truncation warning per occurrence. */
	void EmitTruncationWarning_DataTableStructured(TArray<FString>& OutWarnings, int32 Depth, const FProperty* Property)
	{
		const FString PropName = Property ? Property->GetName() : FString(TEXT("<unknown>"));
		OutWarnings.Add(FString::Printf(TEXT("ClaireonStructReflection: truncated at depth %d in %s"), Depth, *PropName));
	}

	TSharedPtr<FJsonValue> SerializePropertyValueImpl_DataTableStructured(
		const FProperty* Property,
		const void* ValuePtr,
		int32 Depth,
		TArray<FString>& OutWarnings)
	{
		if (!Property || !ValuePtr)
		{
			return MakeShared<FJsonValueNull>();
		}

		// Bool
		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
		{
			return MakeShared<FJsonValueBoolean>(BoolProp->GetPropertyValue(ValuePtr));
		}

		// Enum (FEnumProperty) -- always { value, name }
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
		{
			UEnum* Enum = EnumProp->GetEnum();
			const FNumericProperty* UnderlyingProp = EnumProp->GetUnderlyingProperty();
			const int64 IntValue = UnderlyingProp ? UnderlyingProp->GetSignedIntPropertyValue(ValuePtr) : 0;
			TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetNumberField(TEXT("value"), static_cast<double>(IntValue));
			Out->SetStringField(TEXT("name"), Enum ? Enum->GetDisplayNameTextByValue(IntValue).ToString() : FString());
			return MakeShared<FJsonValueObject>(Out);
		}

		// Byte property -- enum case yields { value, name }, plain byte yields a number.
		if (const FByteProperty* ByteProp = CastField<FByteProperty>(Property))
		{
			if (ByteProp->Enum)
			{
				const int64 IntValue = static_cast<int64>(ByteProp->GetPropertyValue(ValuePtr));
				TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetNumberField(TEXT("value"), static_cast<double>(IntValue));
				Out->SetStringField(TEXT("name"), ByteProp->Enum->GetDisplayNameTextByValue(IntValue).ToString());
				return MakeShared<FJsonValueObject>(Out);
			}
			return MakeShared<FJsonValueNumber>(static_cast<double>(ByteProp->GetPropertyValue(ValuePtr)));
		}

		// Integer types
		if (const FIntProperty* IntProp = CastField<FIntProperty>(Property))
		{
			return MakeShared<FJsonValueNumber>(static_cast<double>(IntProp->GetPropertyValue(ValuePtr)));
		}
		if (const FInt64Property* Int64Prop = CastField<FInt64Property>(Property))
		{
			return MakeShared<FJsonValueNumber>(static_cast<double>(Int64Prop->GetPropertyValue(ValuePtr)));
		}
		if (const FUInt32Property* UInt32Prop = CastField<FUInt32Property>(Property))
		{
			return MakeShared<FJsonValueNumber>(static_cast<double>(UInt32Prop->GetPropertyValue(ValuePtr)));
		}

		// Float / Double
		if (const FFloatProperty* FloatProp = CastField<FFloatProperty>(Property))
		{
			return MakeShared<FJsonValueNumber>(static_cast<double>(FloatProp->GetPropertyValue(ValuePtr)));
		}
		if (const FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Property))
		{
			return MakeShared<FJsonValueNumber>(DoubleProp->GetPropertyValue(ValuePtr));
		}

		// String / Name
		if (const FStrProperty* StrProp = CastField<FStrProperty>(Property))
		{
			return MakeShared<FJsonValueString>(StrProp->GetPropertyValue(ValuePtr));
		}
		if (const FNameProperty* NameProp = CastField<FNameProperty>(Property))
		{
			return MakeShared<FJsonValueString>(NameProp->GetPropertyValue(ValuePtr).ToString());
		}

		// Text
		if (const FTextProperty* TextProp = CastField<FTextProperty>(Property))
		{
			return SerializeTextValue_DataTableStructured(TextProp->GetPropertyValue(ValuePtr));
		}

		// Soft references (check BEFORE the generic object path so we never force-load).
		if (const FSoftObjectProperty* SoftObjProp = CastField<FSoftObjectProperty>(Property))
		{
			return MakeShared<FJsonValueString>(SoftObjProp->GetPropertyValue(ValuePtr).ToString());
		}
		if (const FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Property))
		{
			return MakeShared<FJsonValueString>(SoftClassProp->GetPropertyValue(ValuePtr).ToString());
		}

		// Hard object/class references
		if (const FObjectPropertyBase* ObjPropBase = CastField<FObjectPropertyBase>(Property))
		{
			UObject* Target = ObjPropBase->GetObjectPtrPropertyValue(ValuePtr).Get();
			if (!Target)
			{
				return MakeShared<FJsonValueNull>();
			}
			return MakeShared<FJsonValueString>(Target->GetPathName());
		}

		// Struct (recurse)
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
		{
			TSharedPtr<FJsonObject> StructJson = SerializeStructInstanceImpl_DataTableStructured(StructProp->Struct, ValuePtr, Depth + 1, OutWarnings);
			return MakeShared<FJsonValueObject>(StructJson);
		}

		// Array
		if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			FScriptArrayHelper ArrayHelper(ArrayProp, ValuePtr);
			const int32 Num = ArrayHelper.Num();
			Items.Reserve(Num);
			for (int32 i = 0; i < Num; ++i)
			{
				const void* ElemPtr = ArrayHelper.GetRawPtr(i);
				Items.Add(SerializePropertyValueImpl_DataTableStructured(ArrayProp->Inner, ElemPtr, Depth + 1, OutWarnings));
			}
			return MakeShared<FJsonValueArray>(Items);
		}

		// Set
		if (const FSetProperty* SetProp = CastField<FSetProperty>(Property))
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			FScriptSetHelper SetHelper(SetProp, ValuePtr);
			const int32 MaxIndex = SetHelper.GetMaxIndex();
			for (int32 i = 0; i < MaxIndex; ++i)
			{
				if (!SetHelper.IsValidIndex(i))
				{
					continue;
				}
				const uint8* ElemPtr = SetHelper.GetElementPtr(i);
				Items.Add(SerializePropertyValueImpl_DataTableStructured(SetProp->ElementProp, ElemPtr, Depth + 1, OutWarnings));
			}
			return MakeShared<FJsonValueArray>(Items);
		}

		// Map -- array of { key, value }
		if (const FMapProperty* MapProp = CastField<FMapProperty>(Property))
		{
			TArray<TSharedPtr<FJsonValue>> Pairs;
			FScriptMapHelper MapHelper(MapProp, ValuePtr);
			const int32 MaxIndex = MapHelper.GetMaxIndex();
			for (int32 i = 0; i < MaxIndex; ++i)
			{
				if (!MapHelper.IsValidIndex(i))
				{
					continue;
				}
				const uint8* KeyPtr = MapHelper.GetKeyPtr(i);
				const uint8* ValPtr = MapHelper.GetValuePtr(i);
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetField(TEXT("key"),   SerializePropertyValueImpl_DataTableStructured(MapProp->KeyProp,   KeyPtr, Depth + 1, OutWarnings));
				Entry->SetField(TEXT("value"), SerializePropertyValueImpl_DataTableStructured(MapProp->ValueProp, ValPtr, Depth + 1, OutWarnings));
				Pairs.Add(MakeShared<FJsonValueObject>(Entry));
			}
			return MakeShared<FJsonValueArray>(Pairs);
		}

		// Unknown -- fallback to ExportText
		{
			FString Exported;
			Property->ExportText_Direct(Exported, ValuePtr, ValuePtr, nullptr, PPF_None);
			return MakeShared<FJsonValueString>(Exported);
		}
	}

	TSharedPtr<FJsonObject> SerializeStructInstanceImpl_DataTableStructured(
		const UScriptStruct* Struct,
		const void* InstancePtr,
		int32 Depth,
		TArray<FString>& OutWarnings)
	{
		if (!Struct || !InstancePtr)
		{
			return MakeShared<FJsonObject>();
		}

		if (Depth >= ClaireonStructReflection_MaxDepth)
		{
			EmitTruncationWarning_DataTableStructured(OutWarnings, Depth, nullptr);
			return MakeTruncatedSentinel_DataTableStructured(Depth);
		}

		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		TMap<FString, int32> EmittedKeyCounters;

		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			const FProperty* Prop = *It;
			if (!Prop)
			{
				continue;
			}

			const FString BaseKey = ClaireonStructReflection::GetFriendlyPropertyName(Prop);
			FString EmitKey = BaseKey;

			int32* ExistingCounter = EmittedKeyCounters.Find(BaseKey);
			if (ExistingCounter)
			{
				// Collision -- next occurrence gets a #<n> suffix. First duplicate is #2.
				*ExistingCounter += 1;
				const int32 SuffixIndex = *ExistingCounter;
				EmitKey = FString::Printf(TEXT("%s#%d"), *BaseKey, SuffixIndex);
				UE_LOG(LogClaireon, Warning,
					TEXT("ClaireonStructReflection: friendly-name collision on '%s'; emitting suffixed key '%s' in struct '%s'"),
					*BaseKey, *EmitKey, *Struct->GetName());
			}
			else
			{
				EmittedKeyCounters.Add(BaseKey, 1);
			}

			const void* FieldPtr = Prop->ContainerPtrToValuePtr<void>(InstancePtr);
			TSharedPtr<FJsonValue> FieldVal = SerializePropertyValueImpl_DataTableStructured(Prop, FieldPtr, Depth + 1, OutWarnings);
			Out->SetField(EmitKey, FieldVal);
		}

		return Out;
	}
}

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

	TSharedPtr<FJsonValue> SerializePropertyValue(const FProperty* Property, const void* ValuePtr)
	{
		TArray<FString> Warnings;
		return SerializePropertyValueImpl_DataTableStructured(Property, ValuePtr, /*Depth=*/0, Warnings);
	}

	TSharedPtr<FJsonObject> SerializeStructInstance(const UScriptStruct* Struct, const void* InstancePtr)
	{
		TArray<FString> Warnings;
		return SerializeStructInstanceImpl_DataTableStructured(Struct, InstancePtr, /*Depth=*/0, Warnings);
	}

	TSharedPtr<FJsonObject> SerializeStructInstance(
		const UScriptStruct* Struct,
		const void* InstancePtr,
		TArray<FString>& OutWarnings)
	{
		return SerializeStructInstanceImpl_DataTableStructured(Struct, InstancePtr, /*Depth=*/0, OutWarnings);
	}
}
