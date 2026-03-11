// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

/** Current state of the flythrough */
enum class EFlythroughState : uint8
{
	Idle,
	Initializing,
	Flying,
	Paused,
	WaitingForEvent,
	Complete,
	Error,
};

/** A single waypoint in the flythrough flight plan */
struct FFlythroughWaypoint
{
	FVector Position = FVector::ZeroVector;
	TOptional<FRotator> Rotation;
	TOptional<FVector> LookAt;
	TOptional<float> Speed;
	TOptional<float> Duration;
	TArray<FString> Events;
	float PauseDuration = 0.0f;
};

/** Configuration for a flythrough session */
struct FFlythroughConfig
{
	float DefaultSpeed = 1000.0f;
	FIntPoint ScreenshotResolution = FIntPoint::ZeroValue;
	FString ScreenshotDirectory;
	bool bAutoGodMode = true;
	float AutoScreenshotInterval = 0.0f;
};

/** Status snapshot returned by GetStatus() */
struct FFlythroughStatusInfo
{
	EFlythroughState State = EFlythroughState::Idle;
	float Progress = 0.0f;
	double ElapsedTime = 0.0;
	double TotalEstimatedTime = 0.0;
	int32 CurrentWaypointIndex = 0;
	int32 TotalWaypoints = 0;
	FVector Position = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	int32 ScreenshotsTaken = 0;
	bool bTraceActive = false;
	FString ScreenshotDirectory;
	FString TraceFilePath;
	FString ErrorMessage;
};

/**
 * Tick-driven manager that drives the debug camera along a spline path.
 * Non-UObject. Uses FTSTicker for per-frame callbacks during PIE.
 * Only one flythrough may be active at a time (singleton).
 */
class FClaireonFlythroughManager
{
public:
	FClaireonFlythroughManager() = default;
	~FClaireonFlythroughManager();

	bool Start(TArray<FFlythroughWaypoint> InWaypoints, FFlythroughConfig InConfig);
	void Stop();
	FFlythroughStatusInfo GetStatus() const;

	static FClaireonFlythroughManager* GetActive();
	static void DestroyActive();
	static void SetActive(TUniquePtr<FClaireonFlythroughManager> InManager);

	float GetTotalSplineLength() const { return TotalSplineLength; }
	double GetTotalEstimatedTime() const { return TotalEstimatedTime; }

private:
	bool Tick(float DeltaTime);

	// Spline evaluation
	FVector EvaluateCatmullRom(int32 SegmentIndex, float T) const;
	FVector EvaluateCatmullRomTangent(int32 SegmentIndex, float T) const;
	void GetSegmentControlPoints(int32 SegmentIndex, FVector& OutP0, FVector& OutP1, FVector& OutP2, FVector& OutP3) const;
	float DistanceToT(int32 SegmentIndex, float DistanceInSegment) const;
	void DistanceToSegmentAndT(float Distance, int32& OutSegment, float& OutT) const;

	// Camera driving
	void UpdateCameraPosition(float Distance);
	void ProcessWaypointEvents(int32 WaypointIndex);
	void TakeScreenshot();
	void DisableDebugCamera();

	static TUniquePtr<FClaireonFlythroughManager> ActiveManager;

	// State
	EFlythroughState State = EFlythroughState::Idle;
	FString ErrorMessage;

	// Flight plan
	TArray<FFlythroughWaypoint> Waypoints;
	FFlythroughConfig Config;

	// Spline data (built from waypoints in Start)
	TArray<FVector> SplinePoints;                  // Waypoint positions (same as Waypoints[i].Position)
	TArray<FQuat> WaypointRotations;               // Resolved rotation at each waypoint
	TArray<bool> WaypointUseTangent;               // True if rotation should follow spline tangent
	TArray<float> CumulativeSegmentDistances;      // CumulativeSegmentDistances[i] = total distance from start to waypoint i+1
	TArray<TArray<float>> SegmentArcLengthSamples; // Per-segment: cumulative chord lengths at 32 t-samples
	TArray<float> SegmentSpeeds;                   // Speed for each segment (cm/s)
	TArray<float> SegmentDurations;                // Duration for each segment (seconds)
	float TotalSplineLength = 0.0f;
	double TotalEstimatedTime = 0.0;

	// Runtime state
	float CurrentDistance = 0.0f;
	int32 CurrentSegmentIndex = 0;
	int32 NextWaypointIndex = 1;
	FVector CurrentPosition = FVector::ZeroVector;
	FRotator CurrentRotation = FRotator::ZeroRotator;
	double StartTime = 0.0;
	double ElapsedTime = 0.0;

	// Pause state
	float PauseTimeRemaining = 0.0f;

	// Event/screenshot state
	int32 EventFrameCounter = 0;
	int32 ScreenshotCounter = 0;
	FString PendingScreenshotPath;
	FString ScreenshotSubDirectory;

	// Trace state
	FString TraceFilePath;
	bool bTraceRecording = false;

	// Auto-screenshot state
	float TimeSinceLastAutoScreenshot = 0.0f;

	// Pending events for current waypoint
	TArray<FString> PendingEvents;
	int32 PendingEventIndex = 0;

	FTSTicker::FDelegateHandle TickerHandle;

	static constexpr int32 ARC_LENGTH_SAMPLES = 32;
};
