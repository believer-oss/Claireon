// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPropertyUtils.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"
#include "UObject/PropertyIterator.h"
#include "Dom/JsonValue.h"

namespace ClaireonPropertyUtils
{

// ---------------------------------------------------------------------------
// Path segment: either "Name" or "Name[Index]"
// ---------------------------------------------------------------------------
struct FPathSegment
{
	FString Name;
	int32 ArrayIndex = INDEX_NONE;
	bool IsArrayAccess() const { return ArrayIndex != INDEX_NONE; }
};

bool ParsePathSegments(const FString& PropertyPath, TArray<FPathSegment>& OutSegments, FString& OutError)
{
	TArray<FString> DotParts;
	PropertyPath.ParseIntoArray(DotParts, TEXT("."));

	if (DotParts.IsEmpty())
	{
		OutError = TEXT("Empty property path");
		return false;
	}

	for (const FString& Part : DotParts)
	{
		FPathSegment Seg;

		int32 BracketPos;
		if (Part.FindChar(TEXT('['), BracketPos))
		{
			Seg.Name = Part.Left(BracketPos);
			// Extract index between [ and ]
			int32 CloseBracket;
			if (!Part.FindChar(TEXT(']'), CloseBracket) || CloseBracket <= BracketPos + 1)
			{
				OutError = FString::Printf(TEXT("Malformed array index in '%s'"), *Part);
				return false;
			}
			FString IndexStr = Part.Mid(BracketPos + 1, CloseBracket - BracketPos - 1);
			if (!IndexStr.IsNumeric())
			{
				OutError = FString::Printf(TEXT("Non-numeric array index '%s' in '%s'"), *IndexStr, *Part);
				return false;
			}
			Seg.ArrayIndex = FCString::Atoi(*IndexStr);
		}
		else
		{
			Seg.Name = Part;
		}

		if (Seg.Name.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Empty property name in segment '%s'"), *Part);
			return false;
		}

		OutSegments.Add(MoveTemp(Seg));
	}

	return true;
}

// ---------------------------------------------------------------------------
// Resolve a property path to a (FProperty*, void*) pair
// ---------------------------------------------------------------------------
bool ResolvePath(
	UStruct* Struct,
	void* Container,
	const TArray<FPathSegment>& Segments,
	FProperty*& OutProperty,
	void*& OutContainer,
	FString& OutError)
{
	UStruct* CurrentStruct = Struct;
	void* CurrentContainer = Container;

	for (int32 i = 0; i < Segments.Num(); ++i)
	{
		const FPathSegment& Seg = Segments[i];
		const bool bIsLast = (i == Segments.Num() - 1);

		FProperty* Prop = CurrentStruct->FindPropertyByName(FName(*Seg.Name));
		if (!Prop)
		{
			OutError = FString::Printf(TEXT("Property '%s' not found on '%s'"), *Seg.Name, *CurrentStruct->GetName());
			return false;
		}

		if (Seg.IsArrayAccess())
		{
			// Must be an array property
			FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
			if (!ArrayProp)
			{
				OutError = FString::Printf(TEXT("'%s' is not an array property"), *Seg.Name);
				return false;
			}

			FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(CurrentContainer));
			if (!ArrayHelper.IsValidIndex(Seg.ArrayIndex))
			{
				OutError = FString::Printf(TEXT("Array index %d out of bounds (size %d) for '%s'"), Seg.ArrayIndex, ArrayHelper.Num(), *Seg.Name);
				return false;
			}

			void* ElementPtr = ArrayHelper.GetRawPtr(Seg.ArrayIndex);

			if (bIsLast)
			{
				// The target is the array element itself — use inner property
				OutProperty = ArrayProp->Inner;
				OutContainer = ElementPtr;
				return true;
			}

			// Navigate into the element
			FProperty* Inner = ArrayProp->Inner;
			if (FStructProperty* InnerStruct = CastField<FStructProperty>(Inner))
			{
				CurrentStruct = InnerStruct->Struct;
				CurrentContainer = ElementPtr;
			}
			else if (FObjectProperty* InnerObj = CastField<FObjectProperty>(Inner))
			{
				UObject* Obj = InnerObj->GetObjectPropertyValue(ElementPtr);
				if (!Obj)
				{
					OutError = FString::Printf(TEXT("Null object at '%s[%d]'"), *Seg.Name, Seg.ArrayIndex);
					return false;
				}
				CurrentStruct = Obj->GetClass();
				CurrentContainer = Obj;
			}
			else
			{
				OutError = FString::Printf(TEXT("Cannot navigate into non-struct/non-object array element '%s[%d]'"), *Seg.Name, Seg.ArrayIndex);
				return false;
			}
		}
		else if (bIsLast)
		{
			// Final segment — this is the target
			OutProperty = Prop;
			OutContainer = CurrentContainer;
			return true;
		}
		else
		{
			// Intermediate segment — navigate into it
			if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			{
				CurrentContainer = StructProp->ContainerPtrToValuePtr<void>(CurrentContainer);
				CurrentStruct = StructProp->Struct;
			}
			else if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
			{
				UObject* Obj = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(CurrentContainer));
				if (!Obj)
				{
					OutError = FString::Printf(TEXT("Null object at '%s'"), *Seg.Name);
					return false;
				}
				CurrentStruct = Obj->GetClass();
				CurrentContainer = Obj;
			}
			else
			{
				OutError = FString::Printf(TEXT("Cannot navigate through non-struct/non-object property '%s'"), *Seg.Name);
				return false;
			}
		}
	}

	OutError = TEXT("Failed to resolve property path");
	return false;
}

