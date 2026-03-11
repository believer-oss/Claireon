// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIETestAbility.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameplayAbilitySpec.h"
#include "GameplayEffect.h"

FString ClaireonTool_PIETestAbility::GetName() const
{
	return TEXT("pie_test_ability");
}

FString ClaireonTool_PIETestAbility::GetDescription() const
{
	return TEXT("Test whether a gameplay ability can be activated on an actor. "
		"Does NOT actually activate the ability â only checks activation requirements.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIETestAbility::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// actorId - required
	TSharedPtr<FJsonObject> ActorIdProp = MakeShared<FJsonObject>();
	ActorIdProp->SetStringField(TEXT("type"), TEXT("string"));
	ActorIdProp->SetStringField(TEXT("description"),
		TEXT("The stable actor ID to test the ability on (e.g., 'actor_0')"));
	Properties->SetObjectField(TEXT("actorId"), ActorIdProp);

	// abilityName - required
	TSharedPtr<FJsonObject> AbilityNameProp = MakeShared<FJsonObject>();
	AbilityNameProp->SetStringField(TEXT("type"), TEXT("string"));
	AbilityNameProp->SetStringField(TEXT("description"),
		TEXT("Name or substring of the ability class to find and test (e.g., 'GA_Jump', 'Sprint', 'Attack')"));
	Properties->SetObjectField(TEXT("abilityName"), AbilityNameProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	// Required fields
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actorId")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("abilityName")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIETestAbility::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString ActorId;
	if (!Arguments->TryGetStringField(TEXT("actorId"), ActorId))
	{
		return MakeErrorResult(TEXT("Missing required argument: actorId"));
	}

	FString AbilityName;
	if (!Arguments->TryGetStringField(TEXT("abilityName"), AbilityName))
	{
		return MakeErrorResult(TEXT("Missing required argument: abilityName"));
	}

	if (!GEditor)
	{
		return MakeErrorResult(TEXT("GEditor is not available"));
	}

	// Find the PIE world
	UWorld* PIEWorld = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE && Context.World())
		{
			PIEWorld = Context.World();
			break;
		}
	}

	if (!PIEWorld)
	{
		return MakeErrorResult(TEXT("No active PIE session"));
	}

	// Resolve the actor
	FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
	AActor* Actor = PIEManager.ResolveActorId(ActorId, PIEWorld);
	if (!Actor)
	{
		return MakeErrorResult(FString::Printf(TEXT("Actor not found for ID: %s"), *ActorId));
	}

	// Get the Ability System Component
	IAbilitySystemInterface* ASCInterface = Cast<IAbilitySystemInterface>(Actor);
	if (!ASCInterface)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Actor %s does not implement IAbilitySystemInterface"), *Actor->GetName()));
	}

	UAbilitySystemComponent* ASC = ASCInterface->GetAbilitySystemComponent();
	if (!ASC)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Actor %s has no AbilitySystemComponent"), *Actor->GetName()));
	}

	// Find ability specs that match the name
	FString MatchedAbilityName;
	bool bFound = false;
	bool bCanActivate = false;
	TArray<TSharedPtr<FJsonValue>> EffectsArray;

	TArray<FGameplayAbilitySpec>& Specs = ASC->GetActivatableAbilities();
	for (const FGameplayAbilitySpec& Spec : Specs)
	{
		if (!Spec.Ability)
		{
			continue;
		}

		FString SpecName = Spec.Ability->GetClass()->GetName();
		if (SpecName.Contains(AbilityName, ESearchCase::IgnoreCase))
		{
			bFound = true;
			MatchedAbilityName = SpecName;

			// Check if the ability can be activated by attempting a dry-run via the spec
			// UAbilitySystemComponent does not expose CanActivateAbility publicly;
			// use Spec.Ability->CanActivateAbility to check prerequisites.
			const FGameplayAbilityActorInfo* ActorInfo = ASC->AbilityActorInfo.Get();
			bCanActivate = Spec.Ability->CanActivateAbility(Spec.Handle, ActorInfo, nullptr, nullptr, nullptr);

			// Enumerate gameplay effects on the ASC for context
			TArray<FActiveGameplayEffectHandle> ActiveEffects = ASC->GetActiveEffects(FGameplayEffectQuery());
			for (const FActiveGameplayEffectHandle& EffectHandle : ActiveEffects)
			{
				const FActiveGameplayEffect* ActiveEffect = ASC->GetActiveGameplayEffect(EffectHandle);
				if (ActiveEffect && ActiveEffect->Spec.Def)
				{
					EffectsArray.Add(MakeShared<FJsonValueString>(
						ActiveEffect->Spec.Def->GetClass()->GetName()));
				}
			}
			break;
		}
	}

	if (!bFound)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("No ability matching '%s' found on actor %s"), *AbilityName, *Actor->GetName()));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("ability"), MatchedAbilityName);
	Data->SetBoolField(TEXT("activated"), bCanActivate);
	Data->SetStringField(TEXT("result"), bCanActivate ? TEXT("can_activate") : TEXT("blocked"));
	Data->SetArrayField(TEXT("effects_applied"), EffectsArray);

	FString Summary = FString::Printf(TEXT("%s %s"),
		*MatchedAbilityName,
		bCanActivate ? TEXT("can be activated") : TEXT("cannot be activated (blocked)"));

	return MakeSuccessResult(Data, Summary);
}
