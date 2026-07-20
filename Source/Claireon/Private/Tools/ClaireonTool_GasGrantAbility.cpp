// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_GasGrantAbility.h"

#include "ClaireonGasToolCommon.h"

#include "AbilitySystemComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "GameplayAbilitySpec.h"
#include "Abilities/GameplayAbility.h"

FString ClaireonTool_GasGrantAbility::GetCategory() const { return TEXT("gas"); }
FString ClaireonTool_GasGrantAbility::GetOperation() const { return TEXT("grant_ability"); }

FString ClaireonTool_GasGrantAbility::GetDescription() const
{
	return TEXT("Grant a GameplayAbility to a live PIE actor's ASC (requires the server/authoritative "
		"world -- GiveAbility is authority-only). Identify the GA by ability_class_path or ability_name. "
		"Optional level (default 1) and activate (default false). Returns spec_handle_id for "
		"gas_activate_ability. Requires PIE.");
}

EClaireonToolSessionMode ClaireonTool_GasGrantAbility::GetSessionMode() const
{
	return EClaireonToolSessionMode::ReadOnly;
}

TSharedPtr<FJsonObject> ClaireonTool_GasGrantAbility::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	ClaireonGasToolCommon::AddCommonSchemaParams(Properties);

	TSharedPtr<FJsonObject> PathProp = MakeShared<FJsonObject>();
	PathProp->SetStringField(TEXT("type"), TEXT("string"));
	PathProp->SetStringField(TEXT("description"),
		TEXT("Authoritative generated-class path of the GameplayAbility, e.g. "
			 "'/Game/.../GA_X.GA_X_C'."));
	Properties->SetObjectField(TEXT("ability_class_path"), PathProp);

	TSharedPtr<FJsonObject> NameProp = MakeShared<FJsonObject>();
	NameProp->SetStringField(TEXT("type"), TEXT("string"));
	NameProp->SetStringField(TEXT("description"),
		TEXT("Convenience substring matched against loaded UGameplayAbility subclasses. "
			 "Errors if ambiguous or not loaded; prefer ability_class_path."));
	Properties->SetObjectField(TEXT("ability_name"), NameProp);

	TSharedPtr<FJsonObject> LevelProp = MakeShared<FJsonObject>();
	LevelProp->SetStringField(TEXT("type"), TEXT("number"));
	LevelProp->SetStringField(TEXT("description"), TEXT("Ability level (default 1)."));
	Properties->SetObjectField(TEXT("level"), LevelProp);

	TSharedPtr<FJsonObject> ActivateProp = MakeShared<FJsonObject>();
	ActivateProp->SetStringField(TEXT("type"), TEXT("boolean"));
	ActivateProp->SetStringField(TEXT("description"), TEXT("Activate immediately after granting (default false)."));
	Properties->SetObjectField(TEXT("activate"), ActivateProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actorId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_GasGrantAbility::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	ClaireonGasToolCommon::FGasTarget Target;
	FString Error;
	if (!ClaireonGasToolCommon::ResolveTarget(Arguments, Target, Error))
	{
		return MakeErrorResult(Error);
	}
	UAbilitySystemComponent* ASC = Target.ASC;

	if (!ASC->IsOwnerActorAuthoritative())
	{
		return MakeErrorResult(TEXT("gas_grant_ability requires the authoritative ASC. This resolved to a "
			"non-authority (client) world. Re-run with net_mode='server'."));
	}

	TSubclassOf<UGameplayAbility> AbilityClass = ClaireonGasToolCommon::ResolveAbilityClass(Arguments, Error);
	if (!AbilityClass)
	{
		return MakeErrorResult(Error);
	}

	double LevelValue = 1.0;
	Arguments->TryGetNumberField(TEXT("level"), LevelValue);
	const int32 Level = static_cast<int32>(LevelValue);

	bool bActivate = false;
	Arguments->TryGetBoolField(TEXT("activate"), bActivate);

	FGameplayAbilitySpec Spec(AbilityClass, Level, INDEX_NONE, /*SourceObject=*/nullptr);
	const FGameplayAbilitySpecHandle Handle = ASC->GiveAbility(Spec);
	if (!Handle.IsValid())
	{
		return MakeErrorResult(FString::Printf(
			TEXT("GiveAbility failed for '%s' on %s."), *AbilityClass->GetName(), *Target.Actor->GetName()));
	}

	bool bActivated = false;
	if (bActivate)
	{
		bActivated = ASC->TryActivateAbility(Handle);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("spec_handle_id"), Handle.ToString());
	Data->SetStringField(TEXT("class"), AbilityClass->GetName());
	Data->SetBoolField(TEXT("activated"), bActivated);

	FString Summary = FString::Printf(TEXT("Granted %s (handle %s) to %s"),
		*AbilityClass->GetName(), *Handle.ToString(), *Target.Actor->GetName());
	if (bActivate)
	{
		Summary += bActivated ? TEXT(" and activated it") : TEXT(" (activation blocked)");
	}

	return MakeSuccessResult(Data, Summary);
}