// ---------------------------------------------------------------------------
// Resolve path for array operations (returns the FArrayProperty and its container)
// ---------------------------------------------------------------------------
bool ResolveArrayPath(
	UObject* Object,
	const FString& ArrayPath,
	FArrayProperty*& OutArrayProp,
	void*& OutContainer,
	FString& OutError)
{
	TArray<FPathSegment> Segments;
	if (!ParsePathSegments(ArrayPath, Segments, OutError))
	{
		return false;
	}

	// If path is a single name, find it directly
	if (Segments.Num() == 1 && !Segments[0].IsArrayAccess())
	{
		FProperty* Prop = Object->GetClass()->FindPropertyByName(FName(*Segments[0].Name));
		if (!Prop)
		{
			OutError = FString::Printf(TEXT("Property '%s' not found"), *Segments[0].Name);
			return false;
		}
		OutArrayProp = CastField<FArrayProperty>(Prop);
		if (!OutArrayProp)
		{
			OutError = FString::Printf(TEXT("'%s' is not an array property"), *Segments[0].Name);
			return false;
		}
		OutContainer = Object;
		return true;
	}

	// Multi-segment: resolve all but last, then get the array property
	FPathSegment LastSeg = Segments.Last();
	TArray<FPathSegment> ParentSegments(Segments);
	ParentSegments.Pop();

	FProperty* ParentProp = nullptr;
	void* ParentContainer = nullptr;

	if (ParentSegments.IsEmpty())
	{
		ParentContainer = Object;
	}
	else
	{
		// Resolve parent path — add the last segment name as a final accessor
		// Actually, we need to resolve to the container of the array
		if (!ResolvePath(Object->GetClass(), Object, ParentSegments, ParentProp, ParentContainer, OutError))
		{
			return false;
		}
		// ParentContainer/ParentProp point to the resolved parent
		// We need to get the container that holds the array
		if (FStructProperty* SP = CastField<FStructProperty>(ParentProp))
		{
			ParentContainer = SP->ContainerPtrToValuePtr<void>(ParentContainer);
		}
		else if (FObjectProperty* OP = CastField<FObjectProperty>(ParentProp))
		{
			UObject* Obj = OP->GetObjectPropertyValue(OP->ContainerPtrToValuePtr<void>(ParentContainer));
			if (!Obj)
			{
				OutError = TEXT("Null object in array path resolution");
				return false;
			}
			ParentContainer = Obj;
		}
	}

	// Resolve the full path as-is and check the final property
	FProperty* FinalProp = nullptr;
	void* FinalContainer = nullptr;
	if (!ResolvePath(Object->GetClass(), Object, Segments, FinalProp, FinalContainer, OutError))
	{
		return false;
	}

	// FinalProp should be the array property found at the path
	OutArrayProp = CastField<FArrayProperty>(FinalProp);
	if (!OutArrayProp)
	{
		OutError = FString::Printf(TEXT("'%s' is not an array property"), *ArrayPath);
		return false;
	}
	OutContainer = FinalContainer;
	return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

FString ReadPropertyByPath(UObject* Object, const FString& PropertyPath, FString& OutError)
{
	if (!Object)
	{
		OutError = TEXT("Null object");
		return FString();
	}

	TArray<FPathSegment> Segments;
	if (!ParsePathSegments(PropertyPath, Segments, OutError))
	{
		return FString();
	}

	FProperty* Prop = nullptr;
	void* Container = nullptr;
	if (!ResolvePath(Object->GetClass(), Object, Segments, Prop, Container, OutError))
	{
		return FString();
	}

	FString Value;
	const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Container);

	// For array elements where Container IS the element, ValuePtr is Container itself
	// Check: if the last segment was an array access, Container points to the element data
	if (Segments.Last().IsArrayAccess())
	{
		ValuePtr = Container;
	}

	Prop->ExportText_Direct(Value, ValuePtr, ValuePtr, nullptr, PPF_None);
	return Value;
}

