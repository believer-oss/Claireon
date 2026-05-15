// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

// Internal helpers shared across the decomposed ClaireonWidgetBPTool_*.cpp
// translation units. Previously these lived as file-local (static) helpers
// inside ClaireonWidgetBPEditToolBase.cpp. Stage 024 extracted each per-operation
// body into its own decomposed cpp; to keep those extracted bodies compiling
// without duplication, the MVVM helpers were promoted to this internal header
// and a single companion translation unit (ClaireonWidgetBPEditToolBase_Internal.cpp).
//
// Not exported via CLAIREON_API -- intra-module only.

#include "CoreMinimal.h"
#include "Types/MVVMBindingMode.h"
#include "MVVMBlueprintViewModelContext.h"

class UClass;
class UFunction;
class UWidgetBlueprint;
struct FMVVMBlueprintPropertyPath;

namespace ClaireonWidgetBPInternal
{
	/** Parse a string into EMVVMBindingMode. Returns false if unrecognized. */
	bool ParseBindingMode(const FString& ModeStr, EMVVMBindingMode& OutMode);

	/** Parse a string into EMVVMBlueprintViewModelContextCreationType. Returns false if unrecognized. */
	bool ParseCreationType(const FString& TypeStr, EMVVMBlueprintViewModelContextCreationType& OutType);

	/**
	 * Resolve a property path string (dot-separated) against a starting UClass.
	 * Sets up the FMVVMBlueprintPropertyPath fields using SetPropertyPath/AppendPropertyPath.
	 * Returns false and sets OutError on failure.
	 */
	bool ResolvePropertyPath(
		UWidgetBlueprint* WBP,
		FMVVMBlueprintPropertyPath& Path,
		UClass* StartClass,
		const FString& PropertyPathStr,
		FString& OutError);

	/**
	 * Resolve a conversion function name to a UFunction*.
	 * Tries: full path, Class::Function, self-context.
	 */
	const UFunction* ResolveConversionFunction(
		UWidgetBlueprint* WBP,
		const FString& FunctionNameStr,
		FString& OutError);
}
