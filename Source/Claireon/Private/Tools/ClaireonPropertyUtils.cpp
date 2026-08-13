// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPropertyUtils.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"
#include "UObject/PropertyIterator.h"
#include "Dom/JsonValue.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "UObject/SoftObjectPath.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "StructUtils/InstancedStruct.h"

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
// ---------------------------------------------------------------------------
// SCS component-template fallback.
//
// On a Blueprint CDO the UPROPERTY generated for an SCS component is NULL: component
// instances are created at construction time, while the authored defaults live on the SCS
// node's ComponentTemplate. So a path like "<CDO>.MyComp.PrimaryComponentTick" failed at
// 'MyComp' even though the data plainly exists, and reaching it meant hand-walking
// SimpleConstructionScript.AllNodes -> InternalVariableName -> ComponentTemplate.
//
// Superclasses are walked so components inherited from a parent Blueprint resolve too.
// ---------------------------------------------------------------------------
static UObject* Cl625Prop_FindSCSComponentTemplate(UStruct* OwnerStruct, const FString& ComponentName)
{
	UClass* AsClass = Cast<UClass>(OwnerStruct);
	if (!AsClass)
	{
		return nullptr;
	}
	for (UClass* Cls = AsClass; Cls; Cls = Cls->GetSuperClass())
	{
		UBlueprint* Blueprint = Cast<UBlueprint>(Cls->ClassGeneratedBy);
		if (!Blueprint || !Blueprint->SimpleConstructionScript)
		{
			continue;
		}
		for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
		{
			// Match on the variable name, not the template's object name: templates are named
			// "<Var>_GEN_VARIABLE", which is not what a caller would ever write.
			if (Node && Node->ComponentTemplate
				&& Node->GetVariableName().ToString().Equals(ComponentName, ESearchCase::IgnoreCase))
			{
				return Node->ComponentTemplate;
			}
		}
	}
	return nullptr;
}

// OutOwnerObject, when non-null, receives the innermost UObject the walk
// stepped into -- the object that actually owns the leaf. It differs from the
// root whenever the path traverses a component / sub-object / SCS template
// (e.g. "MyComp.SomeField"), and a caller that needs Modify() or a change
// notification to land on the right object cannot recover it from the void*
// container alone.
bool ResolvePath(
	UStruct* Struct,
	void* Container,
	const TArray<FPathSegment>& Segments,
	FProperty*& OutProperty,
	void*& OutContainer,
	FString& OutError,
	UObject** OutOwnerObject = nullptr)
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
				if (OutOwnerObject) { *OutOwnerObject = Obj; }
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
					// A null component property on a Blueprint CDO is expected, not an error: the
					// authored defaults live on the SCS ComponentTemplate, so fall back to it.
					Obj = Cl625Prop_FindSCSComponentTemplate(CurrentStruct, Seg.Name);
				}
				if (!Obj)
				{
					OutError = FString::Printf(
						TEXT("Null object at '%s'. On a Blueprint CDO, component properties are null ")
						TEXT("until construction, and no SCS ComponentTemplate named '%s' was found either."),
						*Seg.Name, *Seg.Name);
					return false;
				}
				CurrentStruct = Obj->GetClass();
				CurrentContainer = Obj;
				if (OutOwnerObject) { *OutOwnerObject = Obj; }
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

