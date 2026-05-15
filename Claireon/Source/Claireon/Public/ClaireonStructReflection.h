// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UScriptStruct;
class FProperty;

/**
 * Reflection helpers for inspecting USTRUCTs (native and Blueprint user-defined).
 * Used by claireon.struct_inspect and by chooser / blueprint migration workflows
 * that need to introspect struct shapes programmatically.
 */
namespace ClaireonStructReflection
{
	/**
	 * Resolve a struct path to a UScriptStruct. Accepts:
	 *  - /Script/<Module>.<StructName>         (native C++ UScriptStruct)
	 *  - /Game/<...>/<Asset>.<AssetName>       (user-defined struct asset)
	 *  - /Game/<...>/<Asset>                   (bare BP asset path; asset name is inferred)
	 *  - Bare struct name (e.g. "FFSLocoChooserOutputs") — best-effort fuzzy lookup via FindFirstObject
	 */
	CLAIREON_API UScriptStruct* ResolveStructPath(const FString& Path, FString& OutError);

	/**
	 * Classify a property into a broad kind tag (Bool / Int / Float / Double / String / Name /
	 * Text / Enum / Struct / Object / Class / SoftObject / SoftClass / Array / Set / Map / Unknown).
	 */
	CLAIREON_API FString ClassifyProperty(const FProperty* Property);

	/**
	 * For container / nested properties, return the sub-type path in a form usable by follow-up
	 * calls to ResolveStructPath (for structs), LoadClass (for objects/classes), etc.
	 * Returns empty string for scalar properties.
	 */
	CLAIREON_API FString GetPropertySubtypePath(const FProperty* Property);

	/**
	 * For container properties: element type path (Array/Set) or value type path (Map).
	 * Returns empty string for non-container properties.
	 */
	CLAIREON_API FString GetPropertyInnerTypePath(const FProperty* Property);

	/**
	 * User-facing name for a property: strips the trailing GUID suffix that user-defined
	 * struct properties carry internally (e.g., "BlendInTime_A_31C9...").
	 * For native properties, this is just GetName().
	 */
	CLAIREON_API FString GetFriendlyPropertyName(const FProperty* Property);

	/** Retrieve property default as a string via ExportText_Direct from an initialized temp struct. */
	CLAIREON_API bool GetPropertyDefaultValue(UScriptStruct* OwnerStruct, const FProperty* Property, FString& OutValue);

	/** Decompose a CPF flag bitmask into human-readable flag strings. */
	CLAIREON_API TArray<FString> FormatPropertyFlags(uint64 PropertyFlags);

	/**
	 * Serialize a property's full metadata (kind, cpp type, flags, metadata map, optional default)
	 * to a JSON object. OwnerStruct is only needed if bIncludeDefaults is true.
	 */
	CLAIREON_API TSharedPtr<FJsonObject> SerializeProperty(
		UScriptStruct* OwnerStruct,
		const FProperty* Property,
		bool bIncludeDefaults,
		bool bIncludeMetadata);

	/**
	 * Serialize an entire struct's schema (name, module, kind, fields, super struct, size).
	 * The primary entry point for the struct_inspect tool.
	 */
	CLAIREON_API TSharedPtr<FJsonObject> SerializeStructSchema(
		UScriptStruct* ScriptStruct,
		bool bIncludeDefaults,
		bool bIncludeMetadata);
}
