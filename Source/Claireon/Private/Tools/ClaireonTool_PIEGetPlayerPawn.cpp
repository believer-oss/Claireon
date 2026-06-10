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
	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.pie.getPlayerPawnByPlayerIndex"));

	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor is not available"));
	}

	if (!GEditor->IsPlaySessionInProgress())
	{
		return MakeErrorResult(TEXT("PIE is not running. Start a PIE session first with editor.pie.start"));
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
		if (WorldContext.WorldType == EWorldType::PIE && WorldContext.World())
		{
			PIEWorld = WorldContext.World();
			break;
		}
	}

	if (!PIEWorld)
	{
		return MakeErrorResult(TEXT("PIE world not found. PIE may still be initializing — use editor.pie.waitFor with condition 'pieReady'"));
	}

	// Find player controller at the requested index
	APlayerController* TargetPC = nullptr;
	int32 CurrentIndex = 0;
	for (auto It = PIEWorld->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (PC && CurrentIndex == PlayerIndex)
		{
			TargetPC = PC;
			break;
		}
		CurrentIndex++;
	}

	if (!TargetPC)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("No player controller found at index %d. Found %d controller(s) in PIE world."),
			PlayerIndex, CurrentIndex));
	}

	// Get the pawn
	APawn* Pawn = TargetPC->GetPawn();
	if (!Pawn)
	{
		// Player controller exists but no pawn yet — might still be spawning
		FString Output;
		Output += TEXT("isValid: false\n");
		Output += FString::Printf(TEXT("playerIndex: %d\n"), PlayerIndex);
		Output += TEXT("pawn: (null — pawn has not been spawned yet)\n");
		Output += FString::Printf(TEXT("controllerClass: %s\n"), *TargetPC->GetClass()->GetName());
		Output += TEXT("Note: The player controller exists but has no pawn. The pawn may still be initializing. ");
		Output += TEXT("Use editor.pie.waitFor with condition 'initState' to wait for full initialization.\n");
		return MakeSuccessResult(nullptr, Output);
	}

	// Register with PIE manager for stable ID tracking
	FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
	const FString ActorId = PIEManager.GetActorId(Pawn);

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
			if (Component)
			{
				Output += FString::Printf(TEXT("  - %s (%s)\n"),
					*Component->GetName(), *Component->GetClass()->GetName());
			}
		}
	}

	return MakeSuccessResult(nullptr, Output);
}
