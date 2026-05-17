// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_UObjectInspect.h"

#include "ClaireonPathResolver.h"
#include "ClaireonStructReflection.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "Tools/FToolSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace ClaireonToolUObjectInspect_Internal
{
	struct FPropertyEntry
	{
		FString Name;
		FString Kind;
		FString CppType;
		FString Access;        // "public" / "protected" / "private"
		FString BpAccess;      // "none" / "read" / "read_write" / "assignable"
		FString EditorAccess;  // "none" / "visible" / "edit" / "edit_const"
		TSharedPtr<FJsonValue> Value; // optional, only populated when include_values=true
	};

	FString DeriveAccess(EPropertyFlags Flags);
	FString DeriveBpAccess(EPropertyFlags Flags);
	FString DeriveEditorAccess(EPropertyFlags Flags);
	FString DelegateValueDescription(FProperty* Property, const void* ValuePtr);

	void BuildPropertyListing(
		UObject* Object,
		const FString& Filter,
		bool bIncludeValues,
		int32 MaxDepth,
		TArray<FPropertyEntry>& OutEntries);

	TSharedPtr<FJsonObject> EntryToJson(const FPropertyEntry& Entry);

	UObject* ResolveObject(const FString& ObjectPath, bool bAllowLoad, FString& OutError);
}

FString ClaireonTool_UObjectInspect::GetOperation() const
{
	return TEXT("inspect");
}

FString ClaireonTool_UObjectInspect::GetDescription() const
{
	return TEXT(
		"Read any UPROPERTY value on any loaded UObject (asset, CDO, or "
		"world actor) via FProperty reflection. Unlike get_editor_property, "
		"this tool ignores Blueprint accessor specifiers and C++ access "
		"level, so it can read fields declared without "
		"BlueprintReadOnly/BlueprintReadWrite/EditAnywhere or fields "
		"declared protected/private. Read-only. Listing mode (omit "
		"property_path) returns schema for every property; pass "
		"include_values=true to also include serialized values. Targeted "
		"mode (property_path='Foo.Bar[0]') returns one value. Component "
		"sub-properties are reached by traversing the actor's component "
		"UPROPERTY: property_path='MyComp.SomeField'.");
}

TArray<FString> ClaireonTool_UObjectInspect::GetSearchKeywords() const
{
	return {
		TEXT("inspect"),
		TEXT("property"),
		TEXT("properties"),
		TEXT("reflection"),
		TEXT("uobject"),
		TEXT("cdo"),
		TEXT("actor"),
		TEXT("protected"),
		TEXT("private"),
		TEXT("transient"),
		TEXT("bp-visibility"),
	};
}

