// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Generic UPROPERTY read/write utilities using Unreal reflection.
 * Supports dot-path navigation through structs and array index access.
 * No game-specific dependencies — works on any UObject.
 *
 * Path syntax: "PropertyName", "Struct.Member", "Array[0].Member", "Array[0].Nested[1].Value"
 */
namespace ClaireonPropertyUtils
{
	/**
	 * Read a UPROPERTY value by dot-path with optional array index support.
	 * Returns the value as a string via ExportText_Direct.
	 * @param Object - The UObject to read from
	 * @param PropertyPath - Dot-separated path, with optional [N] array indices
	 * @param OutError - Populated on failure
	 * @return The property value as a string, or empty string on failure
	 */
	CLAIREON_API FString ReadPropertyByPath(UObject* Object, const FString& PropertyPath, FString& OutError);

	/**
	 * Write a UPROPERTY value by dot-path with optional array index support.
	 * Uses ImportText_Direct to deserialize the string value.
	 * @param Object - The UObject to write to
	 * @param PropertyPath - Dot-separated path, with optional [N] array indices
	 * @param Value - String representation of the value to set
	 * @param OutError - Populated on failure
	 * @return true on success
	 */
	CLAIREON_API bool WritePropertyByPath(UObject* Object, const FString& PropertyPath, const FString& Value, FString& OutError);

	/**
	 * Enumerate all UPROPERTY values on a UObject as a JSON object.
	 * Skips transient and delegate properties.
	 * @param Object - The UObject to enumerate
	 * @param Filter - Optional substring filter on property names (empty = all)
	 * @param Depth - Recursion depth for sub-objects and structs (0 = top-level only)
	 * @return JSON object with property names as keys
	 */
	CLAIREON_API TSharedPtr<FJsonObject> GetAllProperties(UObject* Object, const FString& Filter = TEXT(""), int32 Depth = 2);

	/**
	 * Add an element to a TArray UPROPERTY.
	 * @param Object - The UObject containing the array
	 * @param ArrayPath - Dot-path to the array property
	 * @param Value - String representation of the element to add
	 * @param OutError - Populated on failure
	 * @return true on success
	 */
	CLAIREON_API bool AddArrayElement(UObject* Object, const FString& ArrayPath, const FString& Value, FString& OutError);

	/**
	 * Remove an element from a TArray UPROPERTY by index.
	 * @param Object - The UObject containing the array
	 * @param ArrayPath - Dot-path to the array property
	 * @param Index - Element index to remove
	 * @param OutError - Populated on failure
	 * @return true on success
	 */
	CLAIREON_API bool RemoveArrayElement(UObject* Object, const FString& ArrayPath, int32 Index, FString& OutError);

	/**
	 * Create an instanced sub-object (EditInlineNew) and add it to a TArray property.
	 * @param Outer - The UObject that owns the array (used as Outer for NewObject)
	 * @param SubObjectClass - The class to instantiate
	 * @param ArrayPath - Dot-path to the TArray<UObject*> property
	 * @param OutError - Populated on failure
	 * @return The created sub-object, or nullptr on failure
	 */
	CLAIREON_API UObject* CreateInstancedSubObject(UObject* Outer, UClass* SubObjectClass, const FString& ArrayPath, FString& OutError);
}
