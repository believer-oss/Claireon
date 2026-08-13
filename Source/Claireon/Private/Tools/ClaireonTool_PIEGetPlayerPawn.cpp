// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIEGetPlayerPawn.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"

FString ClaireonTool_PIEGetPlayerPawn::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_PIEGetPlayerPawn::GetOperation() const { return TEXT("get_player_pawn"); }

FString ClaireonTool_PIEGetPlayerPawn::GetDescription() const
{
	return TEXT("Get the player pawn by player index in the active PIE session. Returns a stable actor ID for use with other PIE tools.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIEGetPlayerPawn::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// playerIndex - optional, default 0
	TSharedPtr<FJsonObject> PlayerIndexProp = MakeShared<FJsonObject>();
	PlayerIndexProp->SetStringField(TEXT("type"), TEXT("integer"));
	PlayerIndexProp->SetStringField(TEXT("description"),
		TEXT("Player index to retrieve (default: 0, the first local player)"));
	PlayerIndexProp->SetNumberField(TEXT("default"), 0);
	Properties->SetObjectField(TEXT("playerIndex"), PlayerIndexProp);

	// includeDetails - optional, default false
	TSharedPtr<FJsonObject> IncludeDetailsProp = MakeShared<FJsonObject>();
	IncludeDetailsProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeDetailsProp->SetStringField(TEXT("description"),
		TEXT("If true, include location, rotation, and component list in the response"));
	IncludeDetailsProp->SetBoolField(TEXT("default"), false);
	Properties->SetObjectField(TEXT("includeDetails"), IncludeDetailsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIEGetPlayerPawn::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	UE_LOG(LogClaireon, Display, TEXT("[MCP] pie_get_player_pawn"));

	if (!IsValid(GEditor))
	{
		return MakeErrorResult(TEXT("Editor is not available"));
	}

	if (!GEditor->IsPlaySessionInProgress())
	{
		return MakeErrorResult(TEXT("PIE is not running. Start a PIE session first with pie_start"));
	}

	// Parse parameters
	int32 PlayerIndex = 0;
	bool bIncludeDetails = false;

	if (Arguments.IsValid())
	{
		if (Arguments->HasField(TEXT("playerIndex")))
		{
			PlayerIndex = static_cast<int32>(Arguments->GetNumberField(TEXT("playerIndex")));
		}
		if (Arguments->HasField(TEXT("includeDetails")))
		{
			bIncludeDetails = Arguments->GetBoolField(TEXT("includeDetails"));
		}
	}

	// Find PIE world
	UWorld* PIEWorld = nullptr;
	for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
	{
		if (WorldContext.WorldType == EWorldType::PIE && IsValid(WorldContext.World()))
		{
			PIEWorld = WorldContext.World();
			break;
		}
	}

	if (!IsValid(PIEWorld))
	{
		return MakeErrorResult(TEXT("PIE world not found. PIE may still be initializing — use pie_wait_for with condition 'pieReady'"));
	}

	// Find player controller at the requested index
	APlayerController* TargetPC = nullptr;
	int32 CurrentIndex = 0;
	for (auto It = PIEWorld->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (IsValid(PC) && CurrentIndex == PlayerIndex)
		{
			TargetPC = PC;
			break;
		}
		CurrentIndex++;
	}

	if (!IsValid(TargetPC))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("No player controller found at index %d. Found %d controller(s) in PIE world."),
			PlayerIndex, CurrentIndex));
	}

	// Get the pawn
	APawn* Pawn = TargetPC->GetPawn();
	if (!IsValid(Pawn))
	{
		// Player controller exists but no pawn yet — might still be spawning
		FString Output;
		Output += TEXT("isValid: false\n");
		Output += FString::Printf(TEXT("playerIndex: %d\n"), PlayerIndex);
		Output += TEXT("pawn: (null — pawn has not been spawned yet)\n");
		Output += FString::Printf(TEXT("controllerClass: %s\n"), *TargetPC->GetClass()->GetName());
		Output += TEXT("Note: The player controller exists but has no pawn. The pawn may still be initializing. ");
		Output += TEXT("Use pie_wait_for with condition 'initState' to wait for full initialization.\n");

		// P2-4: a machine-readable status alongside the prose, so "controller
		// exists, pawn not spawned yet" is distinguishable from the error
		// statuses (no PIE / no world / no controller) without string-matching.
		TSharedPtr<FJsonObject> NoPawnData = MakeShared<FJsonObject>();
		NoPawnData->SetStringField(TEXT("status"), TEXT("no_pawn"));
		NoPawnData->SetBoolField(TEXT("is_valid"), false);
		NoPawnData->SetNumberField(TEXT("player_index"), PlayerIndex);
		NoPawnData->SetStringField(TEXT("controller_class"), TargetPC->GetClass()->GetName());
		return MakeSuccessResult(NoPawnData, Output);
	}

	// Register with PIE manager for stable ID tracking
	FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
	const FString ActorId = PIEManager.GetActorId(Pawn);

	// P2-4: structured result. actor_id is the field every downstream PIE tool
	// consumes; parsing it out of prose was the reported friction.
	TSharedPtr<FJsonObject> PawnData = MakeShared<FJsonObject>();
	PawnData->SetStringField(TEXT("status"), TEXT("ok"));
	PawnData->SetStringField(TEXT("actor_id"), ActorId);
	PawnData->SetBoolField(TEXT("is_valid"), true);
	PawnData->SetStringField(TEXT("class_name"), Pawn->GetClass()->GetName());
	PawnData->SetStringField(TEXT("actor_name"), Pawn->GetName());
	PawnData->SetNumberField(TEXT("player_index"), PlayerIndex);

	// Build output
	FString Output;
	Output += FString::Printf(TEXT("actorId: %s\n"), *ActorId);
	Output += TEXT("isValid: true\n");
	Output += FString::Printf(TEXT("className: %s\n"), *Pawn->GetClass()->GetName());
	Output += FString::Printf(TEXT("actorName: %s\n"), *Pawn->GetName());
	Output += FString::Printf(TEXT("playerIndex: %d\n"), PlayerIndex);

	if (bIncludeDetails)
	{
		// Location
		const FVector Location = Pawn->GetActorLocation();
		Output += FString::Printf(TEXT("location: X=%.2f Y=%.2f Z=%.2f\n"),
			Location.X, Location.Y, Location.Z);

		// Rotation
		const FRotator Rotation = Pawn->GetActorRotation();
		Output += FString::Printf(TEXT("rotation: Pitch=%.2f Yaw=%.2f Roll=%.2f\n"),
			Rotation.Pitch, Rotation.Yaw, Rotation.Roll);

		// Health (if pawn has health attribute — check for common patterns)
		Output += FString::Printf(TEXT("controllerClass: %s\n"), *TargetPC->GetClass()->GetName());

		// Component list
		TArray<UActorComponent*> Components;
		Pawn->GetComponents(Components);
		Output += FString::Printf(TEXT("componentCount: %d\n"), Components.Num());
		Output += TEXT("components:\n");
		for (const UActorComponent* Component : Components)
		{
			if (IsValid(Component))
			{
				Output += FString::Printf(TEXT("  - %s (%s)\n"),
					*Component->GetName(), *Component->GetClass()->GetName());
			}
		}

		TSharedPtr<FJsonObject> LocationObj = MakeShared<FJsonObject>();
		LocationObj->SetNumberField(TEXT("x"), Location.X);
		LocationObj->SetNumberField(TEXT("y"), Location.Y);
		LocationObj->SetNumberField(TEXT("z"), Location.Z);
		PawnData->SetObjectField(TEXT("location"), LocationObj);

		TSharedPtr<FJsonObject> RotationObj = MakeShared<FJsonObject>();
		RotationObj->SetNumberField(TEXT("pitch"), Rotation.Pitch);
		RotationObj->SetNumberField(TEXT("yaw"), Rotation.Yaw);
		RotationObj->SetNumberField(TEXT("roll"), Rotation.Roll);
		PawnData->SetObjectField(TEXT("rotation"), RotationObj);

		PawnData->SetStringField(TEXT("controller_class"), TargetPC->GetClass()->GetName());

		TArray<TSharedPtr<FJsonValue>> ComponentArr;
		ComponentArr.Reserve(Components.Num());
		for (const UActorComponent* Component : Components)
		{
			if (IsValid(Component))
			{
				TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
				CompObj->SetStringField(TEXT("name"), Component->GetName());
				CompObj->SetStringField(TEXT("class_name"), Component->GetClass()->GetName());
				ComponentArr.Add(MakeShared<FJsonValueObject>(CompObj));
			}
		}
		PawnData->SetNumberField(TEXT("component_count"), ComponentArr.Num());
		PawnData->SetArrayField(TEXT("components"), ComponentArr);
	}

	return MakeSuccessResult(PawnData, Output);
}
