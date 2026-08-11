// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAudioTool_SetAudioProperty.h"
#include "Tools/ClaireonAudioApplyHelpers.h"
#include "Tools/ClaireonPropertyResolver.h"
#include "Tools/ClaireonPropertyUtils.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString FClaireonAudioTool_SetAudioProperty::GetCategory() const { return TEXT("audio"); }
FString FClaireonAudioTool_SetAudioProperty::GetOperation() const { return TEXT("set_audio_property"); }

FString FClaireonAudioTool_SetAudioProperty::GetDescription() const
{
	return TEXT("Set a property by reflection on an audio actor (target by actor_name) OR on a component "
	            "(target by component_path = '<actor_label>.<component_name>'); supply exactly one. "
	            "'value' is any JSON value; numbers and bools are stringified for ClaireonPropertyResolver. "
	            "Stateless / non-session: writes the editor world directly inside an FScopedTransaction, "
	            "no open session required.");
}

TSharedPtr<FJsonObject> FClaireonAudioTool_SetAudioProperty::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	ClaireonAudioSchema::AddString(Properties, TEXT("actor_name"), TEXT("Target actor, by label or name."));
	ClaireonAudioSchema::AddString(Properties, TEXT("component_path"), TEXT("Path to the component to write, relative to the actor. Empty targets the actor's own audio component."));
	ClaireonAudioSchema::AddString(Properties, TEXT("field_name"), TEXT("Property name to write on the resolved component."));
	ClaireonAudioSchema::AddAnyType(Properties, TEXT("value"),
		TEXT("New value for field_name. Accepts whatever JSON type the target property takes."));

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("field_name")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult FClaireonAudioTool_SetAudioProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!IsValid(GEditor) || !IsValid(GEditor->GetEditorWorldContext().World()))
	{
		return MakeErrorResult(TEXT("set_audio_property requires an active editor world"));
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();

	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}

	FString ActorName, ComponentPath, FieldName;
	Arguments->TryGetStringField(TEXT("actor_name"), ActorName);
	Arguments->TryGetStringField(TEXT("component_path"), ComponentPath);
	Arguments->TryGetStringField(TEXT("field_name"), FieldName);

	if (FieldName.IsEmpty())
	{
		return MakeErrorResult(TEXT("set_audio_property: missing field_name"));
	}
	if (ActorName.IsEmpty() && ComponentPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("set_audio_property: one of actor_name or component_path is required"));
	}
	if (!ActorName.IsEmpty() && !ComponentPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("set_audio_property: supply exactly one of actor_name or component_path, not both"));
	}

	// "value" field is any JSON value; stringify for ClaireonPropertyResolver.
	FString ValueStr;
	TSharedPtr<FJsonValue> ValueJson = Arguments->TryGetField(TEXT("value"));
	if (ValueJson.IsValid())
	{
		if (!ValueJson->TryGetString(ValueStr))
		{
			double NumVal;
			bool BoolVal;
			if (ValueJson->TryGetNumber(NumVal)) ValueStr = FString::SanitizeFloat(NumVal);
			else if (ValueJson->TryGetBool(BoolVal)) ValueStr = BoolVal ? TEXT("True") : TEXT("False");
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Audio Property")));

	if (!ActorName.IsEmpty())
	{
		AActor* Target = ClaireonAudioApplyHelpers::FindActorByLabel(World, ActorName);
		if (!IsValid(Target)) return MakeErrorResult(FString::Printf(TEXT("Actor '%s' not found"), *ActorName));
		Target->Modify();
		ClaireonPropertyResolver::FResolvedProperty Resolved;
		FString Err;
		if (!ClaireonPropertyResolver::WritePropertyOnActor(Target, FieldName, ValueStr, Resolved, Err))
		{
			return MakeErrorResult(Err);
		}
		Target->MarkPackageDirty();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("actor_name"), ActorName);
		Data->SetStringField(TEXT("field_name"), FieldName);
		return MakeSuccessResult(Data, FString::Printf(TEXT("Set %s on %s"), *FieldName, *ActorName));
	}

	// component_path: expect "ActorLabel.ComponentName"
	FString ActorPart, CompPart;
	if (!ComponentPath.Split(TEXT("."), &ActorPart, &CompPart) || ActorPart.IsEmpty() || CompPart.IsEmpty())
	{
		return MakeErrorResult(TEXT("component_path must be in the form '<actor_label>.<component_name>'"));
	}
	AActor* Target = ClaireonAudioApplyHelpers::FindActorByLabel(World, ActorPart);
	if (!IsValid(Target)) return MakeErrorResult(FString::Printf(TEXT("Actor '%s' not found"), *ActorPart));
	UActorComponent* TargetComp = nullptr;
	for (UActorComponent* Comp : Target->GetComponents())
	{
		if (IsValid(Comp) && Comp->GetName() == CompPart)
		{
			TargetComp = Comp;
			break;
		}
	}
	if (!IsValid(TargetComp)) return MakeErrorResult(FString::Printf(TEXT("Component '%s' not found on actor '%s'"), *CompPart, *ActorPart));

	TargetComp->Modify();
	FString Err;
	if (!ClaireonPropertyUtils::WritePropertyByPath(TargetComp, FieldName, ValueStr, Err))
	{
		return MakeErrorResult(Err);
	}
	Target->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("component_path"), ComponentPath);
	Data->SetStringField(TEXT("field_name"), FieldName);
	return MakeSuccessResult(Data, FString::Printf(TEXT("Set %s on %s"), *FieldName, *ComponentPath));
}
