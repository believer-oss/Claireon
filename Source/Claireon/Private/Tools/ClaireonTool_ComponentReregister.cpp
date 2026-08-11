// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ComponentReregister.h"

#include "ClaireonLog.h"
#include "ClaireonPathResolver.h"
#include "Tools/FToolSchemaBuilder.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace ClaireonToolComponentReregister_Internal
{
	/**
	 * RAII pre/post pair. UActorComponent::PreEditChange checkf()s if a prior pre had
	 * no matching post, so the post must run on every exit path -- including one taken
	 * by an early return added later.
	 */
	struct FScopedComponentChangeNotify
	{
		UActorComponent* Component = nullptr;
		FProperty* Property = nullptr;

		FScopedComponentChangeNotify(UActorComponent* InComponent, FProperty* InProperty)
			: Component(InComponent)
			, Property(InProperty)
		{
			if (IsValid(Component))
			{
				Component->PreEditChange(Property);
			}
		}

		~FScopedComponentChangeNotify()
		{
			if (IsValid(Component))
			{
				FPropertyChangedEvent ChangedEvent(Property, EPropertyChangeType::ValueSet);
				Component->PostEditChangeProperty(ChangedEvent);
			}
		}
	};

	/** Comma-joined component names on an actor, for the disambiguation error. */
	FString ListComponentNames(AActor* Actor)
	{
		TArray<FString> Names;
		for (UActorComponent* Component : Actor->GetComponents())
		{
			if (IsValid(Component))
			{
				Names.Add(Component->GetName());
			}
		}
		Names.Sort();
		return FString::Join(Names, TEXT(", "));
	}
}

FString ClaireonTool_ComponentReregister::GetOperation() const
{
	return TEXT("reregister");
}

FString ClaireonTool_ComponentReregister::GetDescription() const
{
	return TEXT(
		"Run the details-panel change cycle (PreEditChange + PostEditChangeProperty) on an actor "
		"component, re-registering it and rebuilding property-derived engine state. Use after a raw "
		"write that bypassed change notification, e.g. Python set_editor_property on "
		"bCanEverAffectNavigation, which desyncs the nav octree. Pass changed_property so the "
		"name-dispatched handlers run. Immediate-mode: no session.");
}

TArray<FString> ClaireonTool_ComponentReregister::GetSearchKeywords() const
{
	return {
		TEXT("reregister"),
		TEXT("re-register"),
		TEXT("register"),
		TEXT("component"),
		TEXT("navigation"),
		TEXT("nav"),
		TEXT("octree"),
		TEXT("desync"),
		TEXT("posteditchange"),
		TEXT("notify"),
		TEXT("refresh"),
		TEXT("pie"),
	};
}

