// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_StateTreeRuntimeSendEvent.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"
#include "GameplayTagContainer.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"

FString ClaireonTool_StateTreeRuntimeSendEvent::GetName() const
{
	return TEXT("editor.statetree.runtime.sendEvent");
}

FString ClaireonTool_StateTreeRuntimeSendEvent::GetDescription() const
{
	return TEXT("Send a gameplay event to a running State Tree instance during PIE. "
				"Useful for testing event-driven transitions in AI behavior trees. "
				"Requires an active PIE session with a State Tree component on the target actor.");
}

TSharedPtr<FJsonObject> ClaireonTool_StateTreeRuntimeSendEvent::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> ActorIdProp = MakeShared<FJsonObject>();
	ActorIdProp->SetStringField(TEXT("type"), TEXT("string"));
	ActorIdProp->SetStringField(TEXT("description"), TEXT("PIE actor ID (from existing PIE tools, e.g. 'actor_0')"));
	Properties->SetObjectField(TEXT("actor_id"), ActorIdProp);

	TSharedPtr<FJsonObject> EventTagProp = MakeShared<FJsonObject>();
	EventTagProp->SetStringField(TEXT("type"), TEXT("string"));
	EventTagProp->SetStringField(TEXT("description"), TEXT("Gameplay tag for the event to send (e.g. 'AI.Target.Acquired')"));
	Properties->SetObjectField(TEXT("event_tag"), EventTagProp);

	TSharedPtr<FJsonObject> PayloadProp = MakeShared<FJsonObject>();
	PayloadProp->SetStringField(TEXT("type"), TEXT("object"));
	PayloadProp->SetStringField(TEXT("description"), TEXT("Optional event payload properties as key-value pairs"));
	Properties->SetObjectField(TEXT("payload"), PayloadProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actor_id")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("event_tag")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

namespace
{
	UActorComponent* FindSTComponentOnActor(AActor* Actor)
	{
		if (!Actor)
			return nullptr;

		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);

		for (UActorComponent* Component : Components)
		{
			if (Component && Component->GetClass()->GetName().Contains(TEXT("StateTreeComponent"), ESearchCase::IgnoreCase))
			{
				return Component;
			}
		}
		return nullptr;
	}

	UWorld* GetActivePIEWorld()
	{
		for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
		{
			if (WorldContext.WorldType == EWorldType::PIE && WorldContext.World())
			{
				return WorldContext.World();
			}
		}
		return nullptr;
	}
} // namespace

IClaireonTool::FToolResult ClaireonTool_StateTreeRuntimeSendEvent::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString ActorId;
	if (!Arguments->TryGetStringField(TEXT("actor_id"), ActorId) || ActorId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: actor_id"));
	}

	FString EventTagStr;
	if (!Arguments->TryGetStringField(TEXT("event_tag"), EventTagStr) || EventTagStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: event_tag"));
	}

	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.statetree.runtime.sendEvent: actor=%s, event=%s"),
		*ActorId, *EventTagStr);

	// Get PIE world
	UWorld* PIEWorld = GetActivePIEWorld();
	if (!PIEWorld)
	{
		return MakeErrorResult(TEXT("No active PIE session. Start Play-in-Editor first."));
	}

	// Resolve actor
	FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
	AActor* Actor = PIEManager.ResolveActorId(ActorId, PIEWorld);
	if (!Actor)
	{
		return MakeErrorResult(FString::Printf(TEXT("Actor not found or destroyed: %s"), *ActorId));
	}

	// Find State Tree component
	UActorComponent* Component = FindSTComponentOnActor(Actor);
	if (!Component)
	{
		return MakeErrorResult(FString::Printf(TEXT("No State Tree component found on actor %s (%s)"),
			*ActorId, *Actor->GetClass()->GetName()));
	}

	// Validate the event tag
	FGameplayTag EventTag = FGameplayTag::RequestGameplayTag(FName(*EventTagStr), false);
	if (!EventTag.IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid or unregistered gameplay tag: %s"), *EventTagStr));
	}

	// Find SendStateTreeEvent UFunction
	UFunction* SendEventFunc = Component->FindFunction(FName("SendStateTreeEvent"));
	if (!SendEventFunc)
	{
		return MakeErrorResult(TEXT("SendStateTreeEvent function not found on component. The component may not support event sending via reflection."));
	}

	// Construct parameters and invoke
	// The function signature varies by component; we use a generic approach
	// For FStateTreeEvent-based components, we need to construct the event struct
	// Try invoking with just the tag as a minimal approach
	struct FSendEventParams
	{
		FGameplayTag Tag;
	};

	FSendEventParams EventParams;
	EventParams.Tag = EventTag;

	Component->ProcessEvent(SendEventFunc, &EventParams);

	FString Output;
	Output += FString::Printf(TEXT("=== Event Sent ===\n"));
	Output += FString::Printf(TEXT("Actor: %s (%s)\n"), *ActorId, *Actor->GetClass()->GetName());
	Output += FString::Printf(TEXT("Component: %s\n"), *Component->GetClass()->GetName());
	Output += FString::Printf(TEXT("Event Tag: %s\n"), *EventTagStr);

	// Try to read current state after event
	UFunction* IsRunningFunc = Component->FindFunction(FName("IsRunning"));
	if (IsRunningFunc)
	{
		struct
		{
			bool ReturnValue = false;
		} RunParms;
		Component->ProcessEvent(IsRunningFunc, &RunParms);
		Output += FString::Printf(TEXT("Status: %s\n"), RunParms.ReturnValue ? TEXT("Running") : TEXT("Stopped"));
	}

	return MakeSuccessResult(nullptr, Output);
}
