// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_GasApplyEffect.h"

#include "ClaireonGasToolCommon.h"

#include "AbilitySystemComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "GameplayEffect.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagContainer.h"

FString ClaireonTool_GasApplyEffect::GetCategory() const { return TEXT("gas"); }
FString ClaireonTool_GasApplyEffect::GetOperation() const { return TEXT("apply_effect"); }

FString ClaireonTool_GasApplyEffect::GetDescription() const
{
	return TEXT("Apply a GameplayEffect to a live PIE actor's ASC. Identify the GE by effect_class_path (e.g. "
		"'/Game/.../GE_X.GE_X_C') or effect_name (substring of a loaded GE class). Supports level, "
		"SetByCaller magnitudes, and a duration override (HasDuration policy only). Requires a live PIE "
		"session; net_mode defaults to the server world so the effect replicates. Returns effect_handle_id.");
}

EClaireonToolSessionMode ClaireonTool_GasApplyEffect::GetSessionMode() const
{
	return EClaireonToolSessionMode::ReadOnly;
}

TSharedPtr<FJsonObject> ClaireonTool_GasApplyEffect::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	ClaireonGasToolCommon::AddCommonSchemaParams(Properties);

	TSharedPtr<FJsonObject> PathProp = MakeShared<FJsonObject>();
	PathProp->SetStringField(TEXT("type"), TEXT("string"));
	PathProp->SetStringField(TEXT("description"),
		TEXT("Authoritative generated-class path of the GameplayEffect, e.g. "
			 "'/Game/BP/Progression/Aspects/Seismic/GE_Seismic1.GE_Seismic1_C'."));
	Properties->SetObjectField(TEXT("effect_class_path"), PathProp);

	TSharedPtr<FJsonObject> NameProp = MakeShared<FJsonObject>();
	NameProp->SetStringField(TEXT("type"), TEXT("string"));
	NameProp->SetStringField(TEXT("description"),
		TEXT("Convenience substring matched against loaded UGameplayEffect subclasses. "
			 "Errors if ambiguous or not loaded; prefer effect_class_path."));
	Properties->SetObjectField(TEXT("effect_name"), NameProp);

	TSharedPtr<FJsonObject> LevelProp = MakeShared<FJsonObject>();
	LevelProp->SetStringField(TEXT("type"), TEXT("number"));
	LevelProp->SetStringField(TEXT("description"), TEXT("Effect level (default 1)."));
	Properties->SetObjectField(TEXT("level"), LevelProp);

	TSharedPtr<FJsonObject> MagsProp = MakeShared<FJsonObject>();
	MagsProp->SetStringField(TEXT("type"), TEXT("object"));
	MagsProp->SetStringField(TEXT("description"),
		TEXT("Optional SetByCaller magnitudes: { \"<GameplayTag>\": <number> }. Aspects use these "
			 "for damage numbers etc. Keys must be registered gameplay tags."));
	Properties->SetObjectField(TEXT("magnitudes"), MagsProp);

	TSharedPtr<FJsonObject> DurProp = MakeShared<FJsonObject>();
	DurProp->SetStringField(TEXT("type"), TEXT("number"));
	DurProp->SetStringField(TEXT("description"),
		TEXT("Optional duration override in seconds. Only meaningful for a HasDuration effect."));
	Properties->SetObjectField(TEXT("duration_override"), DurProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actorId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_GasApplyEffect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	ClaireonGasToolCommon::FGasTarget Target;
	FString Error;
	if (!ClaireonGasToolCommon::ResolveTarget(Arguments, Target, Error))
	{
		return MakeErrorResult(Error);
	}
	UAbilitySystemComponent* ASC = Target.ASC;

	TSubclassOf<UGameplayEffect> GEClass = ClaireonGasToolCommon::ResolveEffectClass(Arguments, Error);
	if (!IsValid(GEClass))
	{
		return MakeErrorResult(Error);
	}

	double LevelValue = 1.0;
	Arguments->TryGetNumberField(TEXT("level"), LevelValue);
	const float Level = static_cast<float>(LevelValue);

	FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
	FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(GEClass, Level, Context);
	if (!Spec.IsValid() || !Spec.Data.IsValid())
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Failed to build an outgoing spec for effect '%s'."), *GEClass->GetName()));
	}

	TArray<FString> Warnings;

	// SetByCaller magnitudes.
	const TSharedPtr<FJsonObject>* MagnitudesObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("magnitudes"), MagnitudesObj) && MagnitudesObj && MagnitudesObj->IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*MagnitudesObj)->Values)
		{
			double MagValue = 0.0;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetNumber(MagValue))
			{
				Warnings.Add(FString::Printf(TEXT("magnitudes['%s'] is not a number; skipped."), *Pair.Key));
				continue;
			}
			const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*Pair.Key), /*ErrorIfNotFound=*/false);
			if (!Tag.IsValid())
			{
				Warnings.Add(FString::Printf(TEXT("magnitude tag '%s' is not a registered gameplay tag; skipped."), *Pair.Key));
				continue;
			}
			Spec.Data->SetSetByCallerMagnitude(Tag, static_cast<float>(MagValue));
		}
	}

	// Optional duration override.
	double DurationOverride = 0.0;
	if (Arguments->TryGetNumberField(TEXT("duration_override"), DurationOverride))
	{
		if (Spec.Data->Def && Spec.Data->Def->DurationPolicy == EGameplayEffectDurationType::HasDuration)
		{
			Spec.Data->SetDuration(static_cast<float>(DurationOverride), /*bLockDuration=*/true);
		}
		else
		{
			Warnings.Add(TEXT("duration_override ignored: effect is not a HasDuration effect."));
		}
	}

	if (!ASC->IsOwnerActorAuthoritative())
	{
		Warnings.Add(TEXT("ASC is not authoritative (client world); effect is client-predicted only. "
			"Pass net_mode='server' to apply the authoritative effect."));
	}

	const FActiveGameplayEffectHandle Handle = ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("class"), GEClass->GetName());
	Data->SetBoolField(TEXT("applied"), Handle.WasSuccessfullyApplied() || Handle.IsValid());
	// Instant effects apply without leaving a persistent handle -> empty id.
	Data->SetStringField(TEXT("effect_handle_id"), Handle.IsValid() ? Handle.ToString() : FString());

	FGameplayTagContainer GrantedTags;
	Spec.Data->GetAllGrantedTags(GrantedTags);
	TArray<TSharedPtr<FJsonValue>> GrantedJson;
	for (const FGameplayTag& Tag : GrantedTags)
	{
		GrantedJson.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}
	Data->SetArrayField(TEXT("granted_tags"), GrantedJson);

	FString Summary;
	if (Handle.IsValid())
	{
		Summary = FString::Printf(TEXT("Applied %s (handle %s) to %s"),
			*GEClass->GetName(), *Handle.ToString(), *Target.Actor->GetName());
	}
	else if (Handle.WasSuccessfullyApplied())
	{
		Summary = FString::Printf(TEXT("Applied instant effect %s to %s (no persistent handle)"),
			*GEClass->GetName(), *Target.Actor->GetName());
	}
	else
	{
		Summary = FString::Printf(TEXT("Effect %s did not apply to %s (filtered by application rules)"),
			*GEClass->GetName(), *Target.Actor->GetName());
	}

	FToolResult Result = MakeSuccessResult(Data, Summary);
	Result.Warnings = MoveTemp(Warnings);
	return Result;
}
