// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonComponentTickInspectTool.h"

#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"

#include "Components/ActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"

namespace ClaireonComponentTickInspectScratch
{
	// Decode TickGroup enum (ETickingGroup) to string. Mirror of engine values declared
	// at C:/UnrealEngine/Engine/Source/Runtime/Engine/Classes/Engine/EngineBaseTypes.h:171.
	static FString TickingGroupToString(ETickingGroup InGroup)
	{
		switch (InGroup)
		{
		case TG_PrePhysics:           return TEXT("TG_PrePhysics");
		case TG_StartPhysics:         return TEXT("TG_StartPhysics");
		case TG_DuringPhysics:        return TEXT("TG_DuringPhysics");
		case TG_EndPhysics:           return TEXT("TG_EndPhysics");
		case TG_PostPhysics:          return TEXT("TG_PostPhysics");
		case TG_PostUpdateWork:       return TEXT("TG_PostUpdateWork");
		case TG_LastDemotable:        return TEXT("TG_LastDemotable");
		case TG_NewlySpawned:         return TEXT("TG_NewlySpawned");
		case TG_MAX:                  return TEXT("TG_MAX");
		default:                      return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(InGroup));
		}
	}

	// Build the JSON description of a single FTickFunction.
	static TSharedPtr<FJsonObject> BuildTickFunctionJson(const FTickFunction& Tick)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("canEverTick"), Tick.bCanEverTick != 0);
		Obj->SetBoolField(TEXT("startWithTickEnabled"), Tick.bStartWithTickEnabled != 0);
		Obj->SetStringField(TEXT("tickGroup"), TickingGroupToString(Tick.TickGroup));
		Obj->SetStringField(TEXT("endTickGroup"), TickingGroupToString(Tick.EndTickGroup));
		Obj->SetNumberField(TEXT("tickInterval"), Tick.TickInterval);
		Obj->SetBoolField(TEXT("tickEvenWhenPaused"), Tick.bTickEvenWhenPaused != 0);
		Obj->SetBoolField(TEXT("highPriority"), Tick.bHighPriority != 0);
		Obj->SetBoolField(TEXT("allowTickOnDedicatedServer"), Tick.bAllowTickOnDedicatedServer != 0);
		return Obj;
	}

	// Decode EVisibilityBasedAnimTickOption to its declared name.
	// Source: C:/UnrealEngine/Engine/Source/Runtime/Engine/Classes/Components/SkinnedMeshComponent.h:84
	static FString VisibilityBasedAnimTickOptionToString(EVisibilityBasedAnimTickOption InOption)
	{
		switch (InOption)
		{
		case EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones:
			return TEXT("AlwaysTickPoseAndRefreshBones");
		case EVisibilityBasedAnimTickOption::AlwaysTickPose:
			return TEXT("AlwaysTickPose");
		case EVisibilityBasedAnimTickOption::OnlyTickMontagesWhenNotRendered:
			return TEXT("OnlyTickMontagesWhenNotRendered");
		case EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered:
			return TEXT("OnlyTickPoseWhenRendered");
		default:
			return FString::Printf(TEXT("Unknown(%d)"), static_cast<int32>(InOption));
		}
	}
}

FString ClaireonTool_ComponentTickInspect::GetDescription() const
{
    return TEXT("Snapshot per-component tick state on any actor in the paused PIE world. Reports actor and per-component tick-enabled flags plus the static FTickFunction settings (tick group, interval, priority). Adds skeletalMeshExtras for each USkeletalMeshComponent. Read-only / non-session inspector. Targets the budgeter-disabled-server-mesh-tick bug class.");
}

