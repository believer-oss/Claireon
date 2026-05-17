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
	 * Serialize a single FProperty's value to a JSON value, with depth-limited
	 * recursion for nested structs and instanced sub-objects. Object properties
	 * are emitted as GetPathName() unless the property is instanced and Depth
	 * permits expansion. Soft-object / soft-class properties are emitted via
	 * ExportText_Direct (quoted path form).
	 *
	 * Used by ClaireonTool_UObjectInspect and ClaireonTool_StructInspect.
	 *
	 * @param Property      The FProperty to serialize. Must not be null.
	 * @param ValuePtr      Pointer to the value memory (obtained via
	 *                      ContainerPtrToValuePtr or equivalent). Must not be null.
	 * @param OwnerObject   The UObject that owns ValuePtr, used for resolving
	 *                      instanced sub-object references. May be null for
	 *                      non-instanced contexts (e.g. struct-only inspection).
	 * @param Depth         Remaining recursion depth. When zero, nested structs
	 *                      and instanced object expansion stop and the value is
	 *                      summarized as a path/string.
	 * @return              A non-null TSharedPtr<FJsonValue>. On serialization
	 *                      failure, returns a string FJsonValue with an error
	 *                      description rather than nullptr.
	 */
	CLAIREON_API TSharedPtr<FJsonValue> PropertyToJsonValue(
		FProperty* Property,
		const void* ValuePtr,
		UObject* OwnerObject,
		int32 Depth);

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

	/**
	 * Resolve a dotted property path on a UObject down to the leaf FProperty plus the
	 * container pointer that property lives on. The caller can then use
	 * FProperty::ContainerPtrToValuePtr<T>(OutContainer) to access or write the value.
	 *
	 * Path syntax: same as ReadPropertyByPath / WritePropertyByPath
	 * (e.g. "PropertyName", "Struct.Member", "Array[0].Member").
	 *
	 * @param Object       - Root UObject to start path resolution from
	 * @param PropertyPath - Dotted (possibly indexed) property path
	 * @param OutContainer - On success, the container pointer the leaf property lives on
	 *                       (may be the Object itself, a struct interior, or an inner
	 *                       sub-object). Set to nullptr on failure.
	 * @param OutError     - Populated on failure with a diagnostic
	 * @return The leaf FProperty, or nullptr on failure
	 */
	CLAIREON_API FProperty* ResolvePropertyByPath(
		UObject*       Object,
		const FString& PropertyPath,
		void*&         OutContainer,
		FString&       OutError);

	/**
	 * Append a new RF_Transactional inline (EditInline / Instanced) sub-object to a
	 * TArray<UObject*> property reached by ArrayPath, including paths that traverse
	 * nested USTRUCT array elements (e.g. "Operations[0].TargetGameplayActions").
	 *
	 * Verifies that the array's inner property is FObjectProperty AND has
	 * CPF_InstancedReference. SubObjectClass must be a subclass of the inner type.
	 * The Outer of the new UObject is the supplied Outer (caller-owned). The new
	 * object is created with RF_Transactional so editor undo of the array mutation
	 * properly tracks it.
	 *
	 * Caller responsibilities (this helper does NOT do these):
	 *   - Open an FScopedTransaction
	 *   - Call Outer->Modify() before invoking this helper
	 *
	 * @param Outer          - UObject that owns the array (used as Outer for NewObject)
	 * @param SubObjectClass - Concrete UClass to instantiate
	 * @param ArrayPath      - Dotted path to the TArray<UObject*> property
	 * @param OutError       - Populated on failure
	 * @return The newly created sub-object, or nullptr on failure
	 */
	CLAIREON_API UObject* CreateInstancedArrayElement(
		UObject*       Outer,
		UClass*        SubObjectClass,
		const FString& ArrayPath,
		FString&       OutError);

	/**
	 * Replace the value of a singular UPROPERTY(EditAnywhere, Instanced) UObject*
	 * slot with a fresh RF_Transactional inline sub-object. If the slot already
	 * contained a value the previous object is MarkAsGarbage()'d.
	 *
	 * Verifies that the leaf property is FObjectProperty AND has
	 * CPF_InstancedReference. SubObjectClass must be a subclass of the slot type.
	 *
	 * Caller responsibilities (this helper does NOT do these):
	 *   - Open an FScopedTransaction
	 *   - Call Outer->Modify() before invoking this helper
	 *
	 * @param Outer          - UObject that owns the slot (used as Outer for NewObject)
	 * @param SubObjectClass - Concrete UClass to instantiate
	 * @param ObjectPath     - Dotted path to the UObject* slot
	 * @param OutError       - Populated on failure
	 * @return The newly created sub-object, or nullptr on failure
	 */
	CLAIREON_API UObject* SetInstancedSubObject(
		UObject*       Outer,
		UClass*        SubObjectClass,
		const FString& ObjectPath,
		FString&       OutError);
}
