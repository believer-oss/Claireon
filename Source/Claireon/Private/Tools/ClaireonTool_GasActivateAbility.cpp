// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_GasActivateAbility.h"

#include "ClaireonGasToolCommon.h"

#include "AbilitySystemComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "GameplayAbilitySpec.h"
#include "GameplayTagContainer.h"
#include "Abilities/GameplayAbility.h"

FString ClaireonTool_GasActivateAbility::GetCategory() const { return TEXT("gas"); }
FString ClaireonTool_GasActivateAbility::GetOperation() const { return TEXT("activate_ability"); }

FString ClaireonTool_GasActivateAbility::GetDescription() const
{
	return TEXT("Activate an already-granted ability on a live PIE actor's ASC (grant it first with "
		"gas_grant_ability). Address it by spec_handle_id (from gas_runtime_inspect / gas_grant_ability) or "
		"ability_name (substring of a granted spec's class). Reports why activation was blocked when it "
		"fails. Defaults to the server world. Requires PIE.");
}

EClaireonToolSessionMode ClaireonTool_GasActivateAbility::GetSessionMode() const
{
	return EClaireonToolSessionMode::ReadOnly;
}

TSharedPtr<FJsonObject> ClaireonTool_GasActivateAbility::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	ClaireonGasToolCommon::AddCommonSchemaParams(Properties);

	TSharedPtr<FJsonObject> HandleProp = MakeShared<FJsonObject>();
	HandleProp->SetStringField(TEXT("type"), TEXT("string"));
	HandleProp->SetStringField(TEXT("description"),
		TEXT("spec_handle_id from gas_runtime_inspect / gas_grant_ability. Activates exactly that spec."));
	Properties->SetObjectField(TEXT("spec_handle_id"), HandleProp);

	TSharedPtr<FJsonObject> NameProp = MakeShared<FJsonObject>();
	NameProp->SetStringField(TEXT("type"), TEXT("string"));
	NameProp->SetStringField(TEXT("description"),
		TEXT("Substring matched against the actor's granted ability spec classes (e.g. 'GA_Jump')."));
	Properties->SetObjectField(TEXT("ability_name"), NameProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actorId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_GasActivateAbility::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	ClaireonGasToolCommon::FGasTarget Target;
	FString Error;
	if (!ClaireonGasToolCommon::ResolveTarget(Arguments, Target, Error))
	{
		return MakeErrorResult(Error);
	}
	UAbilitySystemComponent* ASC = Target.ASC;

	FString SpecHandleId;
	const bool bHasHandle = Arguments->TryGetStringField(TEXT("spec_handle_id"), SpecHandleId) && !SpecHandleId.IsEmpty();
	FString AbilityName;
	const bool bHasName = Arguments->TryGetStringField(TEXT("ability_name"), AbilityName) && !AbilityName.IsEmpty();

	if (!bHasHandle && !bHasName)
	{
		return MakeErrorResult(TEXT("Provide spec_handle_id or ability_name."));
	}

	// Locate the matching granted spec.
	FGameplayAbilitySpecHandle MatchedHandle;
	FString MatchedClass;
	const UGameplayAbility* MatchedAbilityCDO = nullptr;
	for (const FGameplayAbilitySpec& Spec : ASC->GetActivatableAbilities())
	{
		if (!Spec.Ability) { continue; }
		const bool bMatch = bHasHandle
			? Spec.Handle.ToString() == SpecHandleId
			: Spec.Ability->GetClass()->GetName().Contains(AbilityName, ESearchCase::IgnoreCase);
		if (bMatch)
		{
			MatchedHandle = Spec.Handle;
			MatchedClass = Spec.Ability->GetClass()->GetName();
			MatchedAbilityCDO = Spec.Ability;
			break;
		}
	}

	if (!MatchedHandle.IsValid())
	{
		return MakeErrorResult(FString::Printf(
			TEXT("No granted ability matching %s on %s. Grant it first with gas_grant_ability."),
			bHasHandle ? *FString::Printf(TEXT("spec_handle_id '%s'"), *SpecHandleId)
			           : *FString::Printf(TEXT("ability_name '%s'"), *AbilityName),
			*Target.Actor->GetName()));
	}

	const bool bActivated = ASC->TryActivateAbility(MatchedHandle);

	FString Reason;
	if (!bActivated && MatchedAbilityCDO)
	{
		// Best-effort explanation: re-run the can-activate check to collect the
		// blocking tags. This mirrors pie_test_ability's dry-run diagnostic.
		const FGameplayAbilityActorInfo* ActorInfo = ASC->AbilityActorInfo.Get();
		FGameplayTagContainer FailureTags;
		const bool bCanActivate = MatchedAbilityCDO->CanActivateAbility(
			MatchedHandle, ActorInfo, nullptr, nullptr, &FailureTags);
		if (bCanActivate)
		{
			Reason = TEXT("CanActivateAbility passed but TryActivateAbility returned false "
				"(cost/cooldown committed elsewhere, or activation policy declined).");
		}
		else if (!FailureTags.IsEmpty())
		{
			Reason = FString::Printf(TEXT("Blocked by tags: %s"), *FailureTags.ToStringSimple());
		}
		else
		{
			Reason = TEXT("Blocked (CanActivateAbility returned false with no relevant tags).");
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("class"), MatchedClass);
	Data->SetStringField(TEXT("spec_handle_id"), MatchedHandle.ToString());
	Data->SetBoolField(TEXT("activated"), bActivated);
	Data->SetStringField(TEXT("reason"), Reason);

	const FString Summary = bActivated
		? FString::Printf(TEXT("Activated %s on %s"), *MatchedClass, *Target.Actor->GetName())
		: FString::Printf(TEXT("%s did not activate on %s: %s"), *MatchedClass, *Target.Actor->GetName(), *Reason);

	return MakeSuccessResult(Data, Summary);
}
