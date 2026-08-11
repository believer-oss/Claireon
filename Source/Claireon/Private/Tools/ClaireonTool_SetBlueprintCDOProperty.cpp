// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_SetBlueprintCDOProperty.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h" // kBPCategory
#include "ClaireonPathResolver.h"
#include "ClaireonBlueprintHelpers.h"
#include "Tools/ClaireonPropertyResolver.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "UObject/UnrealType.h"

FString ClaireonTool_SetBlueprintCDOProperty::GetOperation() const { return TEXT("set_cdo_property"); }

FString ClaireonTool_SetBlueprintCDOProperty::GetCategory() const
{
	return kBPCategory;
}

FString ClaireonTool_SetBlueprintCDOProperty::GetDescription() const
{
	return TEXT("Set a property on a Blueprint's Class Default Object by asset path, via ImportText serialization so every "
		"property type (including TSoftClassPtr) is supported. property_path addresses nested structs and array "
		"elements, any segment taking a `[N]` suffix; component template properties resolve by automatic SCS "
		"lookup. Immediate-mode tool: writes the asset directly, no open session required.");
}

TSharedPtr<FJsonObject> ClaireonTool_SetBlueprintCDOProperty::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"),
		TEXT("Full Unreal asset path to the Blueprint (e.g. /Game/Path/To/BP_MyBlueprint)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	TSharedPtr<FJsonObject> PropertyNameProp = MakeShared<FJsonObject>();
	PropertyNameProp->SetStringField(TEXT("type"), TEXT("string"));
	PropertyNameProp->SetStringField(TEXT("description"),
		TEXT("Name of the property to set on the CDO"));
	Properties->SetObjectField(TEXT("property_name"), PropertyNameProp);

	TSharedPtr<FJsonObject> ValueProp = MakeShared<FJsonObject>();
	ValueProp->SetStringField(TEXT("type"), TEXT("string"));
	ValueProp->SetStringField(TEXT("description"),
		TEXT("New value as a string (passed to ImportText_Direct)"));
	Properties->SetObjectField(TEXT("value"), ValueProp);

	TSharedPtr<FJsonObject> PropertyPathProp = MakeShared<FJsonObject>();
	PropertyPathProp->SetStringField(TEXT("type"), TEXT("string"));
	PropertyPathProp->SetStringField(TEXT("description"),
		TEXT("Optional dot-separated path for nested struct/array properties. "
		     "Each segment may be suffixed with `[N]` to index a TArray "
		     "(e.g. `waves[0].spawn_count`). The final segment is taken from `property_name`. "
		     "If the first segment names a component property on the Blueprint CDO, "
		     "the write is redirected to that component's SCS template."));
	Properties->SetObjectField(TEXT("property_path"), PropertyPathProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("property_name")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("value")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_SetBlueprintCDOProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Extract required parameters
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	FString PropertyName;
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		return MakeErrorResult(TEXT("Missing required field: property_name"));
	}

	FString Value;
	if (!Arguments->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required field: value"));
	}

	// Extract optional property_path
	FString PropertyPath;
	Arguments->TryGetStringField(TEXT("property_path"), PropertyPath);

	// Ergonomics guard, before any asset work: callers reasonably read
	// property_path as "the whole path" and pass
	// property_path='PrimaryActorTick.bCanEverTick' together with
	// property_name='bCanEverTick'. These two are concatenated as
	// property_path + '.' + property_name, so that gives
	// 'PrimaryActorTick.bCanEverTick.bCanEverTick' and the resolver's reply was
	// the raw "Cannot navigate through non-struct" leak, which names neither
	// parameter. This is an argument-shape error, so it is answered without
	// loading anything.
	if (!PropertyPath.IsEmpty()
		&& (PropertyPath.EndsWith(TEXT(".") + PropertyName) || PropertyPath.Equals(PropertyName)))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("property_path ('%s') already ends with property_name ('%s'). These two are "
				 "concatenated as property_path + '.' + property_name: pass the container path in "
				 "property_path and the leaf alone in property_name, or pass the whole dotted path "
				 "in property_name and leave property_path empty."),
			*PropertyPath, *PropertyName));
	}

	// Step 1: Resolve asset_path
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	// Step 2: Load Blueprint
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!IsValid(Blueprint))
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath));
	}

	// Step 3: Validate GeneratedClass
	if (!IsValid(Blueprint->GeneratedClass))
	{
		return MakeErrorResult(TEXT("Blueprint has no GeneratedClass -- compile it first"));
	}

	// Step 4: Get CDO
	UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
	if (!IsValid(CDO))
	{
		return MakeErrorResult(TEXT("Failed to get Blueprint CDO"));
	}

	// Step 5: Build the combined path for the resolver + writer.
	// Grammar and bounds-checking live in ClaireonPropertyUtils::WritePropertyByPath.
	// This tool only composes property_path + property_name into a single string.
	FString CombinedPath;
	if (PropertyPath.IsEmpty())
	{
		CombinedPath = PropertyName;
	}
	else
	{
		// The overlap form is rejected up front, before any asset work.
		CombinedPath = PropertyPath + TEXT(".") + PropertyName;
	}

	// Step 6: Resolve on Blueprint CDO (with SCS component fall-through).
	// Resolver handles "first segment is a component name" cases and returns
	// RemainingPath (the tail to forward to the writer).
	ClaireonPropertyResolver::FResolvedProperty Resolved;
	FString ResolveError;
	if (!ClaireonPropertyResolver::ResolvePropertyOnBlueprintCDO(Blueprint, CombinedPath, Resolved, ResolveError))
	{
		return MakeErrorResult(ResolveError);
	}

	UObject* TargetObject = Resolved.TargetObject;
	if (!IsValid(TargetObject))
	{
		// Defensive: ResolvePropertyOnBlueprintCDO should never succeed with a null target.
		return MakeErrorResult(TEXT("Internal error: resolver returned null TargetObject"));
	}

	// Step 7: Export old value through the same path walker used for the write.
	// Error from the reader is not fatal here -- if the read fails the write still proceeds
	// and the response simply omits old_value. This mirrors a missing leaf being created by import.
	FString OldValue;
	{
		FString ReadError;
		OldValue = ClaireonPropertyUtils::ReadPropertyByPath(TargetObject, Resolved.RemainingPath, ReadError);
		// ReadError deliberately swallowed; treated as non-fatal. Do not return MakeErrorResult here.
	}

	// Step 7b: Detect a UPROPERTY(Instanced) FObjectProperty leaf. ImportText_Direct on such a
	// slot interprets a class-path string as a class *reference* and silently reverts to None,
	// so we redirect those writes to SetInstancedSubObject which constructs an embedded sub-object
	// of the requested class. Read-only peek -- no transaction required.
	bool bInstancedLeaf = false;
	{
		void* PeekContainer = nullptr;
		FString PeekError;
		FProperty* LeafProp = ClaireonPropertyUtils::ResolvePropertyByPath(
			TargetObject, Resolved.RemainingPath, PeekContainer, PeekError);
		if (LeafProp)
		{
			FObjectProperty* ObjProp = CastField<FObjectProperty>(LeafProp);
			bInstancedLeaf = ObjProp && ObjProp->HasAnyPropertyFlags(CPF_InstancedReference);
		}
	}

	// Step 8: Open transaction + Modify() cluster.
	// FScopedTransaction + CDO->Modify() capture the whole object's serialized state so
	// in-place array element writes performed by FScriptArrayHelper::GetRawPtr inside
	// WritePropertyByPath are rolled back on undo.
	//
	// SetFlags(RF_Transactional) before Modify() is load-bearing, not defensive. A Blueprint
	// CDO is allocated with only RF_Public|RF_ClassDefaultObject|RF_ArchetypeObject
	// (UClass::CreateDefaultObject, Class.cpp:4867) and SaveToTransactionBuffer refuses any
	// object lacking RF_Transactional (UObjectGlobals.cpp:3256-3259). Without the flag,
	// CDO->Modify() only marks the package dirty and records nothing -- so this tool opened a
	// transaction, advertising undo, and delivered none of it for CDO writes. Epic hits the
	// same wall and works around it identically for Widget Blueprint CDOs
	// (WidgetBlueprintEditorUtils.cpp:240-241, 265-266). The flag is read inside Modify(), so
	// the order matters.
	//
	// Known side effect: RF_Transactional is part of RF_Load (ObjectMacros.h:598), so the flag
	// persists into the saved package's CDO export. Each asset this tool writes therefore takes
	// a one-time binary flag diff. Epic accepts that cost for widget CDOs; the alternative is a
	// tool that claims to be undoable and is not.
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Blueprint CDO Property")));
	Blueprint->Modify();
	UObject* CDOForModify = Blueprint->GeneratedClass->GetDefaultObject();
	if (IsValid(CDOForModify))
	{
		CDOForModify->SetFlags(RF_Transactional);
		CDOForModify->Modify();
	}
	if (TargetObject != CDOForModify)
	{
		TargetObject->SetFlags(RF_Transactional);
		TargetObject->Modify();
	}

	// Step 9: Write. For Instanced FObjectProperty leaves, route through SetInstancedSubObject
	// (value == class path constructs the sub-object; value == "None"/empty clears the slot).
	// Everything else goes through the generic ImportText path.
	FString InstancedNote;
	if (bInstancedLeaf)
	{
		void* SlotContainer = nullptr;
		FString SlotError;
		FProperty* LeafProp = ClaireonPropertyUtils::ResolvePropertyByPath(
			TargetObject, Resolved.RemainingPath, SlotContainer, SlotError);
		FObjectProperty* ObjectProp = CastField<FObjectProperty>(LeafProp);
		if (!ObjectProp)
		{
			return MakeErrorResult(SlotError.IsEmpty()
				? TEXT("Internal error: instanced leaf resolution lost FObjectProperty")
				: SlotError);
		}

		if (Value.IsEmpty() || Value.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			void* SlotPtr = ObjectProp->ContainerPtrToValuePtr<void>(SlotContainer);
			if (UObject* PrevValue = ObjectProp->GetObjectPropertyValue(SlotPtr); IsValid(PrevValue))
			{
				PrevValue->MarkAsGarbage();
			}
			ObjectProp->SetObjectPropertyValue(SlotPtr, nullptr);
			InstancedNote = TEXT("cleared instanced sub-object slot");
		}
		else
		{
			UClass* SubObjectClass = LoadClass<UObject>(nullptr, *Value);
			if (!IsValid(SubObjectClass))
			{
				// Blueprint generated classes resolve with a trailing "_C" suffix.
				SubObjectClass = LoadClass<UObject>(nullptr, *(Value + TEXT("_C")));
			}
			if (IsValid(SubObjectClass))
			{
				FString WriteError;
				UObject* NewSubObject = ClaireonPropertyUtils::SetInstancedSubObject(
					TargetObject, SubObjectClass, Resolved.RemainingPath, WriteError);
				if (!IsValid(NewSubObject))
				{
					return MakeErrorResult(WriteError);
				}
				InstancedNote = FString::Printf(
					TEXT("auto-constructed instanced sub-object of class '%s' via SetInstancedSubObject"),
					*SubObjectClass->GetName());
			}
			else
			{
				// Not a class path. Accept an exported OBJECT reference -- either a
				// bare path or the ClassName'"/Path.To:Subobject"' export form -- by
				// duplicating the referenced template into this object. This is what
				// a cdo_fields_text export of an instanced slot round-trips through.
				FString ObjectPath = Value;
				int32 ApostropheIdx = INDEX_NONE;
				if (ObjectPath.FindChar(TEXT('\''), ApostropheIdx) && ObjectPath.EndsWith(TEXT("'")) && ApostropheIdx < ObjectPath.Len() - 1)
				{
					ObjectPath = ObjectPath.Mid(ApostropheIdx + 1, ObjectPath.Len() - ApostropheIdx - 2);
				}
				ObjectPath = ObjectPath.TrimQuotes();
				UObject* TemplateObj = LoadObject<UObject>(nullptr, *ObjectPath);
				if (!IsValid(TemplateObj))
				{
					TemplateObj = FindObject<UObject>(nullptr, *ObjectPath);
				}
				if (!IsValid(TemplateObj))
				{
					return MakeErrorResult(FString::Printf(
						TEXT("'%s' is UPROPERTY(Instanced); value must be a class path, an object path to duplicate, or 'None' to clear. Failed to resolve '%s'."),
						*CombinedPath, *Value));
				}

				void* SlotPtr = ObjectProp->ContainerPtrToValuePtr<void>(SlotContainer);
				FName DupName = TemplateObj->GetFName();
				if (IsValid(StaticFindObjectFast(nullptr, TargetObject, DupName)))
				{
					DupName = MakeUniqueObjectName(TargetObject, TemplateObj->GetClass(), DupName);
				}
				UObject* Dup = StaticDuplicateObject(TemplateObj, TargetObject, DupName);
				if (!IsValid(Dup))
				{
					return MakeErrorResult(FString::Printf(
						TEXT("Failed to duplicate '%s' into '%s'"), *ObjectPath, *TargetObject->GetName()));
				}
				Dup->SetFlags(RF_Transactional);
				if (TargetObject->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
				{
					Dup->SetFlags(RF_ArchetypeObject);
				}
				if (UObject* PrevValue = ObjectProp->GetObjectPropertyValue(SlotPtr); IsValid(PrevValue))
				{
					PrevValue->MarkAsGarbage();
				}
				ObjectProp->SetObjectPropertyValue(SlotPtr, Dup);
				InstancedNote = FString::Printf(
					TEXT("duplicated instanced sub-object from '%s' (class '%s')"),
					*ObjectPath, *Dup->GetClass()->GetName());
			}
		}
	}
	else
	{
		// Delegate the write to ClaireonPropertyUtils. Error strings come from the helper
		// (ParsePathSegments / ResolvePath / ImportText_Direct) -- do not invent new strings here.
		FString WriteError;
		if (!ClaireonPropertyUtils::WritePropertyByPath(TargetObject, Resolved.RemainingPath, Value, WriteError))
		{
			return MakeErrorResult(WriteError);
		}
	}

	// Step 10: Mark the Blueprint package dirty so editor save/cook picks up the change.
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	// Step 11: Build response JSON.
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("property_name"), PropertyName);
	Data->SetStringField(TEXT("old_value"), OldValue);
	Data->SetStringField(TEXT("new_value"), Value);
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	if (!PropertyPath.IsEmpty())
	{
		Data->SetStringField(TEXT("property_path"), PropertyPath);
	}
	if (!Resolved.ResolvedOn.IsEmpty())
	{
		Data->SetStringField(TEXT("resolved_on"), Resolved.ResolvedOn);
	}
	FString CombinedNote = Resolved.Note;
	if (!InstancedNote.IsEmpty())
	{
		if (CombinedNote.IsEmpty())
		{
			CombinedNote = InstancedNote;
		}
		else
		{
			CombinedNote += TEXT("; ") + InstancedNote;
		}
	}
	if (!CombinedNote.IsEmpty())
	{
		Data->SetStringField(TEXT("note"), CombinedNote);
	}

	const FString Summary = FString::Printf(
		TEXT("Set %s.%s = '%s' (was '%s')"),
		*AssetPath, *CombinedPath, *Value, *OldValue);
	return MakeSuccessResult(Data, Summary);
}
