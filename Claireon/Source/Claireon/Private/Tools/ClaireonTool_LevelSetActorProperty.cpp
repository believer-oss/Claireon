// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_LevelSetActorProperty.h"
#include "Tools/ClaireonPropertyResolver.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_LevelSetActorProperty::GetName() const
{
	return TEXT("claireon.level_set_actor_property");
}

FString ClaireonTool_LevelSetActorProperty::GetDescription() const
{
	return TEXT("Set properties on actors in the currently loaded level by label, class, or path.");
}

TSharedPtr<FJsonObject> ClaireonTool_LevelSetActorProperty::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// actor_label
	TSharedPtr<FJsonObject> ActorLabelProp = MakeShared<FJsonObject>();
	ActorLabelProp->SetStringField(TEXT("type"), TEXT("string"));
	ActorLabelProp->SetStringField(TEXT("description"), TEXT("Actor label from the Outliner panel."));
	Properties->SetObjectField(TEXT("actor_label"), ActorLabelProp);

	// actor_class
	TSharedPtr<FJsonObject> ActorClassProp = MakeShared<FJsonObject>();
	ActorClassProp->SetStringField(TEXT("type"), TEXT("string"));
	ActorClassProp->SetStringField(TEXT("description"), TEXT("Class name for actor lookup. Use with actor_index to disambiguate when multiple actors share the same class."));
	Properties->SetObjectField(TEXT("actor_class"), ActorClassProp);

	// actor_path
	TSharedPtr<FJsonObject> ActorPathProp = MakeShared<FJsonObject>();
	ActorPathProp->SetStringField(TEXT("type"), TEXT("string"));
	ActorPathProp->SetStringField(TEXT("description"), TEXT("Full object path to the actor."));
	Properties->SetObjectField(TEXT("actor_path"), ActorPathProp);

	// actor_index
	TSharedPtr<FJsonObject> ActorIndexProp = MakeShared<FJsonObject>();
	ActorIndexProp->SetStringField(TEXT("type"), TEXT("integer"));
	ActorIndexProp->SetStringField(TEXT("description"), TEXT("Disambiguation index when actor_class matches multiple actors (0-based). Defaults to 0."));
	Properties->SetObjectField(TEXT("actor_index"), ActorIndexProp);

	// properties (array of {name, value} objects)
	TSharedPtr<FJsonObject> PropertiesProp = MakeShared<FJsonObject>();
	PropertiesProp->SetStringField(TEXT("type"), TEXT("array"));
	PropertiesProp->SetStringField(TEXT("description"), TEXT("Array of property name/value pairs to set on the actor."));

	TSharedPtr<FJsonObject> ItemsSchema = MakeShared<FJsonObject>();
	ItemsSchema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> ItemProps = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> NameProp = MakeShared<FJsonObject>();
	NameProp->SetStringField(TEXT("type"), TEXT("string"));
	NameProp->SetStringField(TEXT("description"), TEXT("Property name (UPROPERTY name on the actor class)."));
	ItemProps->SetObjectField(TEXT("name"), NameProp);

	TSharedPtr<FJsonObject> ValueProp = MakeShared<FJsonObject>();
	ValueProp->SetStringField(TEXT("description"), TEXT("Property value. Strings, numbers, booleans, or objects/arrays for complex types."));
	ItemProps->SetObjectField(TEXT("value"), ValueProp);

	ItemsSchema->SetObjectField(TEXT("properties"), ItemProps);

	TArray<TSharedPtr<FJsonValue>> ItemRequired;
	ItemRequired.Add(MakeShared<FJsonValueString>(TEXT("name")));
	ItemRequired.Add(MakeShared<FJsonValueString>(TEXT("value")));
	ItemsSchema->SetArrayField(TEXT("required"), ItemRequired);

	PropertiesProp->SetObjectField(TEXT("items"), ItemsSchema);
	Properties->SetObjectField(TEXT("properties"), PropertiesProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("properties")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonTool_LevelSetActorProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	checkf(GEditor && GEditor->GetEditorWorldContext().World(),
		TEXT("RequiresEditorWorld() tool reached Execute without a valid world. This indicates a dispatch path that bypasses precondition checks."));
	UWorld* World = GEditor->GetEditorWorldContext().World();

	// Find actor by label, path, or class
	FString ActorLabel;
	FString ActorClass;
	FString ActorPath;
	int32 ActorIndex = 0;
	Arguments->TryGetStringField(TEXT("actor_label"), ActorLabel);
	Arguments->TryGetStringField(TEXT("actor_class"), ActorClass);
	Arguments->TryGetStringField(TEXT("actor_path"), ActorPath);
	Arguments->TryGetNumberField(TEXT("actor_index"), ActorIndex);

	AActor* TargetActor = nullptr;
	int32 MatchCount = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}

		bool bMatch = false;
		if (!ActorPath.IsEmpty())
		{
			bMatch = (Actor->GetPathName() == ActorPath);
		}
		else if (!ActorLabel.IsEmpty())
		{
			bMatch = (Actor->GetActorLabel() == ActorLabel);
		}
		else if (!ActorClass.IsEmpty())
		{
			bMatch = Actor->GetClass()->GetName().Contains(ActorClass, ESearchCase::IgnoreCase);
		}

		if (bMatch)
		{
			if (MatchCount == ActorIndex)
			{
				TargetActor = Actor;
				break;
			}
			MatchCount++;
		}
	}

	if (!TargetActor)
	{
		return MakeErrorResult(TEXT("Actor not found"));
	}

	// Get properties array
	const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
	if (!Arguments->TryGetArrayField(TEXT("properties"), PropertiesArray) || !PropertiesArray)
	{
		return MakeErrorResult(TEXT("Missing required field: properties"));
	}

	TArray<TSharedPtr<FJsonValue>> ResultDetails;
	bool bAnySuccess = false;
	FString LastPropertyName;
	FString LastOldValue;
	FString LastNewValue;

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Actor Property")));
	TargetActor->Modify();

	for (const TSharedPtr<FJsonValue>& PropValue : *PropertiesArray)
	{
		const TSharedPtr<FJsonObject>* PropObj = nullptr;
		if (!PropValue->TryGetObject(PropObj) || !PropObj)
		{
			continue;
		}

		FString PropName;
		if (!(*PropObj)->TryGetStringField(TEXT("name"), PropName))
		{
			continue;
		}

		// Read old value via resolver (component-aware)
		FString OldValue;
		{
			ClaireonPropertyResolver::FResolvedProperty ReadResolved;
			FString ReadError;
			OldValue = ClaireonPropertyResolver::ReadPropertyOnActor(TargetActor, PropName, ReadResolved, ReadError);
			// If read fails (property not found), OldValue is empty -- that's OK, we still attempt the write
		}

		// Extract new value from JSON
		FString NewValueStr;
		(*PropObj)->TryGetStringField(TEXT("value"), NewValueStr);

		// Write new value via resolver (component-aware, handles Modify() on component)
		ClaireonPropertyResolver::FResolvedProperty WriteResolved;
		FString WriteError;
		bool bSetOk = ClaireonPropertyResolver::WritePropertyOnActor(TargetActor, PropName, NewValueStr, WriteResolved, WriteError);

		TSharedPtr<FJsonObject> DetailObj = MakeShared<FJsonObject>();
		DetailObj->SetStringField(TEXT("property_name"), PropName);
		DetailObj->SetStringField(TEXT("old_value"), OldValue);
		DetailObj->SetStringField(TEXT("new_value"), NewValueStr);
		DetailObj->SetBoolField(TEXT("success"), bSetOk);
		if (bSetOk)
		{
			DetailObj->SetStringField(TEXT("resolved_on"), WriteResolved.ResolvedOn);
			DetailObj->SetStringField(TEXT("qualified_path"), WriteResolved.QualifiedPath);
			if (!WriteResolved.Note.IsEmpty())
			{
				DetailObj->SetStringField(TEXT("note"), WriteResolved.Note);
			}
		}
		else
		{
			DetailObj->SetStringField(TEXT("error"), WriteError);
		}
		ResultDetails.Add(MakeShared<FJsonValueObject>(DetailObj));

		if (bSetOk)
		{
			bAnySuccess = true;
			LastPropertyName = PropName;
			LastOldValue = OldValue;
			LastNewValue = NewValueStr;
		}
	}

	TargetActor->MarkPackageDirty();

	const FString UsedLabel = TargetActor->GetActorLabel();
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), UsedLabel);
	if (ResultDetails.Num() == 1)
	{
		Data->SetStringField(TEXT("property_name"), LastPropertyName);
		Data->SetStringField(TEXT("old_value"), LastOldValue);
		Data->SetStringField(TEXT("new_value"), LastNewValue);
	}
	Data->SetBoolField(TEXT("success"), bAnySuccess);
	Data->SetArrayField(TEXT("details"), ResultDetails);

	FString Summary;
	if (ResultDetails.Num() == 1)
	{
		Summary = FString::Printf(TEXT("Set %s from %s to %s on %s"),
			*LastPropertyName, *LastOldValue, *LastNewValue, *UsedLabel);
	}
	else
	{
		Summary = FString::Printf(TEXT("Set %d properties on %s"), ResultDetails.Num(), *UsedLabel);
	}

	return MakeSuccessResult(Data, Summary);
}
