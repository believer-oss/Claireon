// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class AActor;
class UBlueprint;

/**
 * Component-aware property resolution for actors and Blueprint CDOs.
 * Cascades property lookups through actor root -> RootComponent -> all components,
 * returning structured resolution metadata so the caller knows exactly where
 * the property was found.
 *
 * Supports component-name prefix syntax: "StaticMeshComponent0.RelativeLocation"
 */
namespace ClaireonPropertyResolver
{
	struct CLAIREON_API FResolvedProperty
	{
		UObject* TargetObject = nullptr;  // The object the property lives on
		FString  ResolvedOn;              // "Actor", "RootComponent", "CDO", or component name
		FString  QualifiedPath;           // Full qualified path, e.g. "StaticMeshComponent0.RelativeLocation"
		FString  RemainingPath;           // Path to pass to ClaireonPropertyUtils (after consuming component-name prefix)
		FString  Note;                    // Diagnostic note for AI feedback
	};

	/**
	 * Resolve a property name on an actor via cascading search.
	 * Search order: explicit component-name prefix -> Actor root -> RootComponent -> iterate GetComponents().
	 * Supports component-name prefix syntax: "StaticMeshComponent0.RelativeLocation"
	 *
	 * @param Actor - The actor to search on
	 * @param PropertyPath - Property name or dot-path (may include component-name prefix)
	 * @param OutResolved - Populated on success with resolution metadata
	 * @param OutError - Populated on failure with descriptive error
	 * @return true if the property was found
	 */
	CLAIREON_API bool ResolvePropertyOnActor(
		AActor* Actor,
		const FString& PropertyPath,
		FResolvedProperty& OutResolved,
		FString& OutError);

	/**
	 * Read a property value from an actor with component fallback.
	 * Calls ResolvePropertyOnActor, then ClaireonPropertyUtils::ReadPropertyByPath
	 * on OutResolved.TargetObject with OutResolved.RemainingPath.
	 *
	 * @param Actor - The actor to read from
	 * @param PropertyPath - Property name or dot-path
	 * @param OutResolved - Populated with resolution metadata
	 * @param OutError - Populated on failure
	 * @return The property value as a string, or empty string on failure
	 */
	CLAIREON_API FString ReadPropertyOnActor(
		AActor* Actor,
		const FString& PropertyPath,
		FResolvedProperty& OutResolved,
		FString& OutError);

	/**
	 * Write a property value on an actor with component fallback.
	 * Calls ResolvePropertyOnActor, then ClaireonPropertyUtils::WritePropertyByPath
	 * on OutResolved.TargetObject with OutResolved.RemainingPath.
	 * Also calls TargetObject->Modify() when TargetObject != Actor (for undo correctness).
	 * The caller is responsible for string-coercing JSON values before passing
	 * them as the Value parameter.
	 *
	 * @param Actor - The actor to write to
	 * @param PropertyPath - Property name or dot-path
	 * @param Value - String representation of the value to set
	 * @param OutResolved - Populated with resolution metadata
	 * @param OutError - Populated on failure
	 * @return true on success
	 */
	CLAIREON_API bool WritePropertyOnActor(
		AActor* Actor,
		const FString& PropertyPath,
		const FString& Value,
		FResolvedProperty& OutResolved,
		FString& OutError);

	/**
	 * Resolve a property on a Blueprint CDO with SCS component template fallback.
	 * Search order: explicit SCS component-name prefix -> CDO -> SCS component templates
	 * (via SimpleConstructionScript->GetAllNodes()).
	 * Uses USCS_Node::GetVariableName() for component name matching.
	 *
	 * @param Blueprint - The Blueprint to resolve on
	 * @param PropertyPath - Property name or dot-path
	 * @param OutResolved - Populated on success
	 * @param OutError - Populated on failure
	 * @return true if the property was found
	 */
	CLAIREON_API bool ResolvePropertyOnBlueprintCDO(
		UBlueprint* Blueprint,
		const FString& PropertyPath,
		FResolvedProperty& OutResolved,
		FString& OutError);
}
