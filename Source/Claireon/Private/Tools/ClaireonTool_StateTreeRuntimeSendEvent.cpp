// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_StateTreeRuntimeSendEvent.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"
#include "ClaireonStateTreeComponentResolver.h"
#include "GameplayTagContainer.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"

FString ClaireonTool_StateTreeRuntimeSendEvent::GetCategory() const { return TEXT("statetree"); }
FString ClaireonTool_StateTreeRuntimeSendEvent::GetOperation() const { return TEXT("runtime_send_event"); }

FString ClaireonTool_StateTreeRuntimeSendEvent::GetDescription() const
{
	return TEXT("Send a gameplay event to a running State Tree instance during PIE. Stateless / non-session: requires no open editing session, but does require an active PIE session with a State Tree component on the target actor. Useful for testing event-driven transitions in AI behaviors. Immediate-write to runtime state.");
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

	// Same escape hatch statetree_runtime_inspect has. Without it this tool had
	// no workaround at all when component resolution picked the wrong component.
	TSharedPtr<FJsonObject> ComponentClassProp = MakeShared<FJsonObject>();
	ComponentClassProp->SetStringField(TEXT("type"), TEXT("string"));
	ComponentClassProp->SetStringField(TEXT("description"), TEXT("Optional component class-name substring, case-insensitive. Default: the first component of any UStateTreeComponent subclass, StateTreeAIComponent included. Use this only to disambiguate between several."));
	Properties->SetObjectField(TEXT("component_class"), ComponentClassProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actor_id")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("event_tag")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

namespace ClaireonTool_StateTreeRuntimeSendEvent_Private
{
	UWorld* GetActivePIEWorld()
	{
		for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
		{
			if (WorldContext.WorldType == EWorldType::PIE && IsValid(WorldContext.World()))
			{
				return WorldContext.World();
			}
		}
		return nullptr;
	}
} // namespace
using namespace ClaireonTool_StateTreeRuntimeSendEvent_Private;

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
	if (!IsValid(PIEWorld))
	{
		return MakeErrorResult(TEXT("No active PIE session. Start Play-in-Editor first."));
	}

	// Resolve actor
	FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
	AActor* Actor = PIEManager.ResolveActorId(ActorId, PIEWorld);
	if (!IsValid(Actor))
	{
		return MakeErrorResult(FString::Printf(TEXT("Actor not found or destroyed: %s"), *ActorId));
	}

	// Find State Tree component
	FString ComponentClass;
	Arguments->TryGetStringField(TEXT("component_class"), ComponentClass);

	UActorComponent* Component = ClaireonStateTreeComponentResolver::FindStateTreeComponent(Actor, ComponentClass);
	if (!IsValid(Component))
	{
		// Enumerate, like inspect does: a caller told only "none found" cannot
		// tell a missing component from a component this tool failed to match.
		const FString ComponentList = ClaireonStateTreeComponentResolver::DescribeComponents(Actor);
		return MakeErrorResult(FString::Printf(TEXT("No State Tree component found on actor %s (%s). Components:%s"),
			*ActorId, *Actor->GetClass()->GetName(), *ComponentList));
	}

	// Validate the event tag
	FGameplayTag EventTag = FGameplayTag::RequestGameplayTag(FName(*EventTagStr), false);
	if (!EventTag.IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid or unregistered gameplay tag: %s"), *EventTagStr));
	}

	// Find SendStateTreeEvent UFunction
	UFunction* SendEventFunc = Component->FindFunction(FName("SendStateTreeEvent"));
	if (!IsValid(SendEventFunc))
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
	if (IsValid(IsRunningFunc))
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
