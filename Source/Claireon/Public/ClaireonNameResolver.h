// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once
#include "CoreMinimal.h"

class UClass;
class UScriptStruct;
class UEnum;
class UFunction;
class UStruct;
class UEdGraphPin;
class UEdGraphNode;
enum EEdGraphPinDirection : int;
class FProperty;

namespace ClaireonNameResolver
{

	struct FNameResolveResult
	{
		// Whether resolution succeeded (exactly one match found)
		bool bSuccess = false;

		// The canonical resolved name/object
		FString ResolvedName;

		// Human-readable note explaining what normalization was applied.
		// Empty if the input was an exact match. Included in tool responses
		// so the AI learns the correct name for future calls.
		FString ResolutionNote;

		// Error message if resolution failed
		FString Error;

		// If multiple fuzzy matches were found, listed here for the error message
		TArray<FString> Candidates;
	};

	/**
	 * Resolve a class name using fuzzy matching.
	 * Tries exact match, prefix/suffix variations, module paths, and case-insensitive search.
	 *
	 * @param Input             The class name to resolve
	 * @param RequiredBaseClass If non-null, only classes derived from this class are accepted
	 * @param OutResult         Populated with resolution details
	 * @return                  The resolved UClass, or nullptr on failure
	 */
	UClass* ResolveClassName(
		const FString& Input,
		UClass* RequiredBaseClass,
		FNameResolveResult& OutResult);

	/**
	 * Resolve a pin name on a graph node using fuzzy matching.
	 * Tries exact match, case-insensitive, common aliases, substring, and direction prefix stripping.
	 *
	 * @param Node           The node to search for pins on (must be non-null)
	 * @param Input          The pin name to resolve
	 * @param DirectionHint  Pin direction filter (EGPD_MAX = any direction)
	 * @param OutResult      Populated with resolution details
	 * @return               The resolved UEdGraphPin, or nullptr on failure
	 */
	UEdGraphPin* ResolvePinName(
		UEdGraphNode* Node,
		const FString& Input,
		EEdGraphPinDirection DirectionHint,
		FNameResolveResult& OutResult);

	/**
	 * Resolve a function name on a class using fuzzy matching.
	 * Tries exact match, event aliases, K2_ prefix handling, and case-insensitive search.
	 *
	 * @param OwnerClass  The class to search for the function on (must be non-null)
	 * @param Input       The function name to resolve
	 * @param OutResult   Populated with resolution details
	 * @return            The resolved UFunction, or nullptr on failure
	 */
	UFunction* ResolveFunctionName(
		UClass* OwnerClass,
		const FString& Input,
		FNameResolveResult& OutResult);

	/**
	 * Resolve a property name on a struct using fuzzy matching.
	 * Tries exact match, case-insensitive, boolean prefix handling, and suffix stripping.
	 *
	 * @param Struct    The struct to search for the property on (must be non-null)
	 * @param Input     The property name to resolve
	 * @param OutResult Populated with resolution details
	 * @return          The resolved FProperty, or nullptr on failure
	 */
	FProperty* ResolvePropertyName(
		UStruct* Struct,
		const FString& Input,
		FNameResolveResult& OutResult);

	/**
	 * Resolve a struct name using fuzzy matching.
	 * Tries exact match, F prefix handling, module paths, and case-insensitive search.
	 *
	 * @param Input     The struct name to resolve
	 * @param OutResult Populated with resolution details
	 * @return          The resolved UScriptStruct, or nullptr on failure
	 */
	UScriptStruct* ResolveStructName(
		const FString& Input,
		FNameResolveResult& OutResult);

	/**
	 * Resolve an enum name using fuzzy matching.
	 * Tries exact match, E prefix handling, module paths, and case-insensitive search.
	 *
	 * @param Input     The enum name to resolve
	 * @param OutResult Populated with resolution details
	 * @return          The resolved UEnum, or nullptr on failure
	 */
	UEnum* ResolveEnumName(
		const FString& Input,
		FNameResolveResult& OutResult);

} // namespace ClaireonNameResolver
