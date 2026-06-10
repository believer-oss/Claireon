// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_UObjectReferencers.h"

#include "ClaireonPathResolver.h"
#include "Tools/FToolSchemaBuilder.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "Serialization/FindReferencersArchive.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace ClaireonToolUObjectReferencers_Internal
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
				OutError = FString::Printf(TEXT("Could not resolve native class '%s'."), *Path);
				return nullptr;
			}
			return ResolvedClass->GetDefaultObject();
		}

		UObject* Found = StaticFindObject(UObject::StaticClass(), nullptr, *Path, /*ExactClass=*/false);
		const bool bIsSubObject = Path.Contains(TEXT(":"));
		if (!Found && bIsSubObject)
		{
			Found = FindFirstObjectSafe<UObject>(*Path);
		}
		if (!Found && !bIsSubObject && bAllowLoad)
		{
			Found = StaticLoadObject(UObject::StaticClass(), nullptr, *Path, nullptr, LOAD_None);
		}
		if (!Found)
		{
			OutError = FString::Printf(TEXT("Could not find object '%s'."), *ObjectPath);
			return nullptr;
		}
		return Found;
	}

	FString ClassifyPropertyKind(const FProperty* Property)
	{
		if (!Property)
		{
			return TEXT("hard");
		}
		if (Property->IsA<FWeakObjectProperty>())
		{
			return TEXT("weak");
		}
		if (Property->IsA<FLazyObjectProperty>())
		{
			return TEXT("lazy");
		}
		if (Property->IsA<FInterfaceProperty>())
		{
			return TEXT("interface");
		}
		if (Property->IsA<FClassProperty>())
		{
			return TEXT("class");
		}
		// FObjectProperty (including TObjectPtr-backed) and the catch-all bucket.
		return TEXT("hard");
	}

	void BuildOuterChain(UObject* Object, TArray<TSharedPtr<FJsonValue>>& Out)
	{
		// Walk outers innermost->outermost, skip the object itself.
		for (UObject* Outer = Object ? Object->GetOuter() : nullptr; Outer; Outer = Outer->GetOuter())
		{
			Out.Add(MakeShared<FJsonValueString>(Outer->GetName()));
		}
	}

	struct FReferencerEntry
	{
		UObject* Referencer = nullptr;
		UObject* TargetHit = nullptr;
		FString TargetKind;   // "actor" or "component" (else "object")
		FProperty* Property = nullptr;
	};
}

FString ClaireonTool_UObjectReferencers::GetDescription() const
{
	return TEXT(
		"Reverse-reference finder for any loaded UObject. Given an object_path, "
		"returns the loaded UObjects that hold a UPROPERTY pointing at it, "
		"together with the holding property's name and kind (hard/weak/interface/"
		"class). Built on FFindReferencersArchive, so it sees the same hard "
		"object refs the engine resolves at package load time -- exactly the "
		"refs that force-load actors across cell / data-layer boundaries. Soft "
		"refs are not included; use asset_references for the static / package-"
		"level view. When the target is an AActor and include_components=true "
		"(default), the actor's UActorComponent sub-objects are also probed and "
		"each hit reports target_kind+target_path so the caller sees whether "
		"the referencer points at the actor or one of its components.");
}

TArray<FString> ClaireonTool_UObjectReferencers::GetSearchKeywords() const
{
	return {
		TEXT("referencers"),
		TEXT("references"),
		TEXT("reverse"),
		TEXT("force-load"),
		TEXT("hard-ref"),
		TEXT("data-layer"),
		TEXT("streaming"),
		TEXT("actor"),
		TEXT("component"),
		TEXT("reflection"),
	};
}

