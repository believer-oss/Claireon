// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_GasInspect.h"

#include "ClaireonGasToolCommon.h"

#include "AbilitySystemComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameplayAbilitySpec.h"
#include "GameplayEffect.h"
#include "Abilities/GameplayAbility.h"

FString ClaireonTool_GasInspect::GetCategory() const { return TEXT("gas"); }
// "runtime_inspect" (not "inspect") so host projects can register an asset-side
// GAS inspector as gas_inspect without colliding with this runtime ASC snapshot.
// Parallels the statetree_runtime_inspect naming.
FString ClaireonTool_GasInspect::GetOperation() const { return TEXT("runtime_inspect"); }

FString ClaireonTool_GasInspect::GetDescription() const
{
	return TEXT("Inspect a live PIE actor's AbilitySystemComponent: granted abilities, active gameplay effects "
		"(with stable effect_handle_id for gas_remove_effect), attributes (base + current), and owned "
		"gameplay tags; narrow with sections. Read-only and non-session, but requires a live PIE session. "
		"net_mode defaults to the server world; pass 'client' to see client-side prediction.");
}

EClaireonToolSessionMode ClaireonTool_GasInspect::GetSessionMode() const
{
	return EClaireonToolSessionMode::ReadOnly;
}

TSharedPtr<FJsonObject> ClaireonTool_GasInspect::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	ClaireonGasToolCommon::AddCommonSchemaParams(Properties);

	// Optional sections filter.
	TSharedPtr<FJsonObject> SectionsProp = MakeShared<FJsonObject>();
	SectionsProp->SetStringField(TEXT("type"), TEXT("array"));
	SectionsProp->SetStringField(TEXT("description"),
		TEXT("Optional subset of sections to return. Any of 'abilities', 'effects', 'attributes', "
			 "'tags'. Default: all four."));
	TSharedPtr<FJsonObject> SectionsItems = MakeShared<FJsonObject>();
	SectionsItems->SetStringField(TEXT("type"), TEXT("string"));
	SectionsProp->SetObjectField(TEXT("items"), SectionsItems);
	Properties->SetObjectField(TEXT("sections"), SectionsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actorId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_GasInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	ClaireonGasToolCommon::FGasTarget Target;
	FString Error;
	if (!ClaireonGasToolCommon::ResolveTarget(Arguments, Target, Error))
	{
		return MakeErrorResult(Error);
	}
	UAbilitySystemComponent* ASC = Target.ASC;

	// Section selection (default all).
	bool bWantAbilities = true, bWantEffects = true, bWantAttributes = true, bWantTags = true;
	const TArray<TSharedPtr<FJsonValue>>* SectionsArray = nullptr;
	if (Arguments->TryGetArrayField(TEXT("sections"), SectionsArray) && SectionsArray)
	{
		bWantAbilities = bWantEffects = bWantAttributes = bWantTags = false;
		for (const TSharedPtr<FJsonValue>& Value : *SectionsArray)
		{
			const FString Section = Value->AsString();
			if (Section.Equals(TEXT("abilities"), ESearchCase::IgnoreCase)) { bWantAbilities = true; }
			else if (Section.Equals(TEXT("effects"), ESearchCase::IgnoreCase)) { bWantEffects = true; }
			else if (Section.Equals(TEXT("attributes"), ESearchCase::IgnoreCase)) { bWantAttributes = true; }
			else if (Section.Equals(TEXT("tags"), ESearchCase::IgnoreCase)) { bWantTags = true; }
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	int32 AbilityCount = 0, EffectCount = 0, AttributeCount = 0, TagCount = 0;

	if (bWantAbilities)
	{
		TArray<TSharedPtr<FJsonValue>> AbilitiesJson;
		for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
		{
			if (!Spec.Ability) { continue; }
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("class"), Spec.Ability->GetClass()->GetName());
			Obj->SetStringField(TEXT("spec_handle_id"), Spec.Handle.ToString());
			Obj->SetNumberField(TEXT("level"), Spec.Level);
			Obj->SetNumberField(TEXT("input_id"), Spec.InputID);
			Obj->SetBoolField(TEXT("is_active"), Spec.IsActive());
			const UObject* Source = Spec.SourceObject.Get();
			Obj->SetStringField(TEXT("source_object"), IsValid(Source) ? Source->GetName() : TEXT(""));
			AbilitiesJson.Add(MakeShared<FJsonValueObject>(Obj));
		}
		AbilityCount = AbilitiesJson.Num();
		Data->SetArrayField(TEXT("abilities"), AbilitiesJson);
	}

	if (bWantEffects)
	{
		const float WorldTime = IsValid(Target.World) ? Target.World->GetTimeSeconds() : 0.0f;
		TArray<TSharedPtr<FJsonValue>> EffectsJson;
		TArray<FActiveGameplayEffectHandle> Handles = ASC->GetActiveEffects(FGameplayEffectQuery());
		for (const FActiveGameplayEffectHandle& Handle : Handles)
		{
			const FActiveGameplayEffect* Active = ASC->GetActiveGameplayEffect(Handle);
			if (!Active || !Active->Spec.Def) { continue; }
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("effect_handle_id"), Handle.ToString());
			Obj->SetStringField(TEXT("class"), Active->Spec.Def->GetClass()->GetName());
			Obj->SetNumberField(TEXT("duration_remaining"), Active->GetTimeRemaining(WorldTime));
			Obj->SetNumberField(TEXT("total_duration"), Active->GetDuration());
			Obj->SetNumberField(TEXT("stacks"), Active->Spec.GetStackCount());
			Obj->SetNumberField(TEXT("level"), Active->Spec.GetLevel());

			FGameplayTagContainer GrantedTags;
			Active->Spec.GetAllGrantedTags(GrantedTags);
			TArray<TSharedPtr<FJsonValue>> GrantedJson;
			for (const FGameplayTag& Tag : GrantedTags)
			{
				GrantedJson.Add(MakeShared<FJsonValueString>(Tag.ToString()));
			}
			Obj->SetArrayField(TEXT("granted_tags"), GrantedJson);

			const UObject* Source = Active->Spec.GetContext().GetSourceObject();
			Obj->SetStringField(TEXT("source"), IsValid(Source) ? Source->GetName() : TEXT(""));
			EffectsJson.Add(MakeShared<FJsonValueObject>(Obj));
		}
		EffectCount = EffectsJson.Num();
		Data->SetArrayField(TEXT("active_effects"), EffectsJson);
	}

	if (bWantAttributes)
	{
		TArray<FGameplayAttribute> Attributes;
		ASC->GetAllAttributes(Attributes);
		TArray<TSharedPtr<FJsonValue>> AttrsJson;
		for (const FGameplayAttribute& Attr : Attributes)
		{
			if (!Attr.IsValid()) { continue; }
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			const UClass* SetClass = Attr.GetAttributeSetClass();
			Obj->SetStringField(TEXT("set"), IsValid(SetClass) ? SetClass->GetName() : TEXT(""));
			Obj->SetStringField(TEXT("name"), Attr.GetName());
			Obj->SetNumberField(TEXT("base_value"), ASC->GetNumericAttributeBase(Attr));
			Obj->SetNumberField(TEXT("current_value"), ASC->GetNumericAttribute(Attr));
			AttrsJson.Add(MakeShared<FJsonValueObject>(Obj));
		}
		AttributeCount = AttrsJson.Num();
		Data->SetArrayField(TEXT("attributes"), AttrsJson);
	}

	if (bWantTags)
	{
		FGameplayTagContainer OwnedTags;
		ASC->GetOwnedGameplayTags(OwnedTags);
		TArray<TSharedPtr<FJsonValue>> TagsJson;
		for (const FGameplayTag& Tag : OwnedTags)
		{
			TagsJson.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		TagCount = TagsJson.Num();
		Data->SetArrayField(TEXT("owned_tags"), TagsJson);
	}

	const FString Summary = FString::Printf(
		TEXT("%d abilities, %d effects, %d attributes, %d tags on %s"),
		AbilityCount, EffectCount, AttributeCount, TagCount, *Target.Actor->GetName());

	return MakeSuccessResult(Data, Summary);
}
