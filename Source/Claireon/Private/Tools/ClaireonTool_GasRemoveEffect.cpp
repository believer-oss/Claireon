// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_GasRemoveEffect.h"

#include "ClaireonGasToolCommon.h"

#include "AbilitySystemComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "GameplayEffect.h"
#include "GameplayEffectTypes.h"

FString ClaireonTool_GasRemoveEffect::GetCategory() const { return TEXT("gas"); }
FString ClaireonTool_GasRemoveEffect::GetOperation() const { return TEXT("remove_effect"); }

FString ClaireonTool_GasRemoveEffect::GetDescription() const
{
	return TEXT("Remove an applied GameplayEffect from a live PIE actor's ASC. Address it by effect_handle_id (from "
		"gas_runtime_inspect / gas_apply_effect) to remove one, or by effect_class_path / effect_name to "
		"remove all matching. Optional stacks_to_remove (default -1 = all stacks). Requires a live PIE "
		"session; net_mode defaults to the server world.");
}

EClaireonToolSessionMode ClaireonTool_GasRemoveEffect::GetSessionMode() const
{
	return EClaireonToolSessionMode::ReadOnly;
}

TSharedPtr<FJsonObject> ClaireonTool_GasRemoveEffect::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	ClaireonGasToolCommon::AddCommonSchemaParams(Properties);

	TSharedPtr<FJsonObject> HandleProp = MakeShared<FJsonObject>();
	HandleProp->SetStringField(TEXT("type"), TEXT("string"));
	HandleProp->SetStringField(TEXT("description"),
		TEXT("effect_handle_id from gas_runtime_inspect / gas_apply_effect. Removes exactly that effect."));
	Properties->SetObjectField(TEXT("effect_handle_id"), HandleProp);

	TSharedPtr<FJsonObject> PathProp = MakeShared<FJsonObject>();
	PathProp->SetStringField(TEXT("type"), TEXT("string"));
	PathProp->SetStringField(TEXT("description"),
		TEXT("Generated-class path of the GameplayEffect; removes ALL active effects of this class."));
	Properties->SetObjectField(TEXT("effect_class_path"), PathProp);

	TSharedPtr<FJsonObject> NameProp = MakeShared<FJsonObject>();
	NameProp->SetStringField(TEXT("type"), TEXT("string"));
	NameProp->SetStringField(TEXT("description"),
		TEXT("Substring matched against loaded UGameplayEffect subclasses; removes ALL matching."));
	Properties->SetObjectField(TEXT("effect_name"), NameProp);

	TSharedPtr<FJsonObject> StacksProp = MakeShared<FJsonObject>();
	StacksProp->SetStringField(TEXT("type"), TEXT("number"));
	StacksProp->SetStringField(TEXT("description"), TEXT("Stacks to remove (default -1 = all)."));
	Properties->SetObjectField(TEXT("stacks_to_remove"), StacksProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actorId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_GasRemoveEffect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	ClaireonGasToolCommon::FGasTarget Target;
	FString Error;
	if (!ClaireonGasToolCommon::ResolveTarget(Arguments, Target, Error))
	{
		return MakeErrorResult(Error);
	}
	UAbilitySystemComponent* ASC = Target.ASC;

	int32 StacksToRemove = -1;
	double StacksValue = 0.0;
	if (Arguments->TryGetNumberField(TEXT("stacks_to_remove"), StacksValue))
	{
		StacksToRemove = static_cast<int32>(StacksValue);
	}

	int32 RemovedCount = 0;

	FString HandleId;
	const bool bHasHandle = Arguments->TryGetStringField(TEXT("effect_handle_id"), HandleId) && !HandleId.IsEmpty();
	const bool bHasClass = Arguments->HasField(TEXT("effect_class_path")) || Arguments->HasField(TEXT("effect_name"));

	if (bHasHandle)
	{
		if (!HandleId.IsNumeric())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("effect_handle_id '%s' is not a valid handle (expected the integer id from gas_runtime_inspect)."),
				*HandleId));
		}
		const FActiveGameplayEffectHandle Handle(FCString::Atoi(*HandleId));
		if (!ASC->GetActiveGameplayEffect(Handle))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("No active effect with handle '%s' on %s (already expired or wrong world/actor?)."),
				*HandleId, *Target.Actor->GetName()));
		}
		if (ASC->RemoveActiveGameplayEffect(Handle, StacksToRemove))
		{
			RemovedCount = 1;
		}
	}
	else if (bHasClass)
	{
		TSubclassOf<UGameplayEffect> GEClass = ClaireonGasToolCommon::ResolveEffectClass(Arguments, Error);
		if (!IsValid(GEClass))
		{
			return MakeErrorResult(Error);
		}
		// Snapshot handles first: removal mutates the active-effects container.
		TArray<FActiveGameplayEffectHandle> ToRemove;
		for (const FActiveGameplayEffectHandle& Handle : ASC->GetActiveEffects(FGameplayEffectQuery()))
		{
			const FActiveGameplayEffect* Active = ASC->GetActiveGameplayEffect(Handle);
			if (Active && Active->Spec.Def && Active->Spec.Def->GetClass()->IsChildOf(GEClass))
			{
				ToRemove.Add(Handle);
			}
		}
		for (const FActiveGameplayEffectHandle& Handle : ToRemove)
		{
			if (ASC->RemoveActiveGameplayEffect(Handle, StacksToRemove))
			{
				++RemovedCount;
			}
		}
	}
	else
	{
		return MakeErrorResult(TEXT("Provide effect_handle_id, or effect_class_path / effect_name."));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("removed_count"), RemovedCount);

	const FString Summary = FString::Printf(TEXT("Removed %d effect(s) from %s"),
		RemovedCount, *Target.Actor->GetName());
	return MakeSuccessResult(Data, Summary);
}