bool WritePropertyByPath(UObject* Object, const FString& PropertyPath, const FString& Value, FString& OutError)
{
	if (!Object)
	{
		OutError = TEXT("Null object");
		return false;
	}

	TArray<FPathSegment> Segments;
	if (!ParsePathSegments(PropertyPath, Segments, OutError))
	{
		return false;
	}

	FProperty* Prop = nullptr;
	void* Container = nullptr;
	if (!ResolvePath(Object->GetClass(), Object, Segments, Prop, Container, OutError))
	{
		return false;
	}

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Container);
	if (Segments.Last().IsArrayAccess())
	{
		ValuePtr = Container;
	}

	const TCHAR* Result = Prop->ImportText_Direct(*Value, ValuePtr, Object, PPF_None);
	if (!Result)
	{
		OutError = FString::Printf(TEXT("Failed to set '%s' to '%s'"), *PropertyPath, *Value);
		return false;
	}

	return true;
}

// ---------------------------------------------------------------------------
// GetAllProperties — recursive property enumeration to JSON
// ---------------------------------------------------------------------------

TSharedPtr<FJsonObject> EnumerateProperties(UStruct* Struct, const void* Container, UObject* OwnerObject, const FString& Filter, int32 Depth);

TSharedPtr<FJsonValue> PropertyToJsonValue(FProperty* Prop, const void* ValuePtr, UObject* OwnerObject, int32 Depth)
{
	if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		if (Depth > 0)
		{
			TSharedPtr<FJsonObject> StructObj = EnumerateProperties(StructProp->Struct, ValuePtr, OwnerObject, TEXT(""), Depth - 1);
			return MakeShared<FJsonValueObject>(StructObj);
		}
		// Depth exhausted — export as string
		FString Value;
		Prop->ExportText_Direct(Value, ValuePtr, ValuePtr, nullptr, PPF_None);
		return MakeShared<FJsonValueString>(Value);
	}

	if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
	{
		UObject* Obj = ObjProp->GetObjectPropertyValue(ValuePtr);
		if (!Obj)
		{
			return MakeShared<FJsonValueNull>();
		}
		if (Depth > 0 && (Obj->HasAnyFlags(RF_DefaultSubObject) || ObjProp->HasAnyPropertyFlags(CPF_InstancedReference)))
		{
			// Instanced sub-object — recurse into it
			TSharedPtr<FJsonObject> SubObj = MakeShared<FJsonObject>();
			SubObj->SetStringField(TEXT("_class"), Obj->GetClass()->GetName());
			TSharedPtr<FJsonObject> Props = EnumerateProperties(Obj->GetClass(), Obj, Obj, TEXT(""), Depth - 1);
			for (auto& Pair : Props->Values)
			{
				SubObj->SetField(Pair.Key, Pair.Value);
			}
			return MakeShared<FJsonValueObject>(SubObj);
		}
		// Non-instanced object — return as path string
		return MakeShared<FJsonValueString>(Obj->GetPathName());
	}

	if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
	{
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		FScriptArrayHelper ArrayHelper(ArrayProp, ValuePtr);
		for (int32 i = 0; i < ArrayHelper.Num(); ++i)
		{
			const void* ElemPtr = ArrayHelper.GetRawPtr(i);
			JsonArray.Add(PropertyToJsonValue(ArrayProp->Inner, ElemPtr, OwnerObject, Depth));
		}
		return MakeShared<FJsonValueArray>(JsonArray);
	}

	if (FSoftObjectProperty* SoftObjProp = CastField<FSoftObjectProperty>(Prop))
	{
		FString Value;
		Prop->ExportText_Direct(Value, ValuePtr, ValuePtr, nullptr, PPF_None);
		return MakeShared<FJsonValueString>(Value);
	}

	if (FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Prop))
	{
		FString Value;
		Prop->ExportText_Direct(Value, ValuePtr, ValuePtr, nullptr, PPF_None);
		return MakeShared<FJsonValueString>(Value);
	}

	// Fallback: export as string
	FString Value;
	Prop->ExportText_Direct(Value, ValuePtr, ValuePtr, nullptr, PPF_None);

	// Try to return as number for numeric types
	if (Prop->IsA<FFloatProperty>() || Prop->IsA<FDoubleProperty>())
	{
		return MakeShared<FJsonValueNumber>(FCString::Atod(*Value));
	}
	if (Prop->IsA<FIntProperty>() || Prop->IsA<FInt64Property>() || Prop->IsA<FUInt32Property>())
	{
		return MakeShared<FJsonValueNumber>(FCString::Atod(*Value));
	}
	if (Prop->IsA<FBoolProperty>())
	{
		return MakeShared<FJsonValueBoolean>(Value.Equals(TEXT("True"), ESearchCase::IgnoreCase));
	}

	return MakeShared<FJsonValueString>(Value);
}

