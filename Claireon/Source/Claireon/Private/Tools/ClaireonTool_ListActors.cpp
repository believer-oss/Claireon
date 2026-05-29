// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ListActors.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/World.h"

FString ClaireonTool_ListActors::GetCategory() const { return TEXT("level"); }
FString ClaireonTool_ListActors::GetOperation() const { return TEXT("list_actors"); }

FString ClaireonTool_ListActors::GetDescription() const
{
    return TEXT("List actors in the currently loaded map. Optionally filter by class name or label wildcard pattern. "
        "PIE-aware: world_context='auto' (default) returns PIE actors when PIE is running, editor actors otherwise. "
        "Returns actor labels, classes, locations, rotations, and full paths. Stateless / read-only / non-session.");
}

TSharedPtr<FJsonObject> ClaireonTool_ListActors::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// class_filter - optional
	TSharedPtr<FJsonObject> ClassProp = MakeShared<FJsonObject>();
	ClassProp->SetStringField(TEXT("type"), TEXT("string"));
	ClassProp->SetStringField(TEXT("description"), TEXT("Filter by actor class name (case-insensitive substring match, e.g. StaticMeshActor, PointLight)"));
	Properties->SetObjectField(TEXT("class_filter"), ClassProp);

	// label_pattern - optional
	TSharedPtr<FJsonObject> LabelProp = MakeShared<FJsonObject>();
	LabelProp->SetStringField(TEXT("type"), TEXT("string"));
	LabelProp->SetStringField(TEXT("description"), TEXT("Filter by actor label using wildcard matching (e.g. *Player*, SM_Wall*)"));
	Properties->SetObjectField(TEXT("label_pattern"), LabelProp);

	// world_context - optional
	TSharedPtr<FJsonObject> WorldCtxProp = MakeShared<FJsonObject>();
	WorldCtxProp->SetStringField(TEXT("type"), TEXT("string"));
	WorldCtxProp->SetStringField(TEXT("description"),
		TEXT("Which world to list actors from: 'auto' (default) uses PIE world if PIE is running, else editor world; "
			 "'pie' forces PIE world (errors if PIE not running); 'editor' forces editor world."));
	TArray<TSharedPtr<FJsonValue>> WorldCtxEnum;
	WorldCtxEnum.Add(MakeShared<FJsonValueString>(TEXT("auto")));
	WorldCtxEnum.Add(MakeShared<FJsonValueString>(TEXT("pie")));
	WorldCtxEnum.Add(MakeShared<FJsonValueString>(TEXT("editor")));
	WorldCtxProp->SetArrayField(TEXT("enum"), WorldCtxEnum);
	WorldCtxProp->SetStringField(TEXT("default"), TEXT("auto"));
	Properties->SetObjectField(TEXT("world_context"), WorldCtxProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_ListActors::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Parse world_context ('auto', 'pie', 'editor'; default 'auto').
	FString WorldContext;
	if (!Arguments->TryGetStringField(TEXT("world_context"), WorldContext) || WorldContext.IsEmpty())
	{
		WorldContext = TEXT("auto");
	}

	// Resolve the target world based on world_context.
	// Auto-route to PIE world when PIE is running so callers see live gameplay
	// actors rather than editor-world actors during PIE.
	UWorld* World = nullptr;
	FString UsedContext;

	if (WorldContext != TEXT("editor"))
	{
		// Look for a live PIE world.
		if (GEngine)
		{
			for (const FWorldContext& WorldCtx : GEngine->GetWorldContexts())
			{
				if (WorldCtx.WorldType == EWorldType::PIE && WorldCtx.World())
				{
					World = WorldCtx.World();
					UsedContext = TEXT("pie");
					break;
				}
			}
		}

		if (!World && WorldContext == TEXT("pie"))
		{
			return MakeErrorResult(TEXT("PIE is not running. Start a PIE session first, or use world_context='editor' to list editor-world actors."));
		}
	}

	if (!World)
	{
		// Fall through to editor world.
		if (!GEditor)
		{
			return MakeErrorResult(TEXT("Editor is not available. Wait for the editor to finish initializing."));
		}
		World = GEditor->GetEditorWorldContext().World();
		if (!World)
		{
			return MakeErrorResult(TEXT("No world loaded. Use open_map to load a map first."));
		}
		UsedContext = TEXT("editor");
	}

	// Parse remaining arguments
	FString ClassFilter;
	if (Arguments->HasField(TEXT("class_filter")))
	{
		ClassFilter = Arguments->GetStringField(TEXT("class_filter"));
	}

	FString LabelPattern;
	if (Arguments->HasField(TEXT("label_pattern")))
	{
		LabelPattern = Arguments->GetStringField(TEXT("label_pattern"));
	}

	// Iterate actors
	TArray<TSharedPtr<FJsonValue>> ActorsArray;
	int32 TotalCount = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}

		const FString ActorLabel = Actor->GetActorLabel();
		const FString ActorClassName = Actor->GetClass()->GetName();

		// Apply class filter (case-insensitive contains)
		if (!ClassFilter.IsEmpty())
		{
			if (!ActorClassName.Contains(ClassFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		// Apply label pattern (wildcard matching)
		if (!LabelPattern.IsEmpty())
		{
			if (!ActorLabel.MatchesWildcard(LabelPattern, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		TotalCount++;

		// Build actor JSON object
		TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("label"), ActorLabel);
		ActorObj->SetStringField(TEXT("class"), ActorClassName);

		// Location as [X, Y, Z]
		const FVector Location = Actor->GetActorLocation();
		TArray<TSharedPtr<FJsonValue>> LocationArray;
		LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
		LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
		LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
		ActorObj->SetArrayField(TEXT("location"), LocationArray);

		// Rotation as [Pitch, Yaw, Roll]
		const FRotator Rotation = Actor->GetActorRotation();
		TArray<TSharedPtr<FJsonValue>> RotationArray;
		RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Pitch));
		RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Yaw));
		RotationArray.Add(MakeShared<FJsonValueNumber>(Rotation.Roll));
		ActorObj->SetArrayField(TEXT("rotation"), RotationArray);

		ActorObj->SetStringField(TEXT("path"), Actor->GetPathName());

		ActorsArray.Add(MakeShared<FJsonValueObject>(ActorObj));
	}

	// Sort by label alphabetically
	ActorsArray.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		const FString LabelA = A->AsObject()->GetStringField(TEXT("label"));
		const FString LabelB = B->AsObject()->GetStringField(TEXT("label"));
		return LabelA < LabelB;
	});

	// Build result
	const FString MapName = World->GetMapName();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("actors"), ActorsArray);
	Data->SetNumberField(TEXT("total_count"), TotalCount);
	Data->SetStringField(TEXT("map_name"), MapName);
	Data->SetStringField(TEXT("world_context"), UsedContext);

	// Build summary
	FString Summary = FString::Printf(TEXT("Found %d actors in %s"), TotalCount, *MapName);
	if (!ClassFilter.IsEmpty())
	{
		Summary += FString::Printf(TEXT(" (filtered by %s: %d results)"), *ClassFilter, TotalCount);
	}
	if (!LabelPattern.IsEmpty())
	{
		Summary += FString::Printf(TEXT(" (label pattern \"%s\")"), *LabelPattern);
	}

	return MakeSuccessResult(Data, Summary);
}