TSharedPtr<FJsonObject> ClaireonTool_ComponentReregister::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("object_path"),
		TEXT("Path to the component itself, or to the actor that owns it (then pass component_name). "
			 "Accepts the same forms as uobject_inspect, including live PIE paths like "
			 "'/Game/Maps/UEDPIE_0_Map.Map:PersistentLevel.MyActor_1'."),
		true);
	S.AddString(TEXT("component_name"),
		TEXT("Component to target when object_path names an actor. Matched case-insensitively against "
			 "the component's name. Ignored when object_path already names a component."));
	S.AddString(TEXT("changed_property"),
		TEXT("Name of the property that was written, e.g. 'bCanEverAffectNavigation'. Drives the "
			 "name-dispatched handlers in UActorComponent::ConsolidatedPostEditChange -- the nav-octree "
			 "update only runs when this names the nav flag. Omit for a plain unregister/re-register."));
	S.AddBoolean(TEXT("allow_load"),
		TEXT("When true (default), fall back to StaticLoadObject for asset paths not already in memory."));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ComponentReregister::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	using namespace ClaireonToolComponentReregister_Internal;

	// 1) Parse args.
	FString ObjectPath;
	if (!Arguments->TryGetStringField(TEXT("object_path"), ObjectPath) || ObjectPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: object_path"));
	}

	FString ComponentName;
	Arguments->TryGetStringField(TEXT("component_name"), ComponentName);

	FString ChangedPropertyName;
	Arguments->TryGetStringField(TEXT("changed_property"), ChangedPropertyName);

	bool bAllowLoad = true;
	Arguments->TryGetBoolField(TEXT("allow_load"), bAllowLoad);

	// 2) Resolve. Shared with uobject_inspect / uobject_set_property, so a path that
	// inspects also re-registers.
	FString ResolveError;
	UObject* Object = ClaireonPathResolver::ResolveObjectFromPath(ObjectPath, bAllowLoad, ResolveError);
	if (!IsValid(Object))
	{
		return MakeErrorResult(ResolveError);
	}

	// 3) Narrow to a component.
	UActorComponent* Component = Cast<UActorComponent>(Object);
	if (!IsValid(Component))
	{
		AActor* Actor = Cast<AActor>(Object);
		if (!IsValid(Actor))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("'%s' is a %s -- expected an actor component, or an actor plus component_name."),
				*ObjectPath, *Object->GetClass()->GetName()));
		}

		if (ComponentName.IsEmpty())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("'%s' is an actor; pass component_name to pick a component. Available: %s"),
				*ObjectPath, *ListComponentNames(Actor)));
		}

		for (UActorComponent* Candidate : Actor->GetComponents())
		{
			if (IsValid(Candidate) && Candidate->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
			{
				Component = Candidate;
				break;
			}
		}

		if (!IsValid(Component))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("No component named '%s' on '%s'. Available: %s"),
				*ComponentName, *ObjectPath, *ListComponentNames(Actor)));
		}
	}

	// 4) Resolve the named property, if one was given. A typo here is silent otherwise:
	// the cycle would still run but the handler the caller wanted would not, and the
	// result would look like a success.
	FProperty* ChangedProperty = nullptr;
	if (!ChangedPropertyName.IsEmpty())
	{
		ChangedProperty = Component->GetClass()->FindPropertyByName(FName(*ChangedPropertyName));
		if (!ChangedProperty)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Property '%s' not found on component class '%s'."),
				*ChangedPropertyName, *Component->GetClass()->GetName()));
		}
	}

	// 5) Snapshot the observable nav state so the caller can see the cycle land.
	const bool bWasRegistered = Component->IsRegistered();
	const bool bNavFlagBefore = Component->CanEverAffectNavigation();
	const bool bNavRelevantBefore = Component->bNavigationRelevant;

	// 6) Run the cycle inside a transaction, mirroring a details-panel edit.
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Re-register Component")));
		Component->Modify();
		FScopedComponentChangeNotify Notify(Component, ChangedProperty);
	}

	const bool bIsRegistered = Component->IsRegistered();
	const bool bNavRelevantAfter = Component->bNavigationRelevant;

	UE_LOG(LogClaireon, Log,
		TEXT("[audit] component_reregister: %s (changed_property=%s) registered %s->%s, navigation_relevant %s->%s"),
		*Component->GetPathName(),
		ChangedPropertyName.IsEmpty() ? TEXT("<none>") : *ChangedPropertyName,
		bWasRegistered ? TEXT("true") : TEXT("false"),
		bIsRegistered ? TEXT("true") : TEXT("false"),
		bNavRelevantBefore ? TEXT("true") : TEXT("false"),
		bNavRelevantAfter ? TEXT("true") : TEXT("false"));

	// 7) Response.
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("component_path"), Component->GetPathName());
	Data->SetStringField(TEXT("component_class"), Component->GetClass()->GetName());
	Data->SetBoolField(TEXT("was_registered"), bWasRegistered);
	Data->SetBoolField(TEXT("is_registered"), bIsRegistered);
	Data->SetBoolField(TEXT("can_ever_affect_navigation"), bNavFlagBefore);
	Data->SetBoolField(TEXT("navigation_relevant_before"), bNavRelevantBefore);
	Data->SetBoolField(TEXT("navigation_relevant_after"), bNavRelevantAfter);
	if (!ChangedPropertyName.IsEmpty())
	{
		Data->SetStringField(TEXT("changed_property"), ChangedPropertyName);
	}

	const FString What = ChangedPropertyName.IsEmpty()
		? FString(TEXT("full re-register"))
		: FString::Printf(TEXT("changed_property=%s"), *ChangedPropertyName);
	const FString Summary = FString::Printf(
		TEXT("component_reregister: %s (%s)"), *Component->GetName(), *What);

	FToolResult Result = MakeSuccessResult(Data, Summary);

	// An unregistered component takes no reregister context and its nav handler
	// early-outs on bRegistered, so the call did far less than the name suggests.
	// Say so rather than reporting a clean success.
	if (!bWasRegistered)
	{
		Result.Warnings.Add(FString::Printf(
			TEXT("Component '%s' was not registered, so no re-register occurred and "
				 "HandleCanEverAffectNavigationChange's registered-only path did not run. "
				 "Re-register affects live world/PIE components; an SCS template or CDO component has nothing to sync."),
			*Component->GetName()));
	}

	// The nav handlers are dispatched by property name -- a full re-register does not
	// reach them. Point at the argument that does. Success-path guidance, so LATCHED:
	// this teaches a parameter, and a bulk repair loop over many components would
	// otherwise repeat the same lesson on every call.
	if (ChangedPropertyName.IsEmpty() &&
		ShouldEmitLatchedHint(FName(TEXT("component_reregister_no_changed_property"))))
	{
		Result.Hint = MakeGuidanceHint(
			GetName(),
			TEXT("Ran a full re-register with no changed_property, so UActorComponent's "
				 "name-dispatched handlers did not run. To resync the navigation octree after a raw "
				 "write, re-issue with changed_property='bCanEverAffectNavigation'."));
	}

	return Result;
}
