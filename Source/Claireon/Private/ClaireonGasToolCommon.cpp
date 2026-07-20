// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonGasToolCommon.h"

#include "ClaireonPIEManager.h"
#include "ClaireonPIEWorldResolver.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "AbilitySystemInterface.h"
#include "AttributeSet.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "GameplayEffect.h"
#include "Abilities/GameplayAbility.h"
#include "UObject/UObjectIterator.h"

namespace ClaireonGasToolCommonInternal
{
	// Skeleton / reinstanced / trashed transient UClasses that must never be
	// offered as a name match.
	static bool IsTransientClass(const UClass* Class)
	{
		const FString Name = Class->GetName();
		return Name.StartsWith(TEXT("SKEL_"))
			|| Name.StartsWith(TEXT("REINST_"))
			|| Name.StartsWith(TEXT("TRASHCLASS"))
			|| Name.Contains(TEXT("HOTRELOADED_"));
	}

	// Shared name/path resolver for the two class-typed params. `PathField` and
	// `NameField` name the two accepted arguments; `Base` bounds the search.
	static UClass* ResolveClassByPathOrName(
		const TSharedPtr<FJsonObject>& Arguments,
		const TCHAR* PathField,
		const TCHAR* NameField,
		UClass* Base,
		FString& OutError)
	{
		FString ClassPath;
		if (Arguments->TryGetStringField(PathField, ClassPath) && !ClassPath.IsEmpty())
		{
			UClass* Loaded = LoadClass<UObject>(nullptr, *ClassPath);
			if (!Loaded)
			{
				OutError = FString::Printf(
					TEXT("Could not load a class from %s='%s'. Expected a generated-class path "
						 "ending in '_C' (e.g. '/Game/.../GE_X.GE_X_C')."),
					PathField, *ClassPath);
				return nullptr;
			}
			if (!Loaded->IsChildOf(Base))
			{
				OutError = FString::Printf(TEXT("Class '%s' is not a %s subclass."),
					*ClassPath, *Base->GetName());
				return nullptr;
			}
			return Loaded;
		}

		FString ClassName;
		if (Arguments->TryGetStringField(NameField, ClassName) && !ClassName.IsEmpty())
		{
			TArray<UClass*> Matches;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* Candidate = *It;
				if (Candidate == Base || !Candidate->IsChildOf(Base)) { continue; }
				if (IsTransientClass(Candidate)) { continue; }
				if (Candidate->GetName().Contains(ClassName, ESearchCase::IgnoreCase))
				{
					Matches.AddUnique(Candidate);
				}
			}
			if (Matches.Num() == 0)
			{
				OutError = FString::Printf(
					TEXT("No loaded %s subclass matches %s='%s'. Load the asset first or pass "
						 "the authoritative class path instead."),
					*Base->GetName(), NameField, *ClassName);
				return nullptr;
			}
			if (Matches.Num() > 1)
			{
				FString Names;
				for (const UClass* M : Matches)
				{
					Names += (Names.IsEmpty() ? TEXT("") : TEXT(", "));
					Names += M->GetName();
				}
				OutError = FString::Printf(
					TEXT("%s='%s' is ambiguous (%d matches: %s). Pass a class path instead."),
					NameField, *ClassName, Matches.Num(), *Names);
				return nullptr;
			}
			return Matches[0];
		}

		OutError = FString::Printf(TEXT("Provide either %s or %s."), PathField, NameField);
		return nullptr;
	}
}

using namespace ClaireonGasToolCommonInternal;