TSharedPtr<FJsonObject> EnumerateProperties(UStruct* Struct, const void* Container, UObject* OwnerObject, const FString& Filter, int32 Depth)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		FProperty* Prop = *It;

		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
		{
			continue;
		}
		if (Prop->IsA<FDelegateProperty>() || Prop->IsA<FMulticastDelegateProperty>())
		{
			continue;
		}
		if (!Filter.IsEmpty() && !Prop->GetName().Contains(Filter))
		{
			continue;
		}

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Container);
		Result->SetField(Prop->GetName(), PropertyToJsonValue(Prop, ValuePtr, OwnerObject, Depth));
	}

	return Result;
}

TSharedPtr<FJsonObject> GetAllProperties(UObject* Object, const FString& Filter, int32 Depth)
{
	if (!Object)
	{
		return MakeShared<FJsonObject>();
	}
	return EnumerateProperties(Object->GetClass(), Object, Object, Filter, Depth);
}

// ---------------------------------------------------------------------------
// Array operations
// ---------------------------------------------------------------------------

bool AddArrayElement(UObject* Object, const FString& ArrayPath, const FString& Value, FString& OutError)
{
	if (!Object)
	{
		OutError = TEXT("Null object");
		return false;
	}

	FArrayProperty* ArrayProp = nullptr;
	void* Container = nullptr;
	if (!ResolveArrayPath(Object, ArrayPath, ArrayProp, Container, OutError))
	{
		return false;
	}

	FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Container));
	int32 NewIndex = ArrayHelper.AddValue();
	void* NewElemPtr = ArrayHelper.GetRawPtr(NewIndex);

	if (!Value.IsEmpty())
	{
		const TCHAR* Result = ArrayProp->Inner->ImportText_Direct(*Value, NewElemPtr, Object, PPF_None);
		if (!Result)
		{
			// Remove the element we just added since import failed
			ArrayHelper.RemoveValues(NewIndex, 1);
			OutError = FString::Printf(TEXT("Failed to set new array element to '%s'"), *Value);
			return false;
		}
	}

	return true;
}

