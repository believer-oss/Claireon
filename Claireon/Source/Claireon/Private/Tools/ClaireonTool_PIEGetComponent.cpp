// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIEGetComponent.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"

FString ClaireonTool_PIEGetComponent::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_PIEGetComponent::GetOperation() const { return TEXT("get_component"); }

FString ClaireonTool_PIEGetComponent::GetDescription() const
{
	return TEXT("Find a component on an actor by class name. Optionally includes component property details via reflection.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIEGetComponent::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// actorId - required
	TSharedPtr<FJsonObject> ActorIdProp = MakeShared<FJsonObject>();
	ActorIdProp->SetStringField(TEXT("type"), TEXT("string"));
	ActorIdProp->SetStringField(TEXT("description"),
		TEXT("The stable actor ID to search for components on (e.g., 'actor_0')"));
	Properties->SetObjectField(TEXT("actorId"), ActorIdProp);

	// componentClass - required
	TSharedPtr<FJsonObject> ComponentClassProp = MakeShared<FJsonObject>();
	ComponentClassProp->SetStringField(TEXT("type"), TEXT("string"));
	ComponentClassProp->SetStringField(TEXT("description"),
		TEXT("Class name (or substring) of the component to find (e.g., 'HealthComponent', 'CharacterMovement')"));
	Properties->SetObjectField(TEXT("componentClass"), ComponentClassProp);

	// includeDetails - optional, default false
	TSharedPtr<FJsonObject> IncludeDetailsProp = MakeShared<FJsonObject>();
	IncludeDetailsProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeDetailsProp->SetStringField(TEXT("description"),
		TEXT("If true, include component properties via UProperty reflection (default: false)"));
	IncludeDetailsProp->SetBoolField(TEXT("default"), false);
	Properties->SetObjectField(TEXT("includeDetails"), IncludeDetailsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	// Required fields
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actorId")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("componentClass")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIEGetComponent::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.pie.getComponentFromActor"));

	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor is not available"));
	}

	if (!GEditor->IsPlaySessionInProgress())
	{
		return MakeErrorResult(TEXT("PIE is not running. Start a PIE session first with editor.pie.start"));
	}

	// Parse required parameters
	if (!Arguments.IsValid() || !Arguments->HasField(TEXT("actorId")) || !Arguments->HasField(TEXT("componentClass")))
	{
		return MakeErrorResult(TEXT("Missing required parameters: actorId and componentClass"));
	}

	const FString ActorId = Arguments->GetStringField(TEXT("actorId"));
	const FString ComponentClassName = Arguments->GetStringField(TEXT("componentClass"));

	bool bIncludeDetails = false;
	if (Arguments->HasField(TEXT("includeDetails")))
	{
		bIncludeDetails = Arguments->GetBoolField(TEXT("includeDetails"));
	}

	// Find PIE world
	UWorld* PIEWorld = nullptr;
	for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
	{
		if (WorldContext.WorldType == EWorldType::PIE && WorldContext.World())
		{
			PIEWorld = WorldContext.World();
			break;
		}
	}

	if (!PIEWorld)
	{
		return MakeErrorResult(TEXT("PIE world not found. PIE may still be initializing."));
	}

	// Resolve the actor
	FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
	AActor* Actor = PIEManager.ResolveActorId(ActorId, PIEWorld);

	if (!Actor)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Could not resolve actor '%s'. The actor may have been destroyed or the ID is stale."),
			*ActorId));
	}

	// Search for the component by class name (substring match)
	UActorComponent* FoundComponent = nullptr;
	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);

	for (UActorComponent* Component : Components)
	{
		if (!Component)
		{
			continue;
		}

		const FString ClassName = Component->GetClass()->GetName();
		if (ClassName.Contains(ComponentClassName) || ClassName.Equals(ComponentClassName, ESearchCase::IgnoreCase))
		{
			FoundComponent = Component;
			break;
		}
	}

	// Build output
	FString Output;
	Output += FString::Printf(TEXT("actorId: %s\n"), *ActorId);
	Output += FString::Printf(TEXT("searchClass: %s\n"), *ComponentClassName);
	Output += FString::Printf(TEXT("found: %s\n"), FoundComponent ? TEXT("true") : TEXT("false"));

	if (!FoundComponent)
	{
		Output += TEXT("availableComponents:\n");
		for (const UActorComponent* Component : Components)
		{
			if (Component)
			{
				Output += FString::Printf(TEXT("  - %s (%s)\n"),
					*Component->GetName(), *Component->GetClass()->GetName());
			}
		}
		return MakeSuccessResult(nullptr, Output);
	}

	Output += FString::Printf(TEXT("componentClass: %s\n"), *FoundComponent->GetClass()->GetName());
	Output += FString::Printf(TEXT("componentName: %s\n"), *FoundComponent->GetName());
	Output += FString::Printf(TEXT("isActive: %s\n"), FoundComponent->IsActive() ? TEXT("true") : TEXT("false"));

	// Class hierarchy
	FString ClassHierarchy;
	for (UClass* Class = FoundComponent->GetClass(); Class; Class = Class->GetSuperClass())
	{
		if (!ClassHierarchy.IsEmpty())
		{
			ClassHierarchy += TEXT(" -> ");
		}
		ClassHierarchy += Class->GetName();
		if (Class == UActorComponent::StaticClass())
		{
			break;
		}
	}
	Output += FString::Printf(TEXT("classHierarchy: %s\n"), *ClassHierarchy);

	if (bIncludeDetails)
	{
		Output += TEXT("properties:\n");

		int32 PropertyCount = 0;
		const int32 MaxProperties = 50; // Limit to avoid overwhelming output

		for (TFieldIterator<FProperty> PropIt(FoundComponent->GetClass()); PropIt; ++PropIt)
		{
			if (PropertyCount >= MaxProperties)
			{
				Output += FString::Printf(TEXT("  ... (truncated, %d+ properties total)\n"),
					PropertyCount);
				break;
			}

			const FProperty* Property = *PropIt;
			if (!Property)
			{
				continue;
			}

			// Skip properties that are not editor-visible or are internal
			if (!Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
			{
				continue;
			}

			FString ValueStr;
			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(FoundComponent);

			// Export the property value to a string
			Property->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, FoundComponent, PPF_None);

			// Truncate very long values
			if (ValueStr.Len() > 200)
			{
				ValueStr = ValueStr.Left(200) + TEXT("...");
			}

			Output += FString::Printf(TEXT("  %s (%s): %s\n"),
				*Property->GetName(),
				*Property->GetCPPType(),
				*ValueStr);

			PropertyCount++;
		}

		if (PropertyCount == 0)
		{
			Output += TEXT("  (no editable properties found)\n");
		}
	}

	return MakeSuccessResult(nullptr, Output);
}