TSharedPtr<FJsonObject> ClaireonTool_ComponentTickInspect::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> ActorIdProp = MakeShared<FJsonObject>();
	ActorIdProp->SetStringField(TEXT("type"), TEXT("string"));
	ActorIdProp->SetStringField(TEXT("description"),
		TEXT("The stable actor ID assigned by PIE tools (e.g., 'actor_0')."));
	Properties->SetObjectField(TEXT("actorId"), ActorIdProp);

	TSharedPtr<FJsonObject> ClassFilterProp = MakeShared<FJsonObject>();
	ClassFilterProp->SetStringField(TEXT("type"), TEXT("string"));
	ClassFilterProp->SetStringField(TEXT("description"),
		TEXT("Optional substring filter on component class name (case-insensitive)."));
	Properties->SetObjectField(TEXT("componentClass"), ClassFilterProp);

	TSharedPtr<FJsonObject> NameFilterProp = MakeShared<FJsonObject>();
	NameFilterProp->SetStringField(TEXT("type"), TEXT("string"));
	NameFilterProp->SetStringField(TEXT("description"),
		TEXT("Optional substring filter on component instance name (case-insensitive)."));
	Properties->SetObjectField(TEXT("componentName"), NameFilterProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actorId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FString ClaireonTool_ComponentTickInspect::GetExampleUsage() const
{
	return TEXT("component_tick_inspect(actorId=\"actor_0\", componentClass=\"SkeletalMesh\")");
}

TSharedPtr<FJsonObject> ClaireonTool_ComponentTickInspect::GetParameterTooltips() const
{
	TSharedPtr<FJsonObject> Tooltips = MakeShared<FJsonObject>();
	Tooltips->SetStringField(TEXT("actorId"),
		TEXT("The stable actor ID assigned by PIE tools (e.g., 'actor_0')."));
	Tooltips->SetStringField(TEXT("componentClass"),
		TEXT("Optional substring filter on component class name."));
	Tooltips->SetStringField(TEXT("componentName"),
		TEXT("Optional substring filter on component instance name."));
	return Tooltips;
}

TArray<FString> ClaireonTool_ComponentTickInspect::GetSearchKeywords() const
{
	return {
		TEXT("tick disabled"),
		TEXT("componentTickEnabled"),
		TEXT("skeletal mesh tick"),
		TEXT("VisibilityBasedAnimTickOption"),
		TEXT("tick budgeter")
	};
}

IClaireonTool::FToolResult ClaireonTool_ComponentTickInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	using namespace ClaireonComponentTickInspectScratch;

	FString ActorId;
	if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("actorId"), ActorId))
	{
		return MakeErrorResult(TEXT("Missing required argument: actorId"));
	}

	FString ClassFilter;
	Arguments->TryGetStringField(TEXT("componentClass"), ClassFilter);
	FString NameFilter;
	Arguments->TryGetStringField(TEXT("componentName"), NameFilter);

	if (!GEditor)
	{
		return MakeErrorResult(TEXT("GEditor is not available"));
	}

	UWorld* PIEWorld = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE && Context.World())
		{
			PIEWorld = Context.World();
			break;
		}
	}

	if (!PIEWorld)
	{
		return MakeErrorResult(TEXT("No active PIE session"));
	}

	FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
	AActor* Actor = PIEManager.ResolveActorId(ActorId, PIEWorld);
	if (!Actor)
	{
		return MakeErrorResult(FString::Printf(TEXT("Actor not found for ID: %s"), *ActorId));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actorId"), ActorId);
	Data->SetBoolField(TEXT("actorTickEnabled"), Actor->IsActorTickEnabled());

	// Actor primary tick state.
	Data->SetObjectField(TEXT("actorPrimaryTick"), BuildTickFunctionJson(Actor->PrimaryActorTick));

	// Components.
	TArray<TSharedPtr<FJsonValue>> ComponentsArray;
	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);

	int32 MatchedCount = 0;
	for (UActorComponent* Component : Components)
	{
		if (!Component)
		{
			continue;
		}

		const FString ComponentClassName = Component->GetClass()->GetName();
		const FString ComponentInstanceName = Component->GetName();

		if (!ClassFilter.IsEmpty() && !ComponentClassName.Contains(ClassFilter))
		{
			continue;
		}
		if (!NameFilter.IsEmpty() && !ComponentInstanceName.Contains(NameFilter))
		{
			continue;
		}

		TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
		CompObj->SetStringField(TEXT("name"), ComponentInstanceName);
		CompObj->SetStringField(TEXT("class"), ComponentClassName);
		CompObj->SetBoolField(TEXT("componentTickEnabled"), Component->IsComponentTickEnabled());
		CompObj->SetObjectField(TEXT("primaryComponentTick"), BuildTickFunctionJson(Component->PrimaryComponentTick));

		// Skeletal-mesh-specific extras. VisibilityBasedAnimTickOption and bDisableMorphTarget are
		// declared on the parent USkinnedMeshComponent (SkinnedMeshComponent.h:694, :750) but
		// inherited by USkeletalMeshComponent.
		if (USkeletalMeshComponent* SkelMesh = Cast<USkeletalMeshComponent>(Component))
		{
			TSharedPtr<FJsonObject> SkelExtras = MakeShared<FJsonObject>();
			SkelExtras->SetStringField(TEXT("visibilityBasedAnimTickOption"),
				VisibilityBasedAnimTickOptionToString(SkelMesh->VisibilityBasedAnimTickOption));
			SkelExtras->SetBoolField(TEXT("recentlyRendered"), SkelMesh->WasRecentlyRendered());
			SkelExtras->SetBoolField(TEXT("disableMorphTarget"), SkelMesh->bDisableMorphTarget != 0);
			SkelExtras->SetNumberField(TEXT("lastRenderTime"), SkelMesh->GetLastRenderTime());
			CompObj->SetObjectField(TEXT("skeletalMeshExtras"), SkelExtras);
		}

		ComponentsArray.Add(MakeShared<FJsonValueObject>(CompObj));
		++MatchedCount;
	}

	Data->SetArrayField(TEXT("components"), ComponentsArray);

	const FString Summary = FString::Printf(
		TEXT("Tick state for %s: actorTick=%s, %d component(s)%s%s"),
		*ActorId,
		Actor->IsActorTickEnabled() ? TEXT("on") : TEXT("off"),
		MatchedCount,
		(!ClassFilter.IsEmpty() || !NameFilter.IsEmpty()) ? TEXT(" (filtered)") : TEXT(""),
		TEXT(""));

	return MakeSuccessResult(Data, Summary);
}
