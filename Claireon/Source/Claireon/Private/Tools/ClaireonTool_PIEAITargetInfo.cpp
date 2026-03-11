// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIEAITargetInfo.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"

#include "AIController.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"

FString ClaireonTool_PIEAITargetInfo::GetName() const
{
	return TEXT("editor.pie.getAITargetInfo");
}

FString ClaireonTool_PIEAITargetInfo::GetDescription() const
{
	return TEXT("Get AI targeting information for a pawn: AI controller class, blackboard state, and current target (if any).");
}

TSharedPtr<FJsonObject> ClaireonTool_PIEAITargetInfo::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// actorId - required
	TSharedPtr<FJsonObject> ActorIdProp = MakeShared<FJsonObject>();
	ActorIdProp->SetStringField(TEXT("type"), TEXT("string"));
	ActorIdProp->SetStringField(TEXT("description"),
		TEXT("The stable actor ID of the pawn to inspect (e.g., 'actor_0')"));
	Properties->SetObjectField(TEXT("actorId"), ActorIdProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	// Required fields
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actorId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIEAITargetInfo::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.pie.getAITargetInfo"));

	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor is not available"));
	}

	if (!GEditor->IsPlaySessionInProgress())
	{
		return MakeErrorResult(TEXT("PIE is not running. Start a PIE session first with editor.pie.start"));
	}

	// Parse required parameter
	if (!Arguments.IsValid() || !Arguments->HasField(TEXT("actorId")))
	{
		return MakeErrorResult(TEXT("Missing required parameter: actorId"));
	}

	const FString ActorId = Arguments->GetStringField(TEXT("actorId"));

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

	// Try to cast to APawn to get the AI controller
	APawn* Pawn = Cast<APawn>(Actor);
	if (!Pawn)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Actor '%s' (%s) is not a Pawn. AI targeting info is only available for Pawns."),
			*ActorId, *Actor->GetClass()->GetName()));
	}

	// Build output
	FString Output;
	Output += FString::Printf(TEXT("actorId: %s\n"), *ActorId);
	Output += FString::Printf(TEXT("pawnClass: %s\n"), *Pawn->GetClass()->GetName());

	// Get the AI controller
	AController* Controller = Pawn->GetController();
	AAIController* AIController = Cast<AAIController>(Controller);

	Output += FString::Printf(TEXT("hasController: %s\n"), Controller ? TEXT("true") : TEXT("false"));

	if (Controller)
	{
		Output += FString::Printf(TEXT("controllerClass: %s\n"), *Controller->GetClass()->GetName());
		Output += FString::Printf(TEXT("isAIController: %s\n"), AIController ? TEXT("true") : TEXT("false"));
	}

	if (!AIController)
	{
		if (Controller)
		{
			Output += TEXT("Note: This pawn has a controller but it is not an AAIController. ");
			Output += TEXT("It may be a player controller or a custom controller type.\n");
		}
		else
		{
			Output += TEXT("Note: This pawn has no controller. It may not have been possessed yet.\n");
		}
		return MakeSuccessResult(nullptr, Output);
	}

	// Check for blackboard
	UBlackboardComponent* BlackboardComp = AIController->GetBlackboardComponent();
	Output += FString::Printf(TEXT("hasBlackboard: %s\n"), BlackboardComp ? TEXT("true") : TEXT("false"));

	if (BlackboardComp)
	{
		// Iterate blackboard keys to find target-related information
		bool bFoundTarget = false;
		FString TargetActorId;
		FString TargetActorClass;

		const UBlackboardData* BBAsset = BlackboardComp->GetBlackboardAsset();
		if (BBAsset)
		{
			Output += TEXT("blackboardKeys:\n");

			TArray<FBlackboard::FKey> AllKeys;
			// Iterate through keys looking for object-type keys that might be targets
			for (const auto& KeyEntry : BBAsset->Keys)
			{
				const FString KeyName = KeyEntry.EntryName.ToString();
				const FString KeyTypeName = KeyEntry.KeyType ? KeyEntry.KeyType->GetClass()->GetName() : TEXT("Unknown");

				// Try to get the value as a string
				const FBlackboard::FKey KeyId = BlackboardComp->GetKeyID(KeyEntry.EntryName);

				if (KeyEntry.KeyType && KeyEntry.KeyType->IsA(UBlackboardKeyType_Object::StaticClass()))
				{
					// This is an Object key — might be a target actor
					UObject* KeyValue = BlackboardComp->GetValueAsObject(KeyEntry.EntryName);
					AActor* TargetActor = Cast<AActor>(KeyValue);

					if (TargetActor)
					{
						const FString TargetId = PIEManager.GetActorId(TargetActor);
						Output += FString::Printf(TEXT("  %s (%s): %s (%s) [actorId: %s]\n"),
							*KeyName, *KeyTypeName,
							*TargetActor->GetName(), *TargetActor->GetClass()->GetName(),
							*TargetId);

						// Check if this looks like a target key
						if (KeyName.Contains(TEXT("Target")) || KeyName.Contains(TEXT("Enemy")) ||
							KeyName.Contains(TEXT("Focus")) || KeyName.Contains(TEXT("Aggro")))
						{
							bFoundTarget = true;
							TargetActorId = TargetId;
							TargetActorClass = TargetActor->GetClass()->GetName();
						}
					}
					else
					{
						Output += FString::Printf(TEXT("  %s (%s): (null)\n"), *KeyName, *KeyTypeName);
					}
				}
				else
				{
					// Non-object key — get as generic string representation
					FString ValueStr = BlackboardComp->DescribeKeyValue(KeyId, EBlackboardDescription::OnlyValue);
					if (ValueStr.Len() > 100)
					{
						ValueStr = ValueStr.Left(100) + TEXT("...");
					}
					Output += FString::Printf(TEXT("  %s (%s): %s\n"), *KeyName, *KeyTypeName, *ValueStr);
				}
			}
		}

		Output += FString::Printf(TEXT("hasTarget: %s\n"), bFoundTarget ? TEXT("true") : TEXT("false"));

		if (bFoundTarget)
		{
			Output += FString::Printf(TEXT("targetActorId: %s\n"), *TargetActorId);
			Output += FString::Printf(TEXT("targetClass: %s\n"), *TargetActorClass);
		}
	}

	// Check for focus actor
	AActor* FocusActor = AIController->GetFocusActor();
	if (FocusActor)
	{
		const FString FocusId = PIEManager.GetActorId(FocusActor);
		Output += FString::Printf(TEXT("focusActor: %s (%s) [actorId: %s]\n"),
			*FocusActor->GetName(), *FocusActor->GetClass()->GetName(), *FocusId);
	}
	else
	{
		Output += TEXT("focusActor: (none)\n");
	}

	return MakeSuccessResult(nullptr, Output);
}