TSharedPtr<FJsonObject> ClaireonTool_UObjectInspect::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("object_path"),
		TEXT("Path to a UObject. Accepts asset paths (/Game/...), native class paths (/Script/Module.ClassName, inspects the CDO), and world-actor / sub-object paths."),
		true);
	S.AddString(TEXT("property_path"),
		TEXT("Dot-path with [N] for TArray indexing (e.g. 'Foo.Bar[0]'). Omit for listing mode."));
	S.AddInteger(TEXT("max_depth"),
		TEXT("Recursion depth for nested structs and instanced sub-objects. Clamped to [0, 8]. Default: 2."));
	S.AddString(TEXT("filter"),
		TEXT("Substring filter on property names; listing mode only. Default: empty (no filter)."));
	S.AddBoolean(TEXT("include_values"),
		TEXT("Listing mode only. When true, each entry includes a 'value' field serialized to max_depth. Default: false."));
	S.AddBoolean(TEXT("allow_load"),
		TEXT("When true (default), fall back to StaticLoadObject for asset paths not already in memory. When false, return 'object not loaded'."));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_UObjectInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	using namespace ClaireonToolUObjectInspect_Internal;

	// 1) Parse args.
	FString ObjectPath;
	if (!Arguments->TryGetStringField(TEXT("object_path"), ObjectPath) || ObjectPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: object_path"));
	}

	FString PropertyPath;
	Arguments->TryGetStringField(TEXT("property_path"), PropertyPath);

	int32 MaxDepth = 2;
	if (Arguments->HasTypedField<EJson::Number>(TEXT("max_depth")))
	{
		MaxDepth = static_cast<int32>(Arguments->GetNumberField(TEXT("max_depth")));
	}
	MaxDepth = FMath::Clamp(MaxDepth, 0, 8);

	FString Filter;
	Arguments->TryGetStringField(TEXT("filter"), Filter);

	bool bIncludeValues = false;
	Arguments->TryGetBoolField(TEXT("include_values"), bIncludeValues);

	bool bAllowLoad = true;
	Arguments->TryGetBoolField(TEXT("allow_load"), bAllowLoad);

	// 2) Resolve object.
	FString ResolveError;
	UObject* Object = ResolveObject(ObjectPath, bAllowLoad, ResolveError);
	if (!Object)
	{
		return MakeErrorResult(ResolveError);
	}

	// 3) Listing mode.
	if (PropertyPath.IsEmpty())
	{
		TArray<FPropertyEntry> Entries;
		BuildPropertyListing(Object, Filter, bIncludeValues, MaxDepth, Entries);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("object_path"), Object->GetPathName());
		Data->SetStringField(TEXT("class"), Object->GetClass()->GetName());
		if (UClass* Super = Object->GetClass()->GetSuperClass())
		{
			Data->SetStringField(TEXT("class_super"), Super->GetName());
		}
		Data->SetBoolField(TEXT("is_cdo"), Object->HasAnyFlags(RF_ClassDefaultObject));

		TArray<TSharedPtr<FJsonValue>> PropertiesArray;
		PropertiesArray.Reserve(Entries.Num());
		for (const FPropertyEntry& Entry : Entries)
		{
			PropertiesArray.Add(MakeShared<FJsonValueObject>(EntryToJson(Entry)));
		}
		Data->SetArrayField(TEXT("properties"), PropertiesArray);

		const FString Summary = FString::Printf(
			TEXT("uobject_inspect: %s (%s) -- %d propert%s"),
			*Object->GetName(),
			*Object->GetClass()->GetName(),
			Entries.Num(),
			Entries.Num() == 1 ? TEXT("y") : TEXT("ies"));
		return MakeSuccessResult(Data, Summary);
	}

	// 4) Targeted mode.
	void* LeafContainer = nullptr;
	FString PathError;
	FProperty* LeafProperty = ClaireonPropertyUtils::ResolvePropertyByPath(
		Object, PropertyPath, LeafContainer, PathError);
	if (!LeafProperty)
	{
		return MakeErrorResult(
			FString::Printf(TEXT("Failed to resolve property_path '%s': %s"),
				*PropertyPath, *PathError));
	}

	// ResolvePropertyByPath returns Container; for array-leaf elements the
	// container IS the element data and the leaf property is the inner.
	// ClaireonPropertyUtils internally normalizes this in ReadPropertyByPath
	// (see lines ~310-315 of ClaireonPropertyUtils.cpp). For uobject_inspect we
	// adopt the same convention: ValuePtr is Container when the path's last
	// segment was an array index, otherwise ContainerPtrToValuePtr<void>.
	const bool bLastSegmentIsArrayIndex = PropertyPath.EndsWith(TEXT("]"));
	const void* ValuePtr = bLastSegmentIsArrayIndex
		? LeafContainer
		: LeafProperty->ContainerPtrToValuePtr<void>(LeafContainer);

	TSharedPtr<FJsonValue> SerializedValue =
		ClaireonPropertyUtils::PropertyToJsonValue(LeafProperty, ValuePtr, Object, MaxDepth);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("object_path"), Object->GetPathName());
	Data->SetStringField(TEXT("property_path"), PropertyPath);
	Data->SetStringField(TEXT("kind"), ClaireonStructReflection::ClassifyProperty(LeafProperty));
	Data->SetStringField(TEXT("cpp_type"), ClaireonStructReflection::GetPropertySubtypePath(LeafProperty));
	Data->SetField(TEXT("value"), SerializedValue);

	const FString Summary = FString::Printf(
		TEXT("uobject_inspect: %s.%s"),
		*Object->GetName(),
		*PropertyPath);
	return MakeSuccessResult(Data, Summary);
}

