// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class UBlueprint;

/**
 * Shared helper for Blueprint interface authoring ops.
 *
 * Both Implement and Remove resolve the interface class (via
 * ClaireonNameResolver::ResolveClassName), validate CLASS_Interface, open a
 * scoped transaction, invoke FBlueprintEditorUtils::ImplementNewInterface /
 * RemoveInterface, mark the Blueprint structurally modified, and auto-compile
 * via FKismetEditorUtilities::CompileBlueprint.
 *
 * The AnimGraph interface tools currently do not auto-compile; migrating them
 * onto this helper is a mechanical follow-up.
 */
namespace ClaireonBPInterfaceAuthor
{
	struct FInterfaceOpResult
	{
		/** Whether the operation succeeded. */
		bool bSucceeded = false;

		/** Populated with a human-readable message when bSucceeded is false. */
		FString Error;

		/** Fuzzy-resolution note, if any, from ClaireonNameResolver::ResolveClassName. */
		FString ResolutionNote;

		/** Resolved class short name (e.g. "MyTargetClass"). Empty on failure. */
		FString ResolvedClassName;

		/** Names of Blueprint->ImplementedInterfaces after the op (short names). */
		TArray<FString> PostOpInterfaceNames;
	};

	/** Add an interface to the Blueprint and auto-compile. */
	FInterfaceOpResult Implement(UBlueprint* Blueprint, const FString& InterfaceName);

	/** Remove an interface from the Blueprint and auto-compile. */
	FInterfaceOpResult Remove(UBlueprint* Blueprint, const FString& InterfaceName);
}
