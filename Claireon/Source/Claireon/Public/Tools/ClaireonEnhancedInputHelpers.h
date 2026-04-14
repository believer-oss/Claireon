// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class UInputAction;
class UInputMappingContext;
class UInputModifier;
class UInputTrigger;
struct FEnhancedActionKeyMapping;
struct FKey;
enum class EInputActionValueType : uint8;

/**
 * Shared utility functions for Enhanced Input MCP tools.
 * Provides asset loading, formatting, class resolution, and property manipulation.
 */
namespace ClaireonEnhancedInputHelpers
{
	/** Load and validate an asset as either UInputAction or UInputMappingContext. */
	UObject* LoadInputAsset(const FString& AssetPath, FString& OutError);

	/** Format a UInputAction as structured text. */
	FString FormatInputAction(const UInputAction* Action, bool bSummaryOnly);

	/** Format a UInputMappingContext as structured text. */
	FString FormatMappingContext(const UInputMappingContext* IMC, bool bSummaryOnly);

	/** Format a single FEnhancedActionKeyMapping. */
	FString FormatMapping(const FEnhancedActionKeyMapping& Mapping, int32 Index, bool bSummaryOnly);

	/** Format a trigger array. */
	FString FormatTriggers(const TArray<TObjectPtr<UInputTrigger>>& Triggers, bool bSummaryOnly, const FString& Indent);

	/** Format a modifier array. */
	FString FormatModifiers(const TArray<TObjectPtr<UInputModifier>>& Modifiers, bool bSummaryOnly, const FString& Indent);

	/** Format all UPROPERTY values on an object (for trigger/modifier property display). */
	FString FormatObjectProperties(const UObject* Object, const FString& Indent);

	/** Resolve a modifier class name (short or full) to a UClass*. */
	UClass* ResolveModifierClass(const FString& ClassName, FString& OutError);

	/** Resolve a trigger class name (short or full) to a UClass*. */
	UClass* ResolveTriggerClass(const FString& ClassName, FString& OutError);

	/** Create a new instanced UInputModifier subclass object. */
	UInputModifier* CreateModifier(UObject* Outer, UClass* ModifierClass);

	/** Create a new instanced UInputTrigger subclass object. */
	UInputTrigger* CreateTrigger(UObject* Outer, UClass* TriggerClass);

	/** Set a property on a UObject by name/value string using reflection. */
	bool SetObjectProperty(UObject* Object, const FString& PropertyName,
						  const FString& PropertyValue, FString& OutError);

	/** Get mutable reference to IMC Mappings array (protected, via reflection). */
	TArray<FEnhancedActionKeyMapping>& GetMappingsMutable(UInputMappingContext* IMC);

	/** Notify the Enhanced Input system that an IMC was modified. */
	void NotifyMappingContextModified(UInputMappingContext* IMC);

	/** Resolve a key name string to an FKey. Validates via EKeys::GetKeyDetails(). */
	bool ResolveKey(const FString& KeyName, FKey& OutKey, FString& OutError);

	/** Convert EInputActionValueType to a human-readable string. */
	FString ValueTypeToString(EInputActionValueType ValueType);

	/** Parse a value type string ("bool", "float", "2d", "3d") to enum. */
	bool ParseValueType(const FString& TypeStr, EInputActionValueType& OutType, FString& OutError);
}