// Recursively walk a just-imported property value and re-instance any instanced
// object reference still owned by a FOREIGN object (typically the asset the value
// text was exported from). ImportText resolves instanced references by path, so
// replaying an exported value onto a different object would otherwise leave the
// copy aliasing the SOURCE's instanced subobjects instead of owning its own.
static void ClaireonPropUtils_InstanceForeignSubobjects(FProperty* Prop, void* ValuePtr, UObject* Owner, int32 Depth)
{
	if (!Prop || !ValuePtr || !Owner || Depth > 8)
	{
		return;
	}

	if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
	{
		if (!ObjProp->HasAnyPropertyFlags(CPF_InstancedReference | CPF_PersistentInstance))
		{
			return;
		}
		UObject* Ref = ObjProp->GetObjectPropertyValue(ValuePtr);
		if (!Ref || Ref->IsIn(Owner) || Ref->IsA<UClass>() || Ref->HasAnyFlags(RF_ClassDefaultObject))
		{
			return;
		}
		FName DupName = Ref->GetFName();
		if (StaticFindObjectFast(nullptr, Owner, DupName))
		{
			DupName = MakeUniqueObjectName(Owner, Ref->GetClass(), DupName);
		}
		UObject* Dup = StaticDuplicateObject(Ref, Owner, DupName);
		if (Dup)
		{
			Dup->SetFlags(RF_Transactional);
			if (Owner->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
			{
				Dup->SetFlags(RF_ArchetypeObject);
			}
			ObjProp->SetObjectPropertyValue(ValuePtr, Dup);
		}
		return;
	}

	if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
	{
		for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
		{
			if (It->ContainsInstancedObjectProperty())
			{
				ClaireonPropUtils_InstanceForeignSubobjects(*It, It->ContainerPtrToValuePtr<void>(ValuePtr), Owner, Depth + 1);
			}
		}
		return;
	}

	if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
	{
		if (!ArrayProp->Inner->ContainsInstancedObjectProperty())
		{
			return;
		}
		FScriptArrayHelper Helper(ArrayProp, ValuePtr);
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			ClaireonPropUtils_InstanceForeignSubobjects(ArrayProp->Inner, Helper.GetRawPtr(Index), Owner, Depth + 1);
		}
		return;
	}

	if (FSetProperty* SetProp = CastField<FSetProperty>(Prop))
	{
		if (!SetProp->ElementProp->ContainsInstancedObjectProperty())
		{
			return;
		}
		FScriptSetHelper Helper(SetProp, ValuePtr);
		for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
		{
			if (Helper.IsValidIndex(Index))
			{
				ClaireonPropUtils_InstanceForeignSubobjects(SetProp->ElementProp, Helper.GetElementPtr(Index), Owner, Depth + 1);
			}
		}
		// Element identity may have changed (object pointers hash by address).
		Helper.Rehash();
		return;
	}

	if (FMapProperty* MapProp = CastField<FMapProperty>(Prop))
	{
		const bool bKeyInstanced = MapProp->KeyProp->ContainsInstancedObjectProperty();
		const bool bValueInstanced = MapProp->ValueProp->ContainsInstancedObjectProperty();
		if (!bKeyInstanced && !bValueInstanced)
		{
			return;
		}
		FScriptMapHelper Helper(MapProp, ValuePtr);
		for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
		{
			if (!Helper.IsValidIndex(Index))
			{
				continue;
			}
			if (bKeyInstanced)
			{
				ClaireonPropUtils_InstanceForeignSubobjects(MapProp->KeyProp, Helper.GetKeyPtr(Index), Owner, Depth + 1);
			}
			if (bValueInstanced)
			{
				ClaireonPropUtils_InstanceForeignSubobjects(MapProp->ValueProp, Helper.GetValuePtr(Index), Owner, Depth + 1);
			}
		}
		if (bKeyInstanced)
		{
			Helper.Rehash();
		}
	}
}

/**
 * Canonicalize a dotless asset path ('/Game/Dir/Foo') to Package.Object form
 * ('/Game/Dir/Foo.Foo') for object-reference leaf writes.
 *
 * Hard object refs do not need this -- FObjectPropertyBase::FindImportedObject
 * appends the leaf name itself. Soft refs DO: FSoftObjectPath::SetPath takes its
 * "no delimiter found" branch and stores the value package-only, leaving AssetName
 * as NAME_None. The import reports success and the reference never resolves, so the
 * write looks like it worked and silently produces a dangling soft pointer.
 *
 * Applied to hard refs too, so the canonicalize-or-reject contract is uniform and
 * testable across object-reference property kinds rather than depending on which
 * engine path happens to be forgiving.
 *
 * Returns false with OutError set when the path names no real asset. There is no
 * legitimate reason to write an object reference to a path that does not resolve,
 * so this rejects rather than guessing further.
 */
