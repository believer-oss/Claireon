// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_GasSetAttribute.h"

#include "ClaireonGasToolCommon.h"

#include "AbilitySystemComponent.h"
#include "AttributeSet.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "GameplayEffectTypes.h"

FString ClaireonTool_GasSetAttribute::GetCategory() const { return TEXT("gas"); }
FString ClaireonTool_GasSetAttribute::GetOperation() const { return TEXT("set_attribute"); }

FString ClaireonTool_GasSetAttribute::GetDescription() const
{
	return TEXT("Set a gameplay attribute's base or current value on a live PIE actor's ASC (e.g. drop Health to "
		"test a threshold aspect). Args: attribute (name matched against the ASC's attributes), value "
		"(number), mode ('base' or 'current', default base). Requires a live PIE session; net_mode defaults "
		"to the server world. Returns old_value and new_value.");
}

EClaireonToolSessionMode ClaireonTool_GasSetAttribute::GetSessionMode() const
{
	return EClaireonToolSessionMode::ReadOnly;
}

TSharedPtr<FJsonObject> ClaireonTool_GasSetAttribute::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	ClaireonGasToolCommon::AddCommonSchemaParams(Properties);

	TSharedPtr<FJsonObject> AttrProp = MakeShared<FJsonObject>();
	AttrProp->SetStringField(TEXT("type"), TEXT("string"));
	AttrProp->SetStringField(TEXT("description"),
		TEXT("Attribute name matched against the ASC's attributes (exact case-insensitive, else unique "
			 "substring), e.g. 'Health'."));
	Properties->SetObjectField(TEXT("attribute"), AttrProp);

	TSharedPtr<FJsonObject> ValueProp = MakeShared<FJsonObject>();
	ValueProp->SetStringField(TEXT("type"), TEXT("number"));
	ValueProp->SetStringField(TEXT("description"), TEXT("New value."));
	Properties->SetObjectField(TEXT("value"), ValueProp);

	TSharedPtr<FJsonObject> ModeProp = MakeShared<FJsonObject>();
	ModeProp->SetStringField(TEXT("type"), TEXT("string"));
	ModeProp->SetStringField(TEXT("description"),
		TEXT("'base' sets the base value (SetNumericAttributeBase); 'current' overrides the current "
			 "value (ApplyModToAttribute Override). Default 'base'."));
	TArray<TSharedPtr<FJsonValue>> ModeEnum;
	ModeEnum.Add(MakeShared<FJsonValueString>(TEXT("base")));
	ModeEnum.Add(MakeShared<FJsonValueString>(TEXT("current")));
	ModeProp->SetArrayField(TEXT("enum"), ModeEnum);
	Properties->SetObjectField(TEXT("mode"), ModeProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actorId")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("attribute")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("value")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_GasSetAttribute::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	ClaireonGasToolCommon::FGasTarget Target;
	FString Error;
	if (!ClaireonGasToolCommon::ResolveTarget(Arguments, Target, Error))
	{
		return MakeErrorResult(Error);
	}
	UAbilitySystemComponent* ASC = Target.ASC;

	FString AttributeName;
	if (!Arguments->TryGetStringField(TEXT("attribute"), AttributeName) || AttributeName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required argument: attribute"));
	}

	double Value = 0.0;
	if (!Arguments->TryGetNumberField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required numeric argument: value"));
	}

	FGameplayAttribute Attribute;
	if (!ClaireonGasToolCommon::ResolveAttribute(ASC, AttributeName, Attribute, Error))
	{
		return MakeErrorResult(Error);
	}

	FString Mode = TEXT("base");
	Arguments->TryGetStringField(TEXT("mode"), Mode);
	const bool bBase = !Mode.Equals(TEXT("current"), ESearchCase::IgnoreCase);

	const float OldValue = bBase ? ASC->GetNumericAttributeBase(Attribute) : ASC->GetNumericAttribute(Attribute);

	if (bBase)
	{
		ASC->SetNumericAttributeBase(Attribute, static_cast<float>(Value));
	}
	else
	{
		ASC->ApplyModToAttribute(Attribute, EGameplayModOp::Override, static_cast<float>(Value));
	}

	const float NewValue = bBase ? ASC->GetNumericAttributeBase(Attribute) : ASC->GetNumericAttribute(Attribute);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("attribute"), ClaireonGasToolCommon::AttributeDisplayName(Attribute));
	Data->SetStringField(TEXT("mode"), bBase ? TEXT("base") : TEXT("current"));
	Data->SetNumberField(TEXT("old_value"), OldValue);
	Data->SetNumberField(TEXT("new_value"), NewValue);

	const FString Summary = FString::Printf(TEXT("%s %s: %.3f -> %.3f on %s"),
		*ClaireonGasToolCommon::AttributeDisplayName(Attribute),
		bBase ? TEXT("(base)") : TEXT("(current)"),
		OldValue, NewValue, *Target.Actor->GetName());

	return MakeSuccessResult(Data, Summary);
}