bool RemoveArrayElement(UObject* Object, const FString& ArrayPath, int32 Index, FString& OutError)
{
	if (!Object)
	{
		OutError = TEXT("Null object");
		return false;
	}

	FArrayProperty* ArrayProp = nullptr;
	void* Container = nullptr;
	if (!ResolveArrayPath(Object, ArrayPath, ArrayProp, Container, OutError))
	{
		return false;
	}

	FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Container));
	if (!ArrayHelper.IsValidIndex(Index))
	{
		OutError = FString::Printf(TEXT("Index %d out of bounds (size %d)"), Index, ArrayHelper.Num());
		return false;
	}

	ArrayHelper.RemoveValues(Index, 1);
	return true;
}

UObject* CreateInstancedSubObject(UObject* Outer, UClass* SubObjectClass, const FString& ArrayPath, FString& OutError)
{
	if (!Outer)
	{
		OutError = TEXT("Null outer object");
		return nullptr;
	}
	if (!SubObjectClass)
	{
		OutError = TEXT("Null sub-object class");
		return nullptr;
	}

	FArrayProperty* ArrayProp = nullptr;
	void* Container = nullptr;
	if (!ResolveArrayPath(Outer, ArrayPath, ArrayProp, Container, OutError))
	{
		return nullptr;
	}

	// Verify inner type is an object property
	FObjectProperty* InnerObjProp = CastField<FObjectProperty>(ArrayProp->Inner);
	if (!InnerObjProp)
	{
		OutError = FString::Printf(TEXT("'%s' is not an array of objects"), *ArrayPath);
		return nullptr;
	}

	// Verify the class is compatible
	if (!SubObjectClass->IsChildOf(InnerObjProp->PropertyClass))
	{
		OutError = FString::Printf(TEXT("'%s' is not a subclass of '%s'"), *SubObjectClass->GetName(), *InnerObjProp->PropertyClass->GetName());
		return nullptr;
	}

	// Create the sub-object
	UObject* NewObj = NewObject<UObject>(Outer, SubObjectClass);
	if (!NewObj)
	{
		OutError = FString::Printf(TEXT("Failed to create object of class '%s'"), *SubObjectClass->GetName());
		return nullptr;
	}

	// Add to array
	FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Container));
	int32 NewIndex = ArrayHelper.AddValue();
	InnerObjProp->SetObjectPropertyValue(ArrayHelper.GetRawPtr(NewIndex), NewObj);

	return NewObj;
}

// ---------------------------------------------------------------------------
// Inline (EditInline / Instanced) sub-object helpers
// ---------------------------------------------------------------------------

FProperty* ResolvePropertyByPath(
	UObject* Object,
	const FString& PropertyPath,
	void*& OutContainer,
	FString& OutError)
{
	OutContainer = nullptr;

	if (!Object)
	{
		OutError = TEXT("Null object");
		return nullptr;
	}

	TArray<FPathSegment> Segments;
	if (!ParsePathSegments(PropertyPath, Segments, OutError))
	{
		return nullptr;
	}

	FProperty* OutProperty = nullptr;
	if (!ResolvePath(Object->GetClass(), Object, Segments, OutProperty, OutContainer, OutError))
	{
		return nullptr;
	}

	return OutProperty;
}