static bool ClaireonPropUtils_CanonicalizeObjectRefValue(
	const FProperty* LeafProp, const FString& Value, FString& OutValue, FString& OutError)
{
	OutValue = Value;

	const bool bObjectRefLeaf =
		LeafProp
		&& (LeafProp->IsA<FSoftObjectProperty>()
			|| LeafProp->IsA<FSoftClassProperty>()
			|| LeafProp->IsA<FObjectPropertyBase>());
	if (!bObjectRefLeaf)
	{
		return true;
	}

	// Only top-level content paths are candidates. Empty/None clears, already-dotted
	// paths, and non-path literals (e.g. a bare class name the resolver handles) pass
	// through untouched.
	if (Value.IsEmpty()
		|| Value == TEXT("None")
		|| Value.Contains(TEXT("."))
		|| !Value.StartsWith(TEXT("/")))
	{
		return true;
	}

	const FString CandidatePath = Value + TEXT(".") + FPackageName::GetShortName(Value);

	IAssetRegistry& Registry =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const FAssetData AssetData = Registry.GetAssetByObjectPath(FSoftObjectPath(CandidatePath));
	if (!AssetData.IsValid())
	{
		OutError = FString::Printf(
			TEXT("SoftObjectProperty requires Package.Object form; did you mean '%s'?"),
			*CandidatePath);
		return false;
	}

	OutValue = CandidatePath;
	return true;
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

	// An empty value on a container property means "clear". ImportText_Direct
	// rejects an empty string outright, but exporters legitimately produce one
	// for a container that is empty on the source and non-empty on the default.
	if (Value.IsEmpty() && !Segments.Last().IsArrayAccess())
	{
		if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
		{
			FScriptArrayHelper(ArrayProp, ValuePtr).EmptyValues();
			return true;
		}
		if (FSetProperty* SetProp = CastField<FSetProperty>(Prop))
		{
			FScriptSetHelper(SetProp, ValuePtr).EmptyElements();
			return true;
		}
		if (FMapProperty* MapProp = CastField<FMapProperty>(Prop))
		{
			FScriptMapHelper(MapProp, ValuePtr).EmptyValues();
			return true;
		}
	}

	// Object-reference leaves: canonicalize a dotless /Game path to Package.Object,
	// or reject. Must happen before the import -- a dotless soft path imports
	// "successfully" as a package-only reference that never resolves.
	FString EffectiveValue;
	if (!ClaireonPropUtils_CanonicalizeObjectRefValue(Prop, Value, EffectiveValue, OutError))
	{
		return false;
	}

	// Mirror a details-panel write: bracket the import with PreEditChange /
	// PostEditChangeProperty so properties with edit-time side effects take their
	// notification path (UStaticMeshComponent::StaticMesh otherwise trips the
	// NotifyIfStaticMeshChanged ensure -- its KnownStaticMesh cache only updates
	// through the notify). PostEditChangeProperty always runs once PreEditChange
	// has, even on import failure, so unregister/reregister stays paired.
	FProperty* TopLevelProp = Object->GetClass()->FindPropertyByName(FName(*Segments[0].Name));
	Object->PreEditChange(TopLevelProp);

	bool bImported = false;
	// Properties declared with a native Setter must not be written by raw
	// ImportText -- import into a scratch value and assign through the setter.
	const bool bDirectOnObject =
		Container == static_cast<void*>(Object) && !Segments.Last().IsArrayAccess();
	if (bDirectOnObject && Prop->HasSetter() && Prop->ArrayDim == 1)
	{
		void* Scratch = FMemory::Malloc(Prop->GetSize(), Prop->GetMinAlignment());
		Prop->InitializeValue(Scratch);
		bImported = Prop->ImportText_Direct(*EffectiveValue, Scratch, Object, PPF_None) != nullptr;
		if (bImported)
		{
			if (Prop->ContainsInstancedObjectProperty())
			{
				ClaireonPropUtils_InstanceForeignSubobjects(Prop, Scratch, Object, 0);
			}
			Prop->SetValue_InContainer(Object, Scratch);
		}
		Prop->DestroyValue(Scratch);
		FMemory::Free(Scratch);
	}
	else
	{
		bImported = Prop->ImportText_Direct(*EffectiveValue, ValuePtr, Object, PPF_None) != nullptr;
		if (bImported && Prop->ContainsInstancedObjectProperty())
		{
			ClaireonPropUtils_InstanceForeignSubobjects(Prop, ValuePtr, Object, 0);
		}
	}

	FPropertyChangedEvent ChangedEvent(Prop, EPropertyChangeType::ValueSet);
	Object->PostEditChangeProperty(ChangedEvent);

	if (!bImported)
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
		// P2-18: FInstancedStruct's payload lives in non-UPROPERTY members, so
		// reflection-driven recursion renders it {}. Unwrap it: _struct names
		// the wrapped type (mirroring the _class convention for instanced
		// sub-objects below) and the fields come from the wrapped struct's own
		// memory. Callers testing `if not value` on an FInstancedStruct field
		// flip from falsy to truthy with this change -- read _struct instead.
		if (StructProp->Struct == TBaseStructure<FInstancedStruct>::Get())
		{
			const FInstancedStruct* Instanced = static_cast<const FInstancedStruct*>(ValuePtr);
			TSharedPtr<FJsonObject> InstObj = MakeShared<FJsonObject>();
			if (Instanced && Instanced->IsValid())
			{
				const UScriptStruct* WrappedStruct = Instanced->GetScriptStruct();
				InstObj->SetStringField(TEXT("_struct"), WrappedStruct->GetName());
				if (Depth > 0)
				{
					TSharedPtr<FJsonObject> Fields = EnumerateProperties(
						const_cast<UScriptStruct*>(WrappedStruct), Instanced->GetMemory(), OwnerObject, TEXT(""), Depth - 1);
					for (auto& Pair : Fields->Values)
					{
						InstObj->SetField(Pair.Key, Pair.Value);
					}
				}
				else
				{
					// Depth exhausted: keep the working ExportText form so
					// max_depth=0 (the documented pre-fix workaround) still
					// yields a non-empty payload.
					FString Value;
					Prop->ExportText_Direct(Value, ValuePtr, ValuePtr, nullptr, PPF_None);
					InstObj->SetStringField(TEXT("_export"), Value);
				}
			}
			else
			{
				// An unset FInstancedStruct wraps nothing; say so explicitly.
				InstObj->SetField(TEXT("_struct"), MakeShared<FJsonValueNull>());
			}
			return MakeShared<FJsonValueObject>(InstObj);
		}

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

	// TMap support. Emit as a JSON array of {key, value} pairs to preserve key types
	// that aren't valid JSON object keys (FName/FStruct/GameplayTag keys etc.). Callers that
	// know keys are strings can post-process trivially. FScriptMapHelper walks valid pairs
	// only (skipping deleted slots).
	if (FMapProperty* MapProp = CastField<FMapProperty>(Prop))
	{
		TArray<TSharedPtr<FJsonValue>> Entries;
		FScriptMapHelper MapHelper(MapProp, ValuePtr);
		const int32 MaxIndex = MapHelper.GetMaxIndex();
		for (int32 i = 0; i < MaxIndex; ++i)
		{
			if (!MapHelper.IsValidIndex(i))
			{
				continue;
			}
			const uint8* KeyPtr   = MapHelper.GetKeyPtr(i);
			const uint8* ValuePtrM = MapHelper.GetValuePtr(i);

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetField(TEXT("key"),   PropertyToJsonValue(MapProp->KeyProp,   KeyPtr,    OwnerObject, Depth));
			Entry->SetField(TEXT("value"), PropertyToJsonValue(MapProp->ValueProp, ValuePtrM, OwnerObject, Depth));
			Entries.Add(MakeShared<FJsonValueObject>(Entry));
		}
		return MakeShared<FJsonValueArray>(Entries);
	}

	// TSet support, symmetric with TMap. Same valid-slot filter via FScriptSetHelper.
	if (FSetProperty* SetProp = CastField<FSetProperty>(Prop))
	{
		TArray<TSharedPtr<FJsonValue>> Elems;
		FScriptSetHelper SetHelper(SetProp, ValuePtr);
		const int32 MaxIndex = SetHelper.GetMaxIndex();
		for (int32 i = 0; i < MaxIndex; ++i)
		{
			if (!SetHelper.IsValidIndex(i))
			{
				continue;
			}
			const void* ElemPtr = SetHelper.GetElementPtr(i);
			Elems.Add(PropertyToJsonValue(SetProp->ElementProp, ElemPtr, OwnerObject, Depth));
		}
		return MakeShared<FJsonValueArray>(Elems);
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
	FString& OutError,
	UObject** OutOwnerObject)
{
	OutContainer = nullptr;
	if (OutOwnerObject)
	{
		// Seed with the root: a path that never hops into a sub-object leaves the
		// root as the owner, and ResolvePath only writes on a hop.
		*OutOwnerObject = Object;
	}

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
	if (!ResolvePath(Object->GetClass(), Object, Segments, OutProperty, OutContainer, OutError, OutOwnerObject))
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

	// Name the new sub-object after the UPROPERTY it fills (e.g. "SphereCenter") so
	// the saved package's archetype graph keeps the property-name => default-subobject
	// linkage that FAsyncPackage2::CreateExport relies on for cooked-client demand-load.
	// NAME_None here would auto-generate a name like "<ClassName>_0" which works in
	// the editor but causes "Could not find template object for <PropertyName>" errors
	// on cooked-client join-in-progress when the slot's class was replaced. See
	// JOIN_IN_PROGRESS_CRASH_PROPOSAL.md / EDITOR_REPRO_NOTES.md for the diagnostic
	// evidence.
	const FName SubObjectName = ObjectProp->GetFName();

	if (PrevValue)
	{
		// Free the FName so NewObject can reuse it without colliding. Rename to the
		// transient package with an auto-generated unique name; MarkAsGarbage so the
		// reflected pointer is treated as dead.
		PrevValue->Rename(
			nullptr,
			GetTransientPackage(),
			REN_DontCreateRedirectors | REN_NonTransactional | REN_ForceNoResetLoaders);
		PrevValue->MarkAsGarbage();
	}

	EObjectFlags Flags = RF_Transactional;
	Flags |= (Outer->GetFlags() & (RF_Public | RF_ArchetypeObject));

	UObject* NewObj = NewObject<UObject>(Outer, SubObjectClass, SubObjectName, Flags);
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
