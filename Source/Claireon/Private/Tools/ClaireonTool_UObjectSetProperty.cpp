// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_UObjectSetProperty.h"

#include "ClaireonLog.h"
#include "ClaireonPathResolver.h"
#include "ClaireonStructReflection.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "Tools/FToolSchemaBuilder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"

namespace ClaireonToolUObjectSetProperty_Internal
{
	/**
	 * Walk outward from a mutated object to the UBlueprint that authored it, if any.
	 *
	 * Covers the two shapes this tool can land on: a Blueprint CDO (its class's
	 * ClassGeneratedBy) and an SCS ComponentTemplate (outered to the BPGC, whose
	 * ClassGeneratedBy is the Blueprint). Returns nullptr for plain assets and for
	 * live world/PIE instances, which have no authoring Blueprint to dirty.
	 */
	UBlueprint* FindAuthoringBlueprint(UObject* Object)
	{
		if (!IsValid(Object))
		{
			return nullptr;
		}

		if (UClass* OwnerClass = Object->GetClass(); IsValid(OwnerClass))
		{
			if (UBlueprint* FromClass = Cast<UBlueprint>(OwnerClass->ClassGeneratedBy); IsValid(FromClass))
			{
				// Only a CDO/archetype represents authored state; a spawned instance of a
				// Blueprint class must not dirty the asset.
				if (Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
				{
					return FromClass;
				}
			}
		}

		// SCS ComponentTemplate: outered to the generated class.
		for (UObject* Outer = Object->GetOuter(); IsValid(Outer); Outer = Outer->GetOuter())
		{
			if (UClass* OuterClass = Cast<UClass>(Outer); IsValid(OuterClass))
			{
				return Cast<UBlueprint>(OuterClass->ClassGeneratedBy);
			}
			if (UBlueprint* OuterBlueprint = Cast<UBlueprint>(Outer); IsValid(OuterBlueprint))
			{
				return OuterBlueprint;
			}
		}

		return nullptr;
	}

	/**
	 * Runs the details-panel change cycle on the object that actually owns the leaf,
	 * when that is not the object the path started from.
	 *
	 * ClaireonPropertyUtils::WritePropertyByPath brackets its import with
	 * PreEditChange / PostEditChangeProperty on the ROOT object. For a path like
	 * "MyComp.bCanEverAffectNavigation" that notifies the actor and never the
	 * component -- so UActorComponent::ConsolidatedPostEditChange never runs, the
	 * FComponentReregisterContext is never taken, and HandleCanEverAffectNavigationChange
	 * never fires. That is precisely the nav-octree desync this batch is fixing.
	 *
	 * RAII because UActorComponent::PreEditChange checkf()s on an unmatched pair: once
	 * the pre has run, the post MUST run even if the write fails.
	 */
	struct FScopedOwnerChangeNotify
	{
		UObject* Owner = nullptr;
		FProperty* Property = nullptr;

		FScopedOwnerChangeNotify(UObject* InOwner, FProperty* InProperty)
			: Owner(InOwner)
			, Property(InProperty)
		{
			if (IsValid(Owner))
			{
				// Pass the owner-relative property only when the leaf really is one of the
				// owner's own; for a leaf inside a nested struct it is not, and null is the
				// honest answer (it still takes the reregister context).
				Owner->PreEditChange(OwnerRelativeProperty());
			}
		}

		~FScopedOwnerChangeNotify()
		{
			if (IsValid(Owner))
			{
				FPropertyChangedEvent ChangedEvent(Property, EPropertyChangeType::ValueSet);
				Owner->PostEditChangeProperty(ChangedEvent);
			}
		}

		FProperty* OwnerRelativeProperty() const
		{
			if (!IsValid(Owner) || !Property)
			{
				return nullptr;
			}
			UClass* OwnerClass = Owner->GetClass();
			return (IsValid(OwnerClass) && OwnerClass->FindPropertyByName(Property->GetFName()) == Property)
				? Property
				: nullptr;
		}
	};
}

FString ClaireonTool_UObjectSetProperty::GetOperation() const
{
	return TEXT("set_property");
}

FString ClaireonTool_UObjectSetProperty::GetDescription() const
{
	return TEXT(
		"Write any UPROPERTY on any loaded UObject (asset, CDO, or PIE actor) via FProperty "
		"reflection -- the write counterpart to uobject_inspect, reaching the same fields, "
		"including protected/private ones. Properties the details panel refuses (no "
		"EditAnywhere, or EditConst) need allow_non_editable=true. Writes are transactional "
		"and fire PreEditChange/PostEditChangeProperty. Immediate-mode: no session.");
}

TArray<FString> ClaireonTool_UObjectSetProperty::GetSearchKeywords() const
{
	return {
		TEXT("set"),
		TEXT("write"),
		TEXT("property"),
		TEXT("reflection"),
		TEXT("uobject"),
		TEXT("cdo"),
		TEXT("actor"),
		TEXT("pie"),
		TEXT("protected"),
		TEXT("private"),
		TEXT("transient"),
		TEXT("non-editable"),
		TEXT("runtime"),
	};
}

TSharedPtr<FJsonObject> ClaireonTool_UObjectSetProperty::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("object_path"),
		TEXT("Path to a UObject. Accepts asset paths (/Game/...), native class paths (/Script/Module.ClassName, writes the CDO), "
			 "world-actor / sub-object paths (including live PIE actor paths like "
			 "'/Game/Maps/UEDPIE_0_Map.Map:PersistentLevel.MyActor_1'), and rooted in-memory object paths "
			 "(/Memory/..., /Temp/...). Same resolution as uobject_inspect."),
		true);
	S.AddString(TEXT("property_path"),
		TEXT("Dot-path to the property, with [N] for TArray indexing (e.g. 'Foo.Bar[0]' or "
			 "'MyComp.PrimaryComponentTick.bStartWithTickEnabled'). On a Blueprint CDO a leading "
			 "component segment resolves to that component's SCS template, where the authored defaults live."),
		true);
	S.AddString(TEXT("value"),
		TEXT("New value in ImportText form -- the same text uobject_inspect exports. Structs use "
			 "'(Member=X,Other=Y)'; enums use the enumerator name; object refs use an asset path. "
			 "An empty string on a TArray/TSet/TMap property clears the container."),
		true);
	S.AddBoolean(TEXT("allow_non_editable"),
		TEXT("Required (true) to write a property the details panel would refuse: one with no EditAnywhere/EditDefaultsOnly, "
			 "or one marked EditConst. Off by default because such fields often carry class invariants the owner maintains. "
			 "Default: false."));
	S.AddBoolean(TEXT("allow_load"),
		TEXT("When true (default), fall back to StaticLoadObject for asset paths not already in memory. When false, return 'object not loaded'."));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_UObjectSetProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	using namespace ClaireonToolUObjectSetProperty_Internal;

	// 1) Parse args.
	FString ObjectPath;
	if (!Arguments->TryGetStringField(TEXT("object_path"), ObjectPath) || ObjectPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: object_path"));
	}

	FString PropertyPath;
	if (!Arguments->TryGetStringField(TEXT("property_path"), PropertyPath) || PropertyPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_path"));
	}

	// A missing `value` is an error; an empty one is not -- empty is the documented
	// "clear this container" form, so TryGetStringField's presence check is the test.
	FString Value;
	if (!Arguments->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	bool bAllowNonEditable = false;
	Arguments->TryGetBoolField(TEXT("allow_non_editable"), bAllowNonEditable);

	bool bAllowLoad = true;
	Arguments->TryGetBoolField(TEXT("allow_load"), bAllowLoad);

	// 2) Resolve object. Shared with uobject_inspect so write reaches exactly what read does.
	FString ResolveError;
	// P0-8b: a path naming a class or Blueprint asset now resolves to its CDO.
	// This is a WRITE path, so disclosing the substitution matters more here than
	// anywhere: the caller needs to know the value landed on the CDO.
	FString CoercionNote;
	UObject* Object = ClaireonPathResolver::ResolveObjectFromPath(ObjectPath, bAllowLoad, ResolveError, &CoercionNote);
	if (!IsValid(Object))
	{
		return MakeErrorResult(ResolveError);
	}

	// 3) Resolve the leaf property and, critically, the object that actually owns it.
	// For "MyComp.Field" the owner is the component, not the actor -- Modify() and the
	// change notification have to land there or the undo record and any derived engine
	// state (the nav octree being the motivating case) go stale.
	void* LeafContainer = nullptr;
	UObject* OwnerObject = Object;
	FString PathError;
	FProperty* LeafProperty = ClaireonPropertyUtils::ResolvePropertyByPath(
		Object, PropertyPath, LeafContainer, PathError, &OwnerObject);
	if (!LeafProperty)
	{
		return MakeErrorResult(
			FString::Printf(TEXT("Failed to resolve property_path '%s': %s"),
				*PropertyPath, *PathError));
	}
	if (!IsValid(OwnerObject))
	{
		OwnerObject = Object;
	}

	const EPropertyFlags LeafFlags = LeafProperty->GetPropertyFlags();
	const FString LeafAccess = ClaireonStructReflection::DescribeAccess(LeafFlags);
	const FString LeafEditorAccess = ClaireonStructReflection::DescribeEditorAccess(LeafFlags);

	// 4) Guard. Reaching past the details panel is opt-in, not implicit. The refusal
	// carries a hint whose args are this exact call with the flag set -- error-derived,
	// so it fires every time rather than being latched.
	const bool bIsEditable = (LeafEditorAccess == TEXT("edit"));
	if (!bIsEditable && !bAllowNonEditable)
	{
		const FString Why = (LeafEditorAccess == TEXT("edit_const"))
			? TEXT("is marked EditConst")
			: TEXT("has no EditAnywhere/EditDefaultsOnly specifier");

		FToolResult Result = MakeErrorResult(FString::Printf(
			TEXT("Property '%s' on %s %s (C++ access: %s), so the details panel would refuse this edit. "
				 "Pass allow_non_editable=true to write it anyway -- such fields often carry invariants "
				 "the owning class maintains."),
			*PropertyPath, *OwnerObject->GetName(), *Why, *LeafAccess));

		TSharedPtr<FJsonObject> HintArgs = CloneHintArgs(Arguments);
		HintArgs->SetBoolField(TEXT("allow_non_editable"), true);
		Result.Hint = MakeGuidanceHint(
			GetName(),
			FString::Printf(
				TEXT("'%s' %s; re-issue with allow_non_editable=true if the write is intended."),
				*PropertyPath, *Why),
			HintArgs);
		return Result;
	}

	// 5) Old value, for the response and the audit line. Read from the root with the
	// same root-relative path the write uses -- not from OwnerObject, which the path is
	// not relative to. A read failure is not fatal: the write can legitimately succeed
	// where the export does not (the same tolerance bp_set_cdo_property applies).
	FString OldValue;
	{
		FString ReadError;
		OldValue = ClaireonPropertyUtils::ReadPropertyByPath(Object, PropertyPath, ReadError);
	}

	// 6) Transaction + Modify() cluster, then write. WritePropertyByPath brackets the
	// import with PreEditChange / PostEditChangeProperty itself.
	//
	// The path is re-walked from Object (not OwnerObject) because WritePropertyByPath
	// takes the same root-relative path this tool was called with; OwnerObject is used
	// only for Modify() and for reporting.
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set UObject Property")));
	Object->Modify();
	if (OwnerObject != Object)
	{
		OwnerObject->Modify();
	}
	UBlueprint* AuthoringBlueprint = FindAuthoringBlueprint(OwnerObject);
	if (IsValid(AuthoringBlueprint))
	{
		AuthoringBlueprint->Modify();
	}

	const bool bNotifiedOwner = (OwnerObject != Object);
	FString WriteError;
	bool bWrote = false;
	{
		// Scoped so the owner's PostEditChangeProperty runs before anything below reads
		// back state that the notification is responsible for re-establishing.
		FScopedOwnerChangeNotify OwnerNotify(
			bNotifiedOwner ? OwnerObject : nullptr, LeafProperty);
		bWrote = ClaireonPropertyUtils::WritePropertyByPath(Object, PropertyPath, Value, WriteError);
	}
	if (!bWrote)
	{
		return MakeErrorResult(WriteError);
	}

	// 7) Dirty the right package. A Blueprint CDO / SCS template edit has to go through
	// MarkBlueprintAsModified or the change is live in memory but never saved.
	if (IsValid(AuthoringBlueprint))
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(AuthoringBlueprint);
	}
	else
	{
		OwnerObject->MarkPackageDirty();
	}

	// 8) Audit line. Every write that reaches past the details panel -- and every one
	// that does not -- is recorded with what changed and whether the guard was waived.
	UE_LOG(LogClaireon, Log,
		TEXT("[audit] uobject_set_property: %s.%s '%s' -> '%s' (access=%s, editor_access=%s, allow_non_editable=%s)"),
		*OwnerObject->GetPathName(),
		*PropertyPath,
		*OldValue,
		*Value,
		*LeafAccess,
		*LeafEditorAccess,
		bAllowNonEditable ? TEXT("true") : TEXT("false"));

	// 9) Response.
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("object_path"), Object->GetPathName());
	Data->SetStringField(TEXT("property_path"), PropertyPath);
	Data->SetStringField(TEXT("old_value"), OldValue);
	Data->SetStringField(TEXT("new_value"), Value);
	Data->SetStringField(TEXT("access"), LeafAccess);
	Data->SetStringField(TEXT("editor_access"), LeafEditorAccess);
	Data->SetBoolField(TEXT("non_editable_override"), !bIsEditable);
	if (bNotifiedOwner)
	{
		Data->SetStringField(TEXT("resolved_on"), OwnerObject->GetName());
		// The write landed on a sub-object, so the change cycle was run there too --
		// without it, a component's derived state (the nav octree, render/physics
		// state) would silently not follow the property.
		Data->SetBoolField(TEXT("owner_change_notified"), true);
	}
	if (IsValid(AuthoringBlueprint))
	{
		Data->SetStringField(TEXT("marked_blueprint_modified"), AuthoringBlueprint->GetPathName());
	}

	const FString Summary = FString::Printf(
		TEXT("uobject_set_property: %s.%s = '%s' (was '%s')"),
		*Object->GetName(), *PropertyPath, *Value, *OldValue);

	FToolResult Result = MakeSuccessResult(Data, Summary);
	if (!CoercionNote.IsEmpty())
	{
		Result.Warnings.Add(CoercionNote);
	}
	if (!bIsEditable)
	{
		Result.Warnings.Add(FString::Printf(
			TEXT("Wrote non-editable property '%s' (editor_access=%s, access=%s) under allow_non_editable=true. "
				 "If the owning class maintains an invariant over this field, it has not been re-established."),
			*PropertyPath, *LeafEditorAccess, *LeafAccess));
	}
	return Result;
}