TSharedPtr<FJsonObject> ClaireonTool_UObjectReferencers::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("object_path"),
		TEXT("Path to a UObject. Accepts asset paths (/Game/...), native class paths (/Script/Module.ClassName, resolves to the CDO), and world-actor / sub-object paths (':PersistentLevel.ActorName')."),
		true);
	S.AddBoolean(TEXT("include_components"),
		TEXT("When target is an AActor, also search for references to each UActorComponent sub-object. Each hit reports target_kind ('actor' or 'component') and target_path. Default: true."));
	S.AddBoolean(TEXT("include_weak"),
		TEXT("Include weak-object references in addition to hard. Default: false."));
	S.AddBoolean(TEXT("include_archetype_refs"),
		TEXT("Include CDO / RF_ArchetypeObject candidates as referencers. Default: false (suppresses BP-CDO noise)."));
	S.AddBoolean(TEXT("include_editor_only"),
		TEXT("Include entries whose holding FProperty has CPF_EditorOnly. Default: false."));
	S.AddInteger(TEXT("max_results"),
		TEXT("Cap on emitted entries. Clamped to [1, 5000]. Default: 200. When the cap is reached, truncated=true on the response."));
	S.AddBoolean(TEXT("allow_load"),
		TEXT("When true (default), fall back to StaticLoadObject for asset paths not already in memory."));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_UObjectReferencers::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	using namespace ClaireonToolUObjectReferencers_Internal;

	FString ObjectPath;
	if (!Arguments->TryGetStringField(TEXT("object_path"), ObjectPath) || ObjectPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: object_path"));
	}

	bool bIncludeComponents = true;
	Arguments->TryGetBoolField(TEXT("include_components"), bIncludeComponents);

	bool bIncludeWeak = false;
	Arguments->TryGetBoolField(TEXT("include_weak"), bIncludeWeak);

	bool bIncludeArchetypeRefs = false;
	Arguments->TryGetBoolField(TEXT("include_archetype_refs"), bIncludeArchetypeRefs);

	bool bIncludeEditorOnly = false;
	Arguments->TryGetBoolField(TEXT("include_editor_only"), bIncludeEditorOnly);

	int32 MaxResults = 200;
	if (Arguments->HasTypedField<EJson::Number>(TEXT("max_results")))
	{
		MaxResults = (int32)Arguments->GetNumberField(TEXT("max_results"));
	}
	MaxResults = FMath::Clamp(MaxResults, 1, 5000);

	bool bAllowLoad = true;
	Arguments->TryGetBoolField(TEXT("allow_load"), bAllowLoad);

	FString ResolveError;
	UObject* Target = ResolveObject(ObjectPath, bAllowLoad, ResolveError);
	if (!Target)
	{
		return MakeErrorResult(ResolveError);
	}

	// Build target set: target itself + optional components.
	TArray<UObject*> TargetSet;
	TargetSet.Add(Target);

	TMap<UObject*, FString> TargetKindByObj;
	TMap<UObject*, FString> TargetPathByObj;
	TargetKindByObj.Add(Target, TEXT("object"));
	TargetPathByObj.Add(Target, Target->GetPathName());

	if (bIncludeComponents)
	{
		if (AActor* Actor = Cast<AActor>(Target))
		{
			TargetKindByObj[Target] = TEXT("actor");
			TArray<UActorComponent*> Components;
			Actor->GetComponents(Components);
			for (UActorComponent* Comp : Components)
			{
				if (Comp && !TargetKindByObj.Contains(Comp))
				{
					TargetSet.Add(Comp);
					TargetKindByObj.Add(Comp, TEXT("component"));
					TargetPathByObj.Add(Comp, Comp->GetPathName());
				}
			}
		}
	}

	// Build a fast lookup of UObject* -> index in TargetSet (FFindReferencersArchive accepts an array view).
	TSet<UObject*> TargetSetLookup(TargetSet);

	// Iterate all loaded UObjects and probe each candidate.
	TArray<FReferencerEntry> Hits;
	int32 ScannedCount = 0;
	bool bTruncated = false;

	for (TObjectIterator<UObject> It; It; ++It)
	{
		UObject* Candidate = *It;
		++ScannedCount;

		if (!IsValid(Candidate))
		{
			continue;
		}
		if (TargetSetLookup.Contains(Candidate))
		{
			continue; // Skip the target itself and (if applicable) its components.
		}
		if (!bIncludeArchetypeRefs && Candidate->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
		{
			continue;
		}

		// Reuse the archive across candidates would be ideal, but the constructor
		// freezes the target set and ResetPotentialReferencer is the supported
		// reset path. Build once, reset per candidate.
		FFindReferencersArchive Ar(Candidate, MakeArrayView(TargetSet.GetData(), TargetSet.Num()), bIncludeWeak);

		TMap<UObject*, int32> RefCounts;
		TMultiMap<UObject*, FProperty*> RefProps;
		const int32 NumRefdTargets = Ar.GetReferenceCounts(RefCounts, RefProps);
		if (NumRefdTargets == 0)
		{
			continue;
		}

		for (const TPair<UObject*, FProperty*>& Pair : RefProps)
		{
			UObject* HitTarget = Pair.Key;
			FProperty* Prop = Pair.Value;

			if (!bIncludeEditorOnly && Prop && Prop->HasAnyPropertyFlags(CPF_EditorOnly))
			{
				continue;
			}

			FReferencerEntry Entry;
			Entry.Referencer = Candidate;
			Entry.TargetHit = HitTarget;
			Entry.TargetKind = TargetKindByObj.FindRef(HitTarget);
			Entry.Property = Prop;
			Hits.Add(MoveTemp(Entry));

			if (Hits.Num() >= MaxResults)
			{
				bTruncated = true;
				break;
			}
		}

		if (bTruncated)
		{
			break;
		}
	}

	// Build JSON response.
	TSharedPtr<FJsonObject> TargetObj = MakeShared<FJsonObject>();
	TargetObj->SetStringField(TEXT("object_path"), Target->GetPathName());
	TargetObj->SetStringField(TEXT("class_path"), Target->GetClass()->GetPathName());

	TArray<TSharedPtr<FJsonValue>> ReferencersArr;
	for (const FReferencerEntry& Entry : Hits)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("object_path"), Entry.Referencer->GetPathName());
		Obj->SetStringField(TEXT("class_path"), Entry.Referencer->GetClass()->GetPathName());
		Obj->SetStringField(TEXT("property_path"), Entry.Property ? Entry.Property->GetName() : FString());
		Obj->SetStringField(TEXT("kind"), ClassifyPropertyKind(Entry.Property));
		const bool bEditorOnly = Entry.Property && Entry.Property->HasAnyPropertyFlags(CPF_EditorOnly);
		Obj->SetBoolField(TEXT("is_editor_only"), bEditorOnly);

		// Target reference (which object in the target set this hit landed on).
		Obj->SetStringField(TEXT("target_path"), TargetPathByObj.FindRef(Entry.TargetHit));
		Obj->SetStringField(TEXT("target_kind"), Entry.TargetKind);

		TArray<TSharedPtr<FJsonValue>> OuterChain;
		BuildOuterChain(Entry.Referencer, OuterChain);
		Obj->SetArrayField(TEXT("outer_chain"), OuterChain);

		ReferencersArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetObjectField(TEXT("target"), TargetObj);
	Data->SetArrayField(TEXT("referencers"), ReferencersArr);
	Data->SetNumberField(TEXT("scanned_object_count"), ScannedCount);
	Data->SetBoolField(TEXT("truncated"), bTruncated);

	const FString Summary = FString::Printf(
		TEXT("%s: %d referencer entr%s across %d scanned object%s%s"),
		*Target->GetName(),
		ReferencersArr.Num(),
		ReferencersArr.Num() == 1 ? TEXT("y") : TEXT("ies"),
		ScannedCount,
		ScannedCount == 1 ? TEXT("") : TEXT("s"),
		bTruncated ? TEXT(" (truncated)") : TEXT(""));

	return MakeSuccessResult(Data, Summary);
}
