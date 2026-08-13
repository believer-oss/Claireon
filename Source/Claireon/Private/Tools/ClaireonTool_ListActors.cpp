// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ListActors.h"
#include "ClaireonLog.h"
#include "ClaireonPIEWorldResolver.h"
#include "ClaireonNameResolver.h"

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
    return TEXT("List actors in the currently loaded map, optionally filtered by class name or by a wildcard "
        "pattern matched against BOTH the actor label and object name. PIE-aware: world_context='auto' "
        "(default) returns PIE actors while PIE runs, editor actors otherwise; pie_instance/net_mode "
        "pick a specific PIE world. Stateless / read-only / non-session.");
}

TSharedPtr<FJsonObject> ClaireonTool_ListActors::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// class_filter - optional
	TSharedPtr<FJsonObject> ClassProp = MakeShared<FJsonObject>();
	ClassProp->SetStringField(TEXT("type"), TEXT("string"));
	ClassProp->SetStringField(TEXT("description"), TEXT("Filter by actor class (is-a test; accepts short names, U/A-prefixed names, BP class names and /Script/ paths, e.g. Pawn, StaticMeshActor, BP_Turret_C). Subclasses are included unless include_subclasses=false. When the name does not resolve to a class, falls back to the legacy case-insensitive substring match on class names, disclosed by a warning."));
	Properties->SetObjectField(TEXT("class_filter"), ClassProp);

	// include_subclasses - optional (P2-8; operator-decided default true)
	TSharedPtr<FJsonObject> SubclassesProp = MakeShared<FJsonObject>();
	SubclassesProp->SetStringField(TEXT("type"), TEXT("boolean"));
	SubclassesProp->SetBoolField(TEXT("default"), true);
	SubclassesProp->SetStringField(TEXT("description"), TEXT("With class_filter: true (default) matches the class and every subclass (is-a); false matches the exact class only. No effect on the substring fallback or without class_filter."));
	Properties->SetObjectField(TEXT("include_subclasses"), SubclassesProp);

	// label_pattern - optional
	TSharedPtr<FJsonObject> LabelProp = MakeShared<FJsonObject>();
	LabelProp->SetStringField(TEXT("type"), TEXT("string"));
	LabelProp->SetStringField(TEXT("description"),
		TEXT("Wildcard pattern matched against the actor label OR the actor object name; the actor is included "
			 "if either matches (e.g. *Player*, SM_Wall*, BP_Turret_C_*). Labels are the editable display names "
			 "shown in the outliner; object names are the stable FNames (e.g. 'BP_Turret_C_1') and can differ "
			 "from labels, especially at runtime."));
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

	// pie_instance / net_mode - optional PIE world selectors (shared contract).
	// Ignored when world_context='editor'.
	ClaireonPIEWorldResolver::AddSchemaParams(Properties);

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
		// Look for a live PIE world, honoring the optional pie_instance /
		// net_mode selectors (shared resolver, WI-13).
		FString PIEResolveError;
		UWorld* PIEWorld = ClaireonPIEWorldResolver::ResolvePIEWorld(Arguments, PIEResolveError);
		if (IsValid(PIEWorld))
		{
			World = PIEWorld;
			UsedContext = TEXT("pie");
		}
		else if (WorldContext == TEXT("pie"))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("%s Use world_context='editor' to list editor-world actors."), *PIEResolveError));
		}
		else if (Arguments->HasField(TEXT("pie_instance")) || Arguments->HasField(TEXT("net_mode")))
		{
			// The caller explicitly asked for a specific PIE world under
			// world_context='auto'; silently falling back to the editor world
			// would return the wrong actor set.
			return MakeErrorResult(PIEResolveError);
		}
	}

	if (!IsValid(World))
	{
		// Fall through to editor world.
		if (!IsValid(GEditor))
		{
			return MakeErrorResult(TEXT("Editor is not available. Wait for the editor to finish initializing."));
		}
		World = GEditor->GetEditorWorldContext().World();
		if (!IsValid(World))
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

	// P2-8 (operator decision 2026-08-12): default TRUE -- the honest is-a
	// semantic. Existing calls can return MORE actors than before.
	bool bIncludeSubclasses = true;
	if (Arguments->HasField(TEXT("include_subclasses")))
	{
		bIncludeSubclasses = Arguments->GetBoolField(TEXT("include_subclasses"));
	}

	FString LabelPattern;
	if (Arguments->HasField(TEXT("label_pattern")))
	{
		LabelPattern = Arguments->GetStringField(TEXT("label_pattern"));
	}

	// P2-8: resolve class_filter to a real UClass and use is-a semantics; the
	// old case-insensitive substring test missed every BP subclass whose name
	// lacked the filter text ("Pawn" found no BP pawns) and matched unrelated
	// classes that happened to contain it. Substring survives ONLY as an
	// explicit fallback when resolution fails, disclosed via a warning -- the
	// same call must never silently take either semantic.
	UClass* ResolvedFilterClass = nullptr;
	TArray<FString> FilterWarnings;
	if (!ClassFilter.IsEmpty())
	{
		ClaireonNameResolver::FNameResolveResult ClassResult;
		ResolvedFilterClass = ClaireonNameResolver::ResolveClassName(ClassFilter, AActor::StaticClass(), ClassResult);
		if (IsValid(ResolvedFilterClass))
		{
			if (!ClassResult.ResolutionNote.IsEmpty())
			{
				FilterWarnings.Add(ClassResult.ResolutionNote);
			}
		}
		else
		{
			FilterWarnings.Add(FString::Printf(
				TEXT("class_filter '%s' did not resolve to an actor class (%s); falling back to the legacy "
				     "case-insensitive substring match on class names. include_subclasses has no effect on "
				     "the fallback."),
				*ClassFilter, *ClassResult.Error));
		}
	}

	// Iterate actors
	TArray<TSharedPtr<FJsonValue>> ActorsArray;
	int32 TotalCount = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor))
		{
			continue;
		}

		const FString ActorLabel = Actor->GetActorLabel();
		const FString ActorObjectName = Actor->GetName();
		const FString ActorClassName = Actor->GetClass()->GetName();

		// Apply class filter: is-a when the filter resolved (P2-8), substring
		// fallback (disclosed by warning) when it did not.
		if (!ClassFilter.IsEmpty())
		{
			if (IsValid(ResolvedFilterClass))
			{
				const bool bMatches = bIncludeSubclasses
					? Actor->IsA(ResolvedFilterClass)
					: (Actor->GetClass() == ResolvedFilterClass);
				if (!bMatches)
				{
					continue;
				}
			}
			else if (!ActorClassName.Contains(ClassFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		// Apply pattern (wildcard matching against label OR object name).
		// Labels and object names differ, especially for runtime-spawned
		// actors; matching either lets callers use whichever they have.
		if (!LabelPattern.IsEmpty())
		{
			const bool bLabelMatches = ActorLabel.MatchesWildcard(LabelPattern, ESearchCase::IgnoreCase);
			const bool bNameMatches = ActorObjectName.MatchesWildcard(LabelPattern, ESearchCase::IgnoreCase);
			if (!bLabelMatches && !bNameMatches)
			{
				continue;
			}
		}

		TotalCount++;

		// Build actor JSON object
		TSharedPtr<FJsonObject> ActorObj = MakeShared<FJsonObject>();
		ActorObj->SetStringField(TEXT("label"), ActorLabel);
		ActorObj->SetStringField(TEXT("name"), ActorObjectName);
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

	FToolResult Result = MakeSuccessResult(Data, Summary);
	Result.Warnings.Append(FilterWarnings);
	return Result;
}