namespace ClaireonToolUObjectInspect_Internal
{
	UObject* ResolveObject(const FString& ObjectPath, bool bAllowLoad, FString& OutError)
	{
		ClaireonPathResolver::FResolveResult Resolved = ClaireonPathResolver::Resolve(ObjectPath);
		if (!Resolved.bSuccess)
		{
			OutError = FString::Printf(
				TEXT("Could not resolve path '%s': %s"),
				*ObjectPath,
				*Resolved.Error);
			return nullptr;
		}

		const FString& Path = Resolved.ResolvedPath.Path;

		// (a) Native class path -> CDO.
		if (Resolved.ResolvedPath.Kind == ClaireonPathResolver::EPathKind::NativeClassPath)
		{
			UClass* ResolvedClass = FindObject<UClass>(nullptr, *Path);
			if (!ResolvedClass)
			{
				ResolvedClass = FindFirstObjectSafe<UClass>(*Path);
			}
			if (!ResolvedClass && bAllowLoad)
			{
				ResolvedClass = LoadObject<UClass>(nullptr, *Path);
			}
			if (!ResolvedClass)
			{
				OutError = FString::Printf(
					TEXT("Could not resolve native class '%s' for CDO inspection."),
					*Path);
				return nullptr;
			}
			return ResolvedClass->GetDefaultObject();
		}

		// (b) Asset path or sub-object path.
		UObject* Found = StaticFindObject(
			UObject::StaticClass(),
			/*Outer=*/nullptr,
			*Path,
			/*ExactClass=*/false);

		const bool bIsSubObject = Path.Contains(TEXT(":"));
		if (!Found && bIsSubObject)
		{
			// ANY_PACKAGE is deprecated in UE 5.1+; use FindFirstObjectSafe for
			// sub-object / world-actor paths that may not have a known outer.
			Found = FindFirstObjectSafe<UObject>(*Path);
		}

		if (!Found && !bIsSubObject && bAllowLoad)
		{
			Found = StaticLoadObject(
				UObject::StaticClass(),
				/*Outer=*/nullptr,
				*Path,
				/*Filename=*/nullptr,
				LOAD_None);
		}

		if (!Found)
		{
			if (!bAllowLoad && !bIsSubObject)
			{
				OutError = FString::Printf(
					TEXT("Object '%s' is not loaded (allow_load=false)."),
					*ObjectPath);
			}
			else
			{
				OutError = FString::Printf(
					TEXT("Could not find object '%s'."),
					*ObjectPath);
			}
			return nullptr;
		}

		return Found;
	}

