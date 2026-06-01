// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonAnimEditToolBase.h" // For FToolSchemaBuilder
#include "Animation/BlendProfile.h"

class USkeleton;
class UBlendProfile;
class USkeletalMeshSocket;

/**
 * Base class for all skeleton editing MCP tools. Stateless: each Execute takes
 * the skeleton_path and re-loads the asset. Category auto-derives to "skeleton".
 */
class CLAIREON_API ClaireonSkeletonToolBase : public IClaireonTool
{
public:
	bool RequiresNoPIE() const override { return true; }
	FString GetCategory() const override { return TEXT("skeleton"); }
};

// Macro to reduce declaration boilerplate for individual skeleton tool classes.
#define DECLARE_SKELETON_TOOL(ClassName) \
	class CLAIREON_API ClassName : public ClaireonSkeletonToolBase \
	{ \
	public: \
		FString GetOperation() const override; \
		FString GetDescription() const override; \
		TSharedPtr<FJsonObject> GetInputSchema() const override; \
		FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override; \
	}

/**
 * Shared helpers for skeleton MCP tools: asset loading, formatting, and parsers.
 */
namespace ClaireonSkeletonHelpers
{
	// ========================================================================
	// Asset Loading
	// ========================================================================

	/** Resolve skeleton_path arg and load the USkeleton. Returns nullptr + OutError on failure. */
	CLAIREON_API USkeleton* LoadSkeleton(const FString& AssetPath, FString& OutError);

	// ========================================================================
	// Parsers
	// ========================================================================

	/** Parse "TimeFactor" / "WeightFactor" / "BlendMask" (case-insensitive). */
	CLAIREON_API bool ParseBlendProfileMode(const FString& ModeStr, EBlendProfileMode& OutMode, FString& OutError);

	CLAIREON_API FString BlendProfileModeToString(EBlendProfileMode Mode);

	/** Read FVector from a JSON object with x/y/z number fields. Missing fields default to Default's component. */
	CLAIREON_API bool ReadVectorFromJson(const TSharedPtr<FJsonObject>& Obj, const FVector& Default, FVector& OutVec);

	/** Read FRotator from a JSON object with pitch/yaw/roll number fields. Missing fields default to Default's component. */
	CLAIREON_API bool ReadRotatorFromJson(const TSharedPtr<FJsonObject>& Obj, const FRotator& Default, FRotator& OutRot);

	// ========================================================================
	// Inspection Builders (JSON)
	// ========================================================================

	/** Top-level counts only; cheap. */
	CLAIREON_API TSharedPtr<FJsonObject> BuildOverview(const USkeleton* Skeleton);

	/** All bones with name, index, parent_index, parent_name. */
	CLAIREON_API TSharedPtr<FJsonObject> BuildBones(const USkeleton* Skeleton);

	/** Virtual bones: source/target/virtual names. */
	CLAIREON_API TSharedPtr<FJsonObject> BuildVirtualBones(const USkeleton* Skeleton);

	/** Sockets: name, bone, transform components. */
	CLAIREON_API TSharedPtr<FJsonObject> BuildSockets(const USkeleton* Skeleton);

	/** Animation notify names + curve metadata + asset user data list. */
	CLAIREON_API TSharedPtr<FJsonObject> BuildMetadata(const USkeleton* Skeleton);

	/** Blend profiles or blend masks, depending on bMasksOnly. Includes per-bone entries. */
	CLAIREON_API TSharedPtr<FJsonObject> BuildBlendProfiles(const USkeleton* Skeleton, bool bMasksOnly);

	// ========================================================================
	// Utility
	// ========================================================================

	/** Find a socket by name. Returns nullptr if not found. */
	CLAIREON_API USkeletalMeshSocket* FindSocket(USkeleton* Skeleton, FName SocketName);

	/** Find a blend profile or mask by name. Returns nullptr if not found. */
	CLAIREON_API UBlendProfile* FindBlendProfile(USkeleton* Skeleton, FName ProfileName);
}
