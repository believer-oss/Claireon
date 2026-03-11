// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_LevelSetActorProperty.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_LevelSetActorProperty::GetName() const
{
	return TEXT("set_actor_property");
}

FString ClaireonTool_LevelSetActorProperty::GetCategory() const
{
	return TEXT("level");
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
	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor not available"));
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return MakeErrorResult(TEXT("No world loaded"));
	}

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

	FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Set Actor Property")));
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

		// Get current value as string
		FString OldValue;
		FProperty* Property = TargetActor->GetClass()->FindPropertyByName(FName(*PropName));
		if (Property)
		{
			Property->ExportTextItem_Direct(OldValue, Property->ContainerPtrToValuePtr<void>(TargetActor), nullptr, nullptr, PPF_None);
		}

		// Set new value
		FString NewValueStr;
		const TSharedPtr<FJsonObject>* NewValueObj = nullptr;

		bool bSetOk = false;
		if ((*PropObj)->TryGetStringField(TEXT("value"), NewValueStr))
		{
			if (Property)
			{
				Property->ImportText_Direct(*NewValueStr, Property->ContainerPtrToValuePtr<void>(TargetActor), TargetActor, PPF_None);
				bSetOk = true;
			}
		}

		TSharedPtr<FJsonObject> DetailObj = MakeShared<FJsonObject>();
		DetailObj->SetStringField(TEXT("property_name"), PropName);
		DetailObj->SetStringField(TEXT("old_value"), OldValue);
		DetailObj->SetStringField(TEXT("new_value"), NewValueStr);
		DetailObj->SetBoolField(TEXT("success"), bSetOk);
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