	void BuildPropertyListing(
		UObject* Object,
		const FString& Filter,
		bool bIncludeValues,
		int32 MaxDepth,
		TArray<FPropertyEntry>& OutEntries)
	{
		UClass* Class = Object->GetClass();
		for (TFieldIterator<FProperty> It(Class); It; ++It)
		{
			FProperty* Property = *It;
			const FString Name = Property->GetName();

			// Substring filter (case-sensitive, matches sibling tools).
			if (!Filter.IsEmpty() && !Name.Contains(Filter))
			{
				continue;
			}

			// CRITICAL: do NOT skip CPF_Transient or CPF_Deprecated here. The
			// whole point of this tool is to surface every reflected property
			// for debugging, including transient runtime state. (Review C2.)

			FPropertyEntry Entry;
			Entry.Name = Name;
			Entry.Kind = ClaireonStructReflection::ClassifyProperty(Property);
			Entry.CppType = ClaireonStructReflection::GetPropertySubtypePath(Property);
			const EPropertyFlags Flags = Property->GetPropertyFlags();
			Entry.Access = DeriveAccess(Flags);
			Entry.BpAccess = DeriveBpAccess(Flags);
			Entry.EditorAccess = DeriveEditorAccess(Flags);

			if (bIncludeValues)
			{
				if (CastField<FMulticastDelegateProperty>(Property) != nullptr ||
					CastField<FDelegateProperty>(Property) != nullptr)
				{
					const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
					Entry.Value = MakeShared<FJsonValueString>(
						DelegateValueDescription(Property, ValuePtr));
				}
				else
				{
					const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
					Entry.Value = ClaireonPropertyUtils::PropertyToJsonValue(Property, ValuePtr, Object, MaxDepth);
				}
			}

			OutEntries.Add(MoveTemp(Entry));
		}
	}

	FString DeriveAccess(EPropertyFlags Flags)
	{
		if (Flags & CPF_NativeAccessSpecifierPrivate)   { return TEXT("private"); }
		if (Flags & CPF_NativeAccessSpecifierProtected) { return TEXT("protected"); }
		return TEXT("public");
	}

	FString DeriveBpAccess(EPropertyFlags Flags)
	{
		if (Flags & CPF_BlueprintAssignable) { return TEXT("assignable"); }
		if (Flags & CPF_BlueprintVisible)
		{
			return (Flags & CPF_BlueprintReadOnly) ? TEXT("read") : TEXT("read_write");
		}
		return TEXT("none");
	}

	FString DeriveEditorAccess(EPropertyFlags Flags)
	{
		const bool bEdit = (Flags & CPF_Edit) != 0;
		const bool bEditConst = (Flags & CPF_EditConst) != 0;
		if (bEdit && bEditConst) { return TEXT("edit_const"); }
		if (bEdit)               { return TEXT("edit"); }
		return TEXT("none");
	}

	FString DelegateValueDescription(FProperty* Property, const void* ValuePtr)
	{
		if (FMulticastDelegateProperty* Multicast = CastField<FMulticastDelegateProperty>(Property))
		{
			const FMulticastScriptDelegate* Delegate =
				Multicast->GetMulticastDelegate(const_cast<void*>(ValuePtr));
			if (!Delegate || !Delegate->IsBound())
			{
				return TEXT("<unbound>");
			}
			TArray<UObject*> Receivers = Delegate->GetAllObjects();
			TArray<FString> Bindings;
			for (UObject* Receiver : Receivers)
			{
				if (Receiver) { Bindings.Add(Receiver->GetName()); }
			}
			if (Bindings.Num() == 0) { return TEXT("<unbound>"); }
			return FString::Join(Bindings, TEXT(", "));
		}

		if (FDelegateProperty* Single = CastField<FDelegateProperty>(Property))
		{
			const FScriptDelegate* Delegate = static_cast<const FScriptDelegate*>(ValuePtr);
			if (!Delegate || !Delegate->IsBound())
			{
				return TEXT("<unbound>");
			}
			return Delegate->GetFunctionName().ToString();
		}

		return FString();
	}

	TSharedPtr<FJsonObject> EntryToJson(const FPropertyEntry& Entry)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Entry.Name);
		Obj->SetStringField(TEXT("kind"), Entry.Kind);
		Obj->SetStringField(TEXT("cpp_type"), Entry.CppType);
		Obj->SetStringField(TEXT("access"), Entry.Access);
		Obj->SetStringField(TEXT("bp_access"), Entry.BpAccess);
		Obj->SetStringField(TEXT("editor_access"), Entry.EditorAccess);
		if (Entry.Value.IsValid())
		{
			Obj->SetField(TEXT("value"), Entry.Value);
		}
		return Obj;
	}
}
