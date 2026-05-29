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
	return TEXT("Set a property on a Blueprint's Class Default Object (CDO) by asset path. "
		"Supports all property types via ImportText serialization including TSoftClassPtr. "
		"Sessionless alternative to edit_blueprint_graph for simple property changes. "
		"Supports component template properties via automatic SCS component lookup. "
		"Supports nested struct and array element writes via the `property_path` argument; "
		"each path segment may be suffixed with `[N]` to index a TArray. Immediate-mode tool: no session required.");
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

	// Step 1: Resolve asset_path
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	// Step 2: Load Blueprint
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath));
	}

	// Step 3: Validate GeneratedClass
	if (!Blueprint->GeneratedClass)
	{
		return MakeErrorResult(TEXT("Blueprint has no GeneratedClass -- compile it first"));
	}

	// Step 4: Get CDO
	UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
	if (!CDO)
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
	if (!TargetObject)
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
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Blueprint CDO Property")));
	Blueprint->Modify();
	UObject* CDOForModify = Blueprint->GeneratedClass->GetDefaultObject();
	if (CDOForModify)
	{
		CDOForModify->Modify();
	}
	if (TargetObject != CDOForModify)
	{
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
			if (UObject* PrevValue = ObjectProp->GetObjectPropertyValue(SlotPtr))
			{
				PrevValue->MarkAsGarbage();
			}
			ObjectProp->SetObjectPropertyValue(SlotPtr, nullptr);
			InstancedNote = TEXT("cleared instanced sub-object slot");
		}
		else
		{
			UClass* SubObjectClass = LoadClass<UObject>(nullptr, *Value);
			if (!SubObjectClass)
			{
				// Blueprint generated classes resolve with a trailing "_C" suffix.
				SubObjectClass = LoadClass<UObject>(nullptr, *(Value + TEXT("_C")));
			}
			if (!SubObjectClass)
			{
				return MakeErrorResult(FString::Printf(
					TEXT("'%s' is UPROPERTY(Instanced); value must be a class path or 'None' to clear. Failed to load class from '%s'."),
					*CombinedPath, *Value));
			}

			FString WriteError;
			UObject* NewSubObject = ClaireonPropertyUtils::SetInstancedSubObject(
				TargetObject, SubObjectClass, Resolved.RemainingPath, WriteError);
			if (!NewSubObject)
			{
				return MakeErrorResult(WriteError);
			}
			InstancedNote = FString::Printf(
				TEXT("auto-constructed instanced sub-object of class '%s' via SetInstancedSubObject"),
				*SubObjectClass->GetName());
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