UObject* CreateInstancedArrayElement(
	UObject* Outer,
	UClass* SubObjectClass,
	const FString& ArrayPath,
	FString& OutError)
{
	if (!Outer)
	{
		OutError = TEXT("Null outer object");
		return nullptr;
	}
	if (!SubObjectClass)
	{
		OutError = TEXT("Null sub-object class");
		return nullptr;
	}

	FArrayProperty* ArrayProp = nullptr;
	void* Container = nullptr;
	if (!ResolveArrayPath(Outer, ArrayPath, ArrayProp, Container, OutError))
	{
		return nullptr;
	}

	FObjectProperty* InnerObjProp = CastField<FObjectProperty>(ArrayProp->Inner);
	if (!InnerObjProp)
	{
		OutError = FString::Printf(
			TEXT("'%s' inner is not an FObjectProperty (CPF_InstancedReference required)"),
			*ArrayPath);
		return nullptr;
	}

	if ((InnerObjProp->PropertyFlags & CPF_InstancedReference) == 0)
	{
		OutError = FString::Printf(
			TEXT("'%s' is not declared UPROPERTY(Instanced) (CPF_InstancedReference required)"),
			*ArrayPath);
		return nullptr;
	}

	if (!SubObjectClass->IsChildOf(InnerObjProp->PropertyClass))
	{
		OutError = FString::Printf(
			TEXT("'%s' is not a subclass of '%s'"),
			*SubObjectClass->GetName(), *InnerObjProp->PropertyClass->GetName());
		return nullptr;
	}

	EObjectFlags Flags = RF_Transactional;
	Flags |= (Outer->GetFlags() & (RF_Public | RF_ArchetypeObject));

	UObject* NewObj = NewObject<UObject>(Outer, SubObjectClass, NAME_None, Flags);
	if (!NewObj)
	{
		OutError = FString::Printf(
			TEXT("Failed to create object of class '%s'"), *SubObjectClass->GetName());
		return nullptr;
	}

	FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Container));
	const int32 NewIndex = Helper.AddValue();
	InnerObjProp->SetObjectPropertyValue(Helper.GetRawPtr(NewIndex), NewObj);

	return NewObj;
}

UObject* SetInstancedSubObject(
	UObject* Outer,
	UClass* SubObjectClass,
	const FString& ObjectPath,
	FString& OutError)
{
	if (!Outer)
	{
		OutError = TEXT("Null outer object");
		return nullptr;
	}
	if (!SubObjectClass)
	{
		OutError = TEXT("Null sub-object class");
		return nullptr;
	}

	void* Container = nullptr;
	FProperty* Resolved = ResolvePropertyByPath(Outer, ObjectPath, Container, OutError);
	if (!Resolved)
	{
		return nullptr;
	}

	FObjectProperty* ObjectProp = CastField<FObjectProperty>(Resolved);
	if (!ObjectProp)
	{
		OutError = FString::Printf(
			TEXT("'%s' does not resolve to an FObjectProperty"), *ObjectPath);
		return nullptr;
	}

	if ((ObjectProp->PropertyFlags & CPF_InstancedReference) == 0)
	{
		OutError = FString::Printf(
			TEXT("'%s' is not declared UPROPERTY(Instanced) (CPF_InstancedReference required)"),
			*ObjectPath);
		return nullptr;
	}

	if (!SubObjectClass->IsChildOf(ObjectProp->PropertyClass))
	{
		OutError = FString::Printf(
			TEXT("'%s' is not a subclass of '%s'"),
			*SubObjectClass->GetName(), *ObjectProp->PropertyClass->GetName());
		return nullptr;
	}

	void* SlotPtr = ObjectProp->ContainerPtrToValuePtr<void>(Container);
	UObject* PrevValue = ObjectProp->GetObjectPropertyValue(SlotPtr);
	if (PrevValue)
	{
		PrevValue->MarkAsGarbage();
	}

	EObjectFlags Flags = RF_Transactional;
	Flags |= (Outer->GetFlags() & (RF_Public | RF_ArchetypeObject));

	UObject* NewObj = NewObject<UObject>(Outer, SubObjectClass, NAME_None, Flags);
	if (!NewObj)
	{
		OutError = FString::Printf(
			TEXT("Failed to create object of class '%s'"), *SubObjectClass->GetName());
		return nullptr;
	}

	ObjectProp->SetObjectPropertyValue(SlotPtr, NewObj);
	return NewObj;
}

} // namespace ClaireonPropertyUtils
