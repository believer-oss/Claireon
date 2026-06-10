// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialTool_ApplyToActor.h"
#include "Tools/ClaireonMaterialApplyHelpers.h"
#include "ClaireonScopedAssetLock.h"

#include "Components/MeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "UObject/Package.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialTool_ApplyToActor::GetCategory() const { return TEXT("material"); }
FString ClaireonMaterialTool_ApplyToActor::GetOperation() const { return TEXT("apply_to_actor"); }

FString ClaireonMaterialTool_ApplyToActor::GetDescription() const
{
	return TEXT("Apply a UMaterial or UMaterialInstanceConstant to a UMeshComponent on a live actor in "
	            "the editor world. element_index = -1 applies to every slot. Stateless / non-session; "
	            "wrapped in FScopedTransaction so editor undo works.");
}

TSharedPtr<FJsonObject> ClaireonMaterialTool_ApplyToActor::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> MaterialPathProp = MakeShared<FJsonObject>();
	MaterialPathProp->SetStringField(TEXT("type"), TEXT("string"));
	MaterialPathProp->SetStringField(TEXT("description"),
		TEXT("Asset path of the UMaterialInterface to apply (UMaterial or UMaterialInstanceConstant)."));
	Properties->SetObjectField(TEXT("material_path"), MaterialPathProp);

	TSharedPtr<FJsonObject> ActorProp = MakeShared<FJsonObject>();
	ActorProp->SetStringField(TEXT("type"), TEXT("string"));
	ActorProp->SetStringField(TEXT("description"), TEXT("Target actor label, name, or path in the editor world."));
	Properties->SetObjectField(TEXT("actor_name"), ActorProp);

	TSharedPtr<FJsonObject> CompProp = MakeShared<FJsonObject>();
	CompProp->SetStringField(TEXT("type"), TEXT("string"));
	CompProp->SetStringField(TEXT("description"), TEXT("Name of the UMeshComponent on the actor."));
	Properties->SetObjectField(TEXT("component_name"), CompProp);

	TSharedPtr<FJsonObject> IndexProp = MakeShared<FJsonObject>();
	IndexProp->SetStringField(TEXT("type"), TEXT("integer"));
	IndexProp->SetStringField(TEXT("description"), TEXT("Material slot index; -1 means all slots."));
	Properties->SetObjectField(TEXT("element_index"), IndexProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("material_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("actor_name")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("component_name")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonMaterialTool_ApplyToActor::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Missing arguments"));
	}

	FString MaterialPath;
	if (!Arguments->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: material_path"));
	}
	FString ActorName;
	if (!Arguments->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: actor_name"));
	}
	FString ComponentName;
	if (!Arguments->TryGetStringField(TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: component_name"));
	}

	int32 ElementIndex = 0;
	{
		double IndexDouble = 0.0;
		Arguments->TryGetNumberField(TEXT("element_index"), IndexDouble);
		ElementIndex = static_cast<int32>(IndexDouble);
	}

	FString LoadError;
	UMaterialInterface* Material = ClaireonMaterialApplyHelpers::LoadMaterialByPath(MaterialPath, LoadError);
	if (!Material) return MakeErrorResult(LoadError);

	if (!GEditor)
	{
		return MakeErrorResult(TEXT("GEditor is unavailable"));
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return MakeErrorResult(TEXT("No editor world available"));
	}

	const FString LockPath = World->GetOutermost()->GetName();
	FClaireonScopedAssetLock Lock(LockPath, GetName());
	if (!Lock.IsAcquired())
	{
		return Lock.GetError();
	}

	AActor* Actor = ClaireonMaterialApplyHelpers::FindActorInEditorWorld(World, ActorName);
	if (!Actor)
	{
		return MakeErrorResult(FString::Printf(TEXT("Actor not found in editor world: '%s'"), *ActorName));
	}
	UMeshComponent* MeshComponent = ClaireonMaterialApplyHelpers::FindMeshComponentOnActor(Actor, ComponentName);
	if (!MeshComponent)
	{
		return MakeErrorResult(FString::Printf(TEXT("UMeshComponent '%s' not found on actor '%s'"),
			*ComponentName, *Actor->GetActorLabel()));
	}

	const FString TargetLabel = FString::Printf(TEXT("actor %s"), *Actor->GetActorLabel());
	return ClaireonMaterialApplyHelpers::ApplyMaterialToMeshComponent(
		Material, MeshComponent, ElementIndex, TargetLabel, ComponentName, Actor, /*Blueprint=*/nullptr);
}
