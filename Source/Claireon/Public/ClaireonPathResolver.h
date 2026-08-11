// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once
#include "CoreMinimal.h"

namespace ClaireonPathResolver
{
	/** Classification of what type of Unreal path was resolved. */
	enum class EPathKind : uint8
	{
		/** Standard asset package path (e.g. /Game/Characters/BP_Player) */
		PackagePath,
		/** Native class reference (e.g. /Script/Engine.Actor) */
		NativeClassPath,
	};

	struct FResolvedPath
	{
		// The normalized canonical path. For PackagePath kind, this has the form
		// "/Game/Foo/Bar.Bar" (object-path canonical) after normalization. For NativeClassPath
		// kind, this is the "/Script/Module.ClassName" form unchanged.
		FString Path;

		// The package-prefix form of the resolved path with no trailing .<ObjectName>.
		// Populated by Resolve() alongside Path. Folder-path callers (callers that use
		// the path with IAssetRegistry::GetAssetsByPath / FARFilter::PackagePaths) must
		// read this field instead of Path. Object-path callers should continue to read
		// Path. For NativeClassPath kind, PackagePath equals Path.
		FString PackagePath;

		// Classification of what type of path was resolved
		EPathKind Kind = EPathKind::PackagePath;

		// If the input had a _C suffix that was stripped, this is true (only relevant for PackagePath kind)
		bool bIsClassReference = false;

		// The original input for error reporting
		FString OriginalInput;

		// Human-readable description of which normalization steps were applied (for debugging)
		FString NormalizationTrace;
	};

	struct FResolveResult
	{
		// Whether resolution succeeded
		bool bSuccess = false;

		// The resolved path (only valid if bSuccess is true)
		FResolvedPath ResolvedPath;

		// Error message if resolution failed (empty on success)
		FString Error;
	};

	/**
	 * Resolve any user-provided path into a canonical Unreal path.
	 * Returns a result struct containing success/failure, the resolved path, and any error.
	 *
	 * Threading: Must be called on the game thread if _C disambiguation via
	 * AssetRegistry lookup is needed. Off-thread calls will do best-effort
	 * _C stripping without registry validation.
	 */
	CLAIREON_API FResolveResult Resolve(const FString& InPath);

	/**
	 * Resolve a user-provided path all the way to a live UObject.
	 *
	 * Runs Resolve() first, then materializes the object per path kind:
	 *  - NativeClassPath (/Script/Module.ClassName) -> that class's CDO.
	 *  - Sub-object / world-actor paths (containing ':', including live PIE
	 *    actor paths) -> StaticFindObject, then FindFirstObjectSafe.
	 *  - Asset paths -> StaticFindObject, then StaticLoadObject when bAllowLoad.
	 *
	 * Falls back to looking the RAW input up verbatim if all of that misses. Resolve()
	 * normalizes for asset paths, which can mangle an already-precise object path --
	 * a CDO sub-object, or an object in a world whose package name contains dots.
	 * Those are what GetPathName() returns, so they have to resolve.
	 *
	 * Shared by uobject_inspect, uobject_set_property, and component_reregister so the
	 * read, write, and repair surfaces reach exactly the same set of objects.
	 *
	 * @param InPath     - Asset path, native class path, sub-object path, or rooted
	 *                     in-memory path (/Memory/..., /Temp/...)
	 * @param bAllowLoad - When true, fall back to StaticLoadObject for asset paths
	 *                     not already in memory. When false, an unloaded asset is an error.
	 * @param OutError   - Populated on failure
	 * @param OutCoercionNote - Optional. Populated when the named object was a class
	 *                     or Blueprint asset and the result was coerced to its class
	 *                     default object, so a caller can disclose the substitution
	 *                     rather than silently reporting properties of a different
	 *                     object than the one the path named.
	 * @return The resolved UObject, or nullptr on failure
	 */
	CLAIREON_API UObject* ResolveObjectFromPath(const FString& InPath, bool bAllowLoad, FString& OutError,
		FString* OutCoercionNote = nullptr);
}
