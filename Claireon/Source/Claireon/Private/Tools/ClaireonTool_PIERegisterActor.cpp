// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIERegisterActor.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

FString ClaireonTool_PIERegisterActor::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_PIERegisterActor::GetOperation() const { return TEXT("register_actor"); }

FString ClaireonTool_PIERegisterActor::GetDescription() const
{
	return TEXT("Resolve a PIE UObject path (e.g. '/Game/Maps/UEDPIE_0_MyMap.MyMap:PersistentLevel.BP_FSAIController_C_1') to a stable actor_id usable with statetree_runtime_inspect and other pie_* tools. Errors out when called outside PIE or when the path does not resolve. Read-only.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIERegisterActor::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> ActorPathProp = MakeShared<FJsonObject>();
	ActorPathProp->SetStringField(TEXT("type"), TEXT("string"));
	ActorPathProp->SetStringField(TEXT("description"),
		TEXT("Full PIE UObject path to the actor (e.g. '/Game/Maps/UEDPIE_0_MyMap.MyMap:PersistentLevel.BP_FSAIController_C_1')."));
	Properties->SetObjectField(TEXT("actor_path"), ActorPathProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actor_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIERegisterActor::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString ActorPath;
	if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: actor_path"));
	}

	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor is not available"));
	}

	if (!GEditor->IsPlaySessionInProgress())
	{
		return MakeErrorResult(TEXT("PIE is not running. Start a PIE session first with pie_start."));
	}

	// Locate the PIE world. Required so we can scope the actor lookup -- a path
	// from a stopped previous PIE session would otherwise leak through.
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
		return MakeErrorResult(TEXT("PIE world not found. PIE may still be initializing -- use pie_wait_for with condition 'pieReady'."));
	}

	// Resolve the path. PIE actor paths are fully-qualified UObject paths of the
	// form "/Game/.../UEDPIE_0_<Map>.<Map>:PersistentLevel.<ActorName>", which
	// ClaireonPathResolver does not handle, so we go straight to FindObject.
	AActor* Actor = FindObject<AActor>(nullptr, *ActorPath);
	if (!Actor)
	{
		// Fall back to scanning the PIE world's actor list and matching against the
		// full path. Some PIE actor names contain characters FindObject parses
		// differently (e.g. ':' separator vs '.'), so this second pass covers cases
		// where the caller supplied a slightly different canonical form.
		for (TActorIterator<AActor> It(PIEWorld); It; ++It)
		{
			AActor* Candidate = *It;
			if (Candidate && Candidate->GetPathName() == ActorPath)
			{
				Actor = Candidate;
				break;
			}
		}
	}
	if (!Actor)
	{
		return MakeErrorResult(FString::Printf(TEXT("Actor not found at path: %s"), *ActorPath));
	}

	// Verify the actor lives in the PIE world. FindObject above is global; an
	// editor-world actor with the same path shape must not be silently registered.
	if (Actor->GetWorld() != PIEWorld)
	{
		return MakeErrorResult(FString::Printf(TEXT("Actor '%s' is not part of the active PIE world."), *ActorPath));
	}

	FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
	const FString ActorId = PIEManager.GetActorId(Actor);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_id"), ActorId);
	Data->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	Data->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());
	Data->SetStringField(TEXT("actor_name"), Actor->GetName());

	const FString Summary = FString::Printf(TEXT("Registered '%s' as %s"), *Actor->GetName(), *ActorId);
	return MakeSuccessResult(Data, Summary);
}
