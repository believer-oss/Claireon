// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_GasSetTags.h"

#include "ClaireonGasToolCommon.h"

#include "AbilitySystemComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"

FString ClaireonTool_GasSetTags::GetCategory() const { return TEXT("gas"); }
FString ClaireonTool_GasSetTags::GetOperation() const { return TEXT("set_tags"); }

FString ClaireonTool_GasSetTags::GetDescription() const
{
	return TEXT("Add or remove loose gameplay tags on a live PIE actor's ASC to simulate a trigger/state condition "
		"(e.g. add a state tag so an aspect's HitModifier condition fires). Args: tags (array of registered "
		"gameplay tag strings) and op ('add' or 'remove'). Requires a live PIE session; net_mode defaults "
		"to the server world. Returns the owned tags after the change.");
}

EClaireonToolSessionMode ClaireonTool_GasSetTags::GetSessionMode() const
{
	return EClaireonToolSessionMode::ReadOnly;
}

TSharedPtr<FJsonObject> ClaireonTool_GasSetTags::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	ClaireonGasToolCommon::AddCommonSchemaParams(Properties);

	TSharedPtr<FJsonObject> TagsProp = MakeShared<FJsonObject>();
	TagsProp->SetStringField(TEXT("type"), TEXT("array"));
	TagsProp->SetStringField(TEXT("description"),
		TEXT("Gameplay tag strings to add/remove (must be registered tags, e.g. 'State.Grappling')."));
	TSharedPtr<FJsonObject> TagsItems = MakeShared<FJsonObject>();
	TagsItems->SetStringField(TEXT("type"), TEXT("string"));
	TagsProp->SetObjectField(TEXT("items"), TagsItems);
	Properties->SetObjectField(TEXT("tags"), TagsProp);

	TSharedPtr<FJsonObject> OpProp = MakeShared<FJsonObject>();
	OpProp->SetStringField(TEXT("type"), TEXT("string"));
	OpProp->SetStringField(TEXT("description"), TEXT("'add' or 'remove' (default 'add')."));
	TArray<TSharedPtr<FJsonValue>> OpEnum;
	OpEnum.Add(MakeShared<FJsonValueString>(TEXT("add")));
	OpEnum.Add(MakeShared<FJsonValueString>(TEXT("remove")));
	OpProp->SetArrayField(TEXT("enum"), OpEnum);
	Properties->SetObjectField(TEXT("op"), OpProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actorId")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("tags")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_GasSetTags::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	ClaireonGasToolCommon::FGasTarget Target;
	FString Error;
	if (!ClaireonGasToolCommon::ResolveTarget(Arguments, Target, Error))
	{
		return MakeErrorResult(Error);
	}
	UAbilitySystemComponent* ASC = Target.ASC;

	const TArray<TSharedPtr<FJsonValue>>* TagsArray = nullptr;
	if (!Arguments->TryGetArrayField(TEXT("tags"), TagsArray) || !TagsArray || TagsArray->Num() == 0)
	{
		return MakeErrorResult(TEXT("Missing required argument: tags (non-empty array of tag strings)."));
	}

	FString Op = TEXT("add");
	Arguments->TryGetStringField(TEXT("op"), Op);
	const bool bAdd = !Op.Equals(TEXT("remove"), ESearchCase::IgnoreCase);

	TArray<FString> Warnings;
	int32 Changed = 0;
	for (const TSharedPtr<FJsonValue>& Value : *TagsArray)
	{
		const FString TagString = Value->AsString();
		if (TagString.IsEmpty()) { continue; }
		const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagString), /*ErrorIfNotFound=*/false);
		if (!Tag.IsValid())
		{
			Warnings.Add(FString::Printf(TEXT("'%s' is not a registered gameplay tag; skipped."), *TagString));
			continue;
		}
		if (bAdd)
		{
			ASC->AddLooseGameplayTag(Tag);
		}
		else
		{
			ASC->RemoveLooseGameplayTag(Tag);
		}
		++Changed;
	}

	FGameplayTagContainer OwnedTags;
	ASC->GetOwnedGameplayTags(OwnedTags);
	TArray<TSharedPtr<FJsonValue>> OwnedJson;
	for (const FGameplayTag& Tag : OwnedTags)
	{
		OwnedJson.Add(MakeShared<FJsonValueString>(Tag.ToString()));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("owned_tags_after"), OwnedJson);

	const FString Summary = FString::Printf(TEXT("%s %d tag(s) on %s (%d owned now)"),
		bAdd ? TEXT("Added") : TEXT("Removed"), Changed, *Target.Actor->GetName(), OwnedJson.Num());

	FToolResult Result = MakeSuccessResult(Data, Summary);
	Result.Warnings = MoveTemp(Warnings);
	return Result;
}
