// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Tools/IClaireonTool.h"

class UMaterialInterface;
class UMeshComponent;
class UBlueprint;
class AActor;
class UWorld;
class FJsonObject;

/**
 * Shared helpers used by the decomposed Claireon material_apply tools
 * (material_apply_to_actor / material_apply_to_blueprint).
 *
 * The dispatcher in ClaireonTool_MaterialApply has been split into per-kind tools per the
 * "one Claireon tool does one verb" invariant; these helpers preserve the common slot-
 * iteration, transaction, and response-building logic across the new tools.
 */
namespace ClaireonMaterialApplyHelpers
{
	/** Find a UMeshComponent on the given actor whose GetName() matches ComponentName. */
	UMeshComponent* FindMeshComponentOnActor(AActor* Actor, const FString& ComponentName);

	/** Find a UMeshComponent SCS template on the given blueprint by SCS node variable name. */
	UMeshComponent* FindMeshComponentOnBlueprint(UBlueprint* Blueprint, const FString& ComponentName);

	/** Linear-search the editor world for an actor by label, name, or path. */
	AActor* FindActorInEditorWorld(UWorld* World, const FString& ActorName);

	/** Load a UMaterialInterface by path; tries LoadObject first, then FSoftObjectPath fallback. */
	UMaterialInterface* LoadMaterialByPath(const FString& MaterialPath, FString& OutError);

	/**
	 * Common body for "apply Material to a MeshComponent" -- transactional, slot-iterating,
	 * response-building. Caller resolves the MeshComponent (one of: live actor or SCS template
	 * on a blueprint) and identifies the target via TargetLabel.
	 *
	 * If Blueprint is non-null, the blueprint is also marked dirty and compiled, and the
	 * response includes compile_ms / compile_status and the child-blueprint enumeration.
	 * If Actor is non-null and Blueprint is null, the actor is marked dirty only.
	 *
	 * ElementIndex = -1 means "all slots".
	 */
	IClaireonTool::FToolResult ApplyMaterialToMeshComponent(
		UMaterialInterface* Material,
		UMeshComponent* MeshComponent,
		int32 ElementIndex,
		const FString& TargetLabel,
		const FString& ComponentName,
		AActor* Actor,
		UBlueprint* Blueprint);
}
