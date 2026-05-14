// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class UCameraAsset;
class UCameraRigAsset;
class UCameraNode;
class UClass;
class FJsonObject;
namespace UE::Cameras
{
	class FCameraBuildLog;
}

namespace ClaireonCameraAssetHelpers
{
	/** Resolve a node-id path ("Root", "Root.Children[2]", etc.) to a UCameraNode within a rig. */
	CLAIREON_API UCameraNode* ResolveNode(UCameraRigAsset* Rig, const FString& NodeId, FString& OutError);

	/** Lookup a UCameraNode subclass by short name, with optional 'U' prefix forgiveness. */
	CLAIREON_API UClass* ResolveNodeClass(const FString& Name);

	/** Enumerate concrete UCameraNode subclasses (filters CLASS_Abstract / CLASS_Deprecated). */
	CLAIREON_API TArray<UClass*> EnumerateCameraNodeClasses();

	/** Marshal an FCameraBuildLog into [{severity, text, object?}] JSON. */
	CLAIREON_API TSharedPtr<FJsonObject> BuildLogToJson(const UE::Cameras::FCameraBuildLog& Log);

	/** Common close-toolkit guard called at the start of mutating tools. */
	CLAIREON_API void CloseEditorToolkitForAsset(UCameraAsset* Asset);

	/**
	 * Extract the trailing `[N]` index from a node-id path. Returns -1 if the path
	 * does not end in a bracketed index (e.g. "Root", "Root.SomeMember").
	 * Examples:
	 *   "Root.Children[2]"             -> 2
	 *   "Root.Children[0].Children[7]" -> 7
	 *   "Root"                         -> -1
	 *   ""                             -> -1
	 */
	CLAIREON_API int32 ParseTrailingIndex(const FString& NodeId);

	/**
	 * Compute the parent node-id by stripping the last `.<segment>` (and any trailing
	 * `[N]`). Returns an empty string when called on the root node-id ("Root") since
	 * the rig is not itself a node.
	 * Examples:
	 *   "Root.Children[2]"             -> "Root"
	 *   "Root.Children[0].Children[7]" -> "Root.Children[0]"
	 *   "Root.DrivingBlend"            -> "Root"
	 *   "Root"                         -> ""
	 */
	CLAIREON_API FString GetParentPath(const FString& NodeId);

	/** Compose `<ParentId>.Children[<ChildIndex>]`. */
	CLAIREON_API FString ComputeNodeIdForChildOf(const FString& ParentId, int32 ChildIndex);
} // namespace ClaireonCameraAssetHelpers
