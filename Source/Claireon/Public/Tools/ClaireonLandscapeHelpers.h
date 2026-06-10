// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UWorld;
class ULandscapeInfo;
class ALandscapeProxy;
class AInstancedFoliageActor;

/**
 * Brush mode for landscape sculpting operations.
 */
enum class EClaireonBrushMode : uint8
{
	Raise,
	Lower,
	Smooth,
	Flatten,
	Erode
};

/**
 * Helper functions for Landscape and Foliage MCP tools.
 * Provides landscape lookup, foliage actor access, data conversion, and brush math.
 */
namespace ClaireonLandscapeHelpers
{
	/**
	 * Find all landscapes in the world, optionally filtered by name substring (case-insensitive).
	 * Returns pairs of (ULandscapeInfo*, ALandscapeProxy*).
	 */
	TArray<TPair<ULandscapeInfo*, ALandscapeProxy*>> FindLandscapeInWorld(UWorld* World, const FString& NameFilter);

	/**
	 * Get or create the AInstancedFoliageActor for the current level.
	 * Sets OutError and returns nullptr on failure.
	 */
	AInstancedFoliageActor* GetOrCreateFoliageActor(UWorld* World, FString& OutError);

	/**
	 * Build a JSON object describing landscape metadata.
	 * DetailLevel: "summary" or "full". Full includes material, weight layers, spline info.
	 */
	TSharedPtr<FJsonObject> BuildLandscapeInfoJson(ULandscapeInfo* LandscapeInfo, const FString& DetailLevel);

	/**
	 * Apply a brush kernel to a uint16 height data buffer.
	 * CenterX/CenterY are relative to the data buffer origin.
	 */
	void ApplyBrushKernel(
		TArray<uint16>& HeightData,
		int32 DataWidth,
		int32 DataHeight,
		int32 CenterX,
		int32 CenterY,
		float Radius,
		float Strength,
		EClaireonBrushMode Mode,
		float TargetHeight = 0.0f
	);

	/** Convert a world-space height to landscape uint16 representation. */
	uint16 HeightWorldToUint16(float WorldHeight);

	/** Convert a landscape uint16 height to world-space representation. */
	float HeightUint16ToWorld(uint16 RawHeight);
}