namespace ClaireonGasToolCommon
{

void AddCommonSchemaParams(const TSharedPtr<FJsonObject>& Properties)
{
	if (!Properties.IsValid())
	{
		return;
	}

	TSharedPtr<FJsonObject> ActorIdProp = MakeShared<FJsonObject>();
	ActorIdProp->SetStringField(TEXT("type"), TEXT("string"));
	ActorIdProp->SetStringField(TEXT("description"),
		TEXT("Stable actor ID (e.g. 'actor_0') minted by pie_get_player_pawn / pie_get_actor. "
			 "Actor IDs are per-world: an ID minted on the client world will not resolve on the "
			 "server world, so re-resolve on the world you target with net_mode."));
	Properties->SetObjectField(TEXT("actorId"), ActorIdProp);

	// pie_instance + net_mode.
	ClaireonPIEWorldResolver::AddSchemaParams(Properties);
}

bool ResolveTarget(const TSharedPtr<FJsonObject>& Arguments, FGasTarget& OutTarget, FString& OutError)
{
	OutTarget = FGasTarget();

	if (!Arguments.IsValid())
	{
		OutError = TEXT("Missing arguments.");
		return false;
	}

	FString ActorId;
	if (!Arguments->TryGetStringField(TEXT("actorId"), ActorId) || ActorId.IsEmpty())
	{
		OutError = TEXT("Missing required argument: actorId");
		return false;
	}

	if (!GEditor || !GEditor->IsPlaySessionInProgress())
	{
		OutError = TEXT("No active PIE session. Start Play-in-Editor first (these tools mutate/read "
			"live runtime GAS state, not on-disk assets).");
		return false;
	}

	// GAS is authority-driven: default to the server world when the caller did
	// not specify a role, so mutations replicate and inspection reads truth.
	if (!Arguments->HasField(TEXT("net_mode")))
	{
		Arguments->SetStringField(TEXT("net_mode"), TEXT("server"));
	}

	UWorld* World = ClaireonPIEWorldResolver::ResolvePIEWorld(Arguments, OutError);
	if (!World)
	{
		return false;
	}
	OutTarget.World = World;

	AActor* Actor = FClaireonPIEManager::Get().ResolveActorId(ActorId, World);
	if (!Actor)
	{
		OutError = FString::Printf(
			TEXT("Actor not found for ID '%s' on the resolved world. Actor IDs are per-world; "
				 "re-resolve the actor on the world you target with net_mode."), *ActorId);
		return false;
	}
	OutTarget.Actor = Actor;

	UAbilitySystemComponent* ASC = nullptr;
	if (IAbilitySystemInterface* ASCInterface = Cast<IAbilitySystemInterface>(Actor))
	{
		ASC = ASCInterface->GetAbilitySystemComponent();
	}
	if (!ASC)
	{
		ASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Actor);
	}
	if (!ASC)
	{
		OutError = FString::Printf(
			TEXT("Actor '%s' (%s) has no AbilitySystemComponent."),
			*Actor->GetName(), *ActorId);
		return false;
	}
	OutTarget.ASC = ASC;

	return true;
}

TSubclassOf<UGameplayEffect> ResolveEffectClass(const TSharedPtr<FJsonObject>& Arguments, FString& OutError)
{
	UClass* Resolved = ResolveClassByPathOrName(
		Arguments, TEXT("effect_class_path"), TEXT("effect_name"),
		UGameplayEffect::StaticClass(), OutError);
	return Resolved ? TSubclassOf<UGameplayEffect>(Resolved) : nullptr;
}

TSubclassOf<UGameplayAbility> ResolveAbilityClass(const TSharedPtr<FJsonObject>& Arguments, FString& OutError)
{
	UClass* Resolved = ResolveClassByPathOrName(
		Arguments, TEXT("ability_class_path"), TEXT("ability_name"),
		UGameplayAbility::StaticClass(), OutError);
	return Resolved ? TSubclassOf<UGameplayAbility>(Resolved) : nullptr;
}

bool ResolveAttribute(UAbilitySystemComponent* ASC, const FString& Name, FGameplayAttribute& OutAttr, FString& OutError)
{
	if (!ASC)
	{
		OutError = TEXT("No AbilitySystemComponent.");
		return false;
	}
	if (Name.IsEmpty())
	{
		OutError = TEXT("Missing required argument: attribute");
		return false;
	}

	TArray<FGameplayAttribute> Attributes;
	ASC->GetAllAttributes(Attributes);

	// Exact (case-insensitive) name match wins outright.
	for (const FGameplayAttribute& Attr : Attributes)
	{
		if (Attr.GetName().Equals(Name, ESearchCase::IgnoreCase))
		{
			OutAttr = Attr;
			return true;
		}
	}

	// Otherwise a unique substring match.
	TArray<FGameplayAttribute> Partial;
	for (const FGameplayAttribute& Attr : Attributes)
	{
		if (Attr.GetName().Contains(Name, ESearchCase::IgnoreCase))
		{
			Partial.Add(Attr);
		}
	}
	if (Partial.Num() == 1)
	{
		OutAttr = Partial[0];
		return true;
	}
	if (Partial.Num() > 1)
	{
		FString Names;
		for (const FGameplayAttribute& Attr : Partial)
		{
			Names += (Names.IsEmpty() ? TEXT("") : TEXT(", "));
			Names += AttributeDisplayName(Attr);
		}
		OutError = FString::Printf(TEXT("Attribute '%s' is ambiguous (%d matches: %s)."),
			*Name, Partial.Num(), *Names);
		return false;
	}

	OutError = FString::Printf(TEXT("No attribute matching '%s' on this ASC."), *Name);
	return false;
}

FString AttributeDisplayName(const FGameplayAttribute& Attr)
{
	const UClass* SetClass = Attr.GetAttributeSetClass();
	if (SetClass)
	{
		return FString::Printf(TEXT("%s.%s"), *SetClass->GetName(), *Attr.GetName());
	}
	return Attr.GetName();
}

} // namespace ClaireonGasToolCommon
