// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_StateTreeRuntimeInspect.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"
#include "ClaireonPIEWorldResolver.h"
#include "ClaireonStateTreeComponentResolver.h"
#include "StateTree.h"
#include "StateTreeTypes.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"

FString ClaireonTool_StateTreeRuntimeInspect::GetCategory() const { return TEXT("statetree"); }
FString ClaireonTool_StateTreeRuntimeInspect::GetOperation() const { return TEXT("runtime_inspect"); }

FString ClaireonTool_StateTreeRuntimeInspect::GetDescription() const
{
	return TEXT("Query the runtime execution state of a running State Tree instance during PIE. Stateless / read-only / non-session: never mutates and requires no open editing session, but does require an active PIE session. Returns active states, recent transitions, and context data for the target actor's State Tree component.");
}

TSharedPtr<FJsonObject> ClaireonTool_StateTreeRuntimeInspect::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> ActorIdProp = MakeShared<FJsonObject>();
	ActorIdProp->SetStringField(TEXT("type"), TEXT("string"));
	ActorIdProp->SetStringField(TEXT("description"), TEXT("PIE actor ID (from existing PIE tools, e.g. 'actor_0')"));
	Properties->SetObjectField(TEXT("actor_id"), ActorIdProp);

	TSharedPtr<FJsonObject> ComponentClassProp = MakeShared<FJsonObject>();
	ComponentClassProp->SetStringField(TEXT("type"), TEXT("string"));
	ComponentClassProp->SetStringField(TEXT("description"), TEXT("Optional component class-name substring, case-insensitive. Default: the first component of any UStateTreeComponent subclass, StateTreeAIComponent included. Use this only to disambiguate between several."));
	Properties->SetObjectField(TEXT("component_class"), ComponentClassProp);

	TSharedPtr<FJsonObject> DetailProp = MakeShared<FJsonObject>();
	DetailProp->SetStringField(TEXT("type"), TEXT("string"));
	DetailProp->SetStringField(TEXT("description"), TEXT("Level of detail: 'summary' for active states only, 'full' for complete execution state (default: summary)"));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("summary")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("full")));
		DetailProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("detail_level"), DetailProp);

	// pie_instance / net_mode - optional PIE world selectors (shared contract).
	ClaireonPIEWorldResolver::AddSchemaParams(Properties);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actor_id")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_StateTreeRuntimeInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString ActorId;
	if (!Arguments->TryGetStringField(TEXT("actor_id"), ActorId) || ActorId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: actor_id"));
	}

	FString ComponentClass;
	Arguments->TryGetStringField(TEXT("component_class"), ComponentClass);

	FString DetailLevel = TEXT("summary");
	Arguments->TryGetStringField(TEXT("detail_level"), DetailLevel);

	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.statetree.runtime.inspect: actor=%s, component=%s, detail=%s"),
		*ActorId, *ComponentClass, *DetailLevel);

	// Get PIE world (honors optional pie_instance / net_mode selectors).
	FString PIEResolveError;
	UWorld* PIEWorld = ClaireonPIEWorldResolver::ResolvePIEWorld(Arguments, PIEResolveError);
	if (!IsValid(PIEWorld))
	{
		return MakeErrorResult(PIEResolveError);
	}

	// Resolve actor
	FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
	AActor* Actor = PIEManager.ResolveActorId(ActorId, PIEWorld);
	if (!IsValid(Actor))
	{
		return MakeErrorResult(FString::Printf(TEXT("Actor not found or destroyed: %s"), *ActorId));
	}

	// Find State Tree component
	UActorComponent* Component = ClaireonStateTreeComponentResolver::FindStateTreeComponent(Actor, ComponentClass);
	if (!IsValid(Component))
	{
		// List available components for debugging
		const FString ComponentList = ClaireonStateTreeComponentResolver::DescribeComponents(Actor);
		return MakeErrorResult(FString::Printf(TEXT("No State Tree component found on actor %s (%s). Components:%s"),
			*ActorId, *Actor->GetClass()->GetName(), *ComponentList));
	}

	FString Output;
	Output += FString::Printf(TEXT("=== Runtime State Tree ===\n"));
	Output += FString::Printf(TEXT("Actor: %s (%s)\n"), *ActorId, *Actor->GetClass()->GetName());
	Output += FString::Printf(TEXT("Component: %s (%s)\n"), *Component->GetName(), *Component->GetClass()->GetName());

	// Try to get the State Tree asset via UFunction reflection
	UFunction* GetStateTreeFunc = Component->FindFunction(FName("GetStateTree"));
	if (IsValid(GetStateTreeFunc))
	{
		struct
		{
			UStateTree* ReturnValue = nullptr;
		} Parms;
		Component->ProcessEvent(GetStateTreeFunc, &Parms);
		if (IsValid(Parms.ReturnValue))
		{
			Output += FString::Printf(TEXT("State Tree: %s\n"), *Parms.ReturnValue->GetName());
		}
	}

	// Try to get active states via reflection
	// Look for IsRunning property or function
	UFunction* IsRunningFunc = Component->FindFunction(FName("IsRunning"));
	if (IsValid(IsRunningFunc))
	{
		struct
		{
			bool ReturnValue = false;
		} Parms;
		Component->ProcessEvent(IsRunningFunc, &Parms);
		Output += FString::Printf(TEXT("Status: %s\n"), Parms.ReturnValue ? TEXT("Running") : TEXT("Stopped"));
	}

	// Try GetActiveStateNames or similar debug info
	UFunction* GetDebugInfoFunc = Component->FindFunction(FName("GetDebugInfoString"));
	if (IsValid(GetDebugInfoFunc))
	{
		struct
		{
			FString ReturnValue;
		} Parms;
		Component->ProcessEvent(GetDebugInfoFunc, &Parms);
		if (!Parms.ReturnValue.IsEmpty())
		{
			Output += FString::Printf(TEXT("\n=== Debug Info ===\n%s\n"), *Parms.ReturnValue);
		}
	}

	// Iterate properties for additional state info when detail_level is full
	if (DetailLevel == TEXT("full"))
	{
		Output += TEXT("\n=== Component Properties ===\n");
		for (TFieldIterator<FProperty> It(Component->GetClass()); It; ++It)
		{
			const FProperty* Prop = *It;
			// Skip internal UObject properties
			if (Prop->GetOwnerClass() == UObject::StaticClass() || Prop->GetOwnerClass() == UActorComponent::StaticClass())
			{
				continue;
			}

			FString ValueStr;
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Component);
			Prop->ExportText_Direct(ValueStr, ValuePtr, ValuePtr, nullptr, PPF_None);

			if (ValueStr.Len() > 100)
				ValueStr = ValueStr.Left(97) + TEXT("...");
			Output += FString::Printf(TEXT("  %s: %s\n"), *Prop->GetName(), *ValueStr);
		}
	}

	return MakeSuccessResult(nullptr, Output);
}
