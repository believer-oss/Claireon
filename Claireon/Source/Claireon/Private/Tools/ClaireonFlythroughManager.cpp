// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonFlythroughManager.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Editor.h"
#include "UnrealClient.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/CheatManager.h"
#include "GameFramework/PlayerState.h"

TUniquePtr<FClaireonFlythroughManager> FClaireonFlythroughManager::ActiveManager;

FClaireonFlythroughManager::~FClaireonFlythroughManager()
{
	Stop();
}

// ------------------------------------------------------------------
// Spline helpers
// ------------------------------------------------------------------

void FClaireonFlythroughManager::GetSegmentControlPoints(int32 SegmentIndex, FVector& OutP0, FVector& OutP1, FVector& OutP2, FVector& OutP3) const
{
	const int32 Num = SplinePoints.Num();
	OutP1 = SplinePoints[SegmentIndex];
	OutP2 = SplinePoints[SegmentIndex + 1];

	// Phantom endpoints for first/last segments
	if (SegmentIndex == 0)
	{
		OutP0 = OutP1 + (OutP1 - OutP2); // mirror of P2 through P1
	}
	else
	{
		OutP0 = SplinePoints[SegmentIndex - 1];
	}

	if (SegmentIndex + 2 >= Num)
	{
		OutP3 = OutP2 + (OutP2 - OutP1); // mirror of P1 through P2
	}
	else
	{
		OutP3 = SplinePoints[SegmentIndex + 2];
	}
}

FVector FClaireonFlythroughManager::EvaluateCatmullRom(int32 SegmentIndex, float T) const
{
	FVector P0, P1, P2, P3;
	GetSegmentControlPoints(SegmentIndex, P0, P1, P2, P3);

	const float T2 = T * T;
	const float T3 = T2 * T;
	return 0.5f * ((2.0f * P1)
		+ (-P0 + P2) * T
		+ (2.0f * P0 - 5.0f * P1 + 4.0f * P2 - P3) * T2
		+ (-P0 + 3.0f * P1 - 3.0f * P2 + P3) * T3);
}

FVector FClaireonFlythroughManager::EvaluateCatmullRomTangent(int32 SegmentIndex, float T) const
{
	FVector P0, P1, P2, P3;
	GetSegmentControlPoints(SegmentIndex, P0, P1, P2, P3);

	const float T2 = T * T;
	return 0.5f * ((-P0 + P2)
		+ (4.0f * P0 - 10.0f * P1 + 8.0f * P2 - 2.0f * P3) * T
		+ (-3.0f * P0 + 9.0f * P1 - 9.0f * P2 + 3.0f * P3) * T2);
}

float FClaireonFlythroughManager::DistanceToT(int32 SegmentIndex, float DistanceInSegment) const
{
	if (!SegmentArcLengthSamples.IsValidIndex(SegmentIndex))
	{
		return 0.0f;
	}

	const TArray<float>& Samples = SegmentArcLengthSamples[SegmentIndex];
	if (Samples.Num() == 0)
	{
		return 0.0f;
	}

	const float SegmentLength = Samples.Last();
	if (SegmentLength <= UE_KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}

	const float ClampedDist = FMath::Clamp(DistanceInSegment, 0.0f, SegmentLength);

	// Binary search for the sample interval containing our distance
	int32 Low = 0;
	int32 High = Samples.Num() - 1;
	while (Low < High)
	{
		const int32 Mid = (Low + High) / 2;
		if (Samples[Mid] < ClampedDist)
		{
			Low = Mid + 1;
		}
		else
		{
			High = Mid;
		}
	}

	// Interpolate within the sample interval
	if (Low == 0)
	{
		if (Samples[0] <= UE_KINDA_SMALL_NUMBER)
		{
			return 0.0f;
		}
		return (ClampedDist / Samples[0]) * (1.0f / ARC_LENGTH_SAMPLES);
	}

	const float D0 = Samples[Low - 1];
	const float D1 = Samples[Low];
	const float Frac = (D1 - D0 > UE_KINDA_SMALL_NUMBER)
		? (ClampedDist - D0) / (D1 - D0)
		: 0.0f;

	return (static_cast<float>(Low - 1) + Frac) * (1.0f / ARC_LENGTH_SAMPLES);
}

void FClaireonFlythroughManager::DistanceToSegmentAndT(float Distance, int32& OutSegment, float& OutT) const
{
	const float ClampedDist = FMath::Clamp(Distance, 0.0f, TotalSplineLength);

	// Find segment
	OutSegment = 0;
	for (int32 i = 0; i < CumulativeSegmentDistances.Num(); ++i)
	{
		if (ClampedDist <= CumulativeSegmentDistances[i])
		{
			OutSegment = i;
			break;
		}
		OutSegment = i;
	}

	// Distance within this segment
	const float SegmentStartDist = (OutSegment > 0) ? CumulativeSegmentDistances[OutSegment - 1] : 0.0f;
	const float DistInSegment = ClampedDist - SegmentStartDist;
	OutT = DistanceToT(OutSegment, DistInSegment);
}

// ------------------------------------------------------------------
// PIE world helpers
// ------------------------------------------------------------------

static UWorld* FindPIEWorldWithLocalPlayer()
{
	for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
	{
		if (WorldContext.WorldType == EWorldType::PIE && WorldContext.World())
		{
			for (FConstPlayerControllerIterator It = WorldContext.World()->GetPlayerControllerIterator(); It; ++It)
			{
				if (It->Get() && It->Get()->IsLocalController())
				{
					return WorldContext.World();
				}
			}
		}
	}
	return nullptr;
}

/** Find the server PIE world (DedicatedServer or ListenServer net mode). */
static UWorld* FindPIEServerWorld()
{
	for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
	{
		if (WorldContext.WorldType == EWorldType::PIE && WorldContext.World())
		{
			const ENetMode NetMode = WorldContext.World()->GetNetMode();
			if (NetMode == NM_DedicatedServer || NetMode == NM_ListenServer)
			{
				return WorldContext.World();
			}
		}
	}
	return nullptr;
}

/**
 * Disable the movement component tick on a pawn so it doesn't fight flythrough teleportation.
 * Clears accumulated forces and stops all movement. Works on both client and server pawns.
 */
static void DisablePawnMovement(APawn* Pawn)
{
	if (!Pawn)
	{
		return;
	}
	if (UPawnMovementComponent* MoveComp = Pawn->GetMovementComponent())
	{
		MoveComp->StopMovementImmediately();

		if (UCharacterMovementComponent* CMC = Cast<UCharacterMovementComponent>(MoveComp))
		{
			CMC->ClearAccumulatedForces();

			// Flush any buffered server moves so the server doesn't replay stale input
			if (Pawn->GetWorld() && Pawn->GetWorld()->GetNetMode() != NM_Client)
			{
				CMC->FlushServerMoves();
			}

			// Clear client-side saved moves so they don't replay when re-enabled
			if (FNetworkPredictionData_Client_Character* ClientData = CMC->GetPredictionData_Client_Character())
			{
				ClientData->SavedMoves.Empty();
				ClientData->PendingMove = nullptr;
			}
		}

		MoveComp->SetComponentTickEnabled(false);
	}
}

/**
 * Re-enable the movement component tick on a pawn after flythrough completes.
 * Clears any stale prediction data before re-enabling so saved moves don't snap the character.
 */
static void EnablePawnMovement(APawn* Pawn)
{
	if (!Pawn)
	{
		return;
	}
	if (UPawnMovementComponent* MoveComp = Pawn->GetMovementComponent())
	{
		if (UCharacterMovementComponent* CMC = Cast<UCharacterMovementComponent>(MoveComp))
		{
			CMC->ClearAccumulatedForces();

			if (Pawn->GetWorld() && Pawn->GetWorld()->GetNetMode() != NM_Client)
			{
				CMC->FlushServerMoves();
			}

			if (FNetworkPredictionData_Client_Character* ClientData = CMC->GetPredictionData_Client_Character())
			{
				ClientData->SavedMoves.Empty();
				ClientData->PendingMove = nullptr;
			}
		}

		MoveComp->SetComponentTickEnabled(true);
	}
}

/**
 * Find the server-side pawn that corresponds to the local player.
 * In client-server PIE, the server world has its own copy of each player's pawn.
 * We match by PlayerState->GetPlayerId().
 */
static APawn* FindServerPawnForLocalPlayer(UWorld* ServerWorld, APlayerController* ClientPC)
{
	if (!ServerWorld || !ClientPC || !ClientPC->PlayerState)
	{
		return nullptr;
	}

	const int32 LocalPlayerId = ClientPC->PlayerState->GetPlayerId();

	for (FConstPlayerControllerIterator It = ServerWorld->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* ServerPC = It->Get();
		if (ServerPC && ServerPC->PlayerState && ServerPC->PlayerState->GetPlayerId() == LocalPlayerId)
		{
			return ServerPC->GetPawn();
		}
	}
	return nullptr;
}

// ------------------------------------------------------------------
// Camera position update
// ------------------------------------------------------------------

void FClaireonFlythroughManager::UpdateCameraPosition(float Distance)
{
	int32 SegIdx = 0;
	float T = 0.0f;
	DistanceToSegmentAndT(Distance, SegIdx, T);

	CurrentPosition = EvaluateCatmullRom(SegIdx, T);

	// Interpolate rotation
	const int32 WpA = SegIdx;
	const int32 WpB = SegIdx + 1;

	FQuat RotA, RotB;

	if (WaypointUseTangent[WpA])
	{
		const FVector Tangent = EvaluateCatmullRomTangent(SegIdx, 0.0f);
		RotA = Tangent.GetSafeNormal().ToOrientationQuat();
	}
	else
	{
		RotA = WaypointRotations[WpA];
	}

	if (WpB < WaypointRotations.Num())
	{
		if (WaypointUseTangent[WpB])
		{
			const FVector Tangent = EvaluateCatmullRomTangent(SegIdx, 1.0f);
			RotB = Tangent.GetSafeNormal().ToOrientationQuat();
		}
		else
		{
			RotB = WaypointRotations[WpB];
		}
	}
	else
	{
		RotB = RotA;
	}

	CurrentRotation = FQuat::Slerp(RotA, RotB, T).Rotator();

	// Apply position/rotation to both client and server pawns.
	// The server pawn must be teleported first so it doesn't send corrections.
	UWorld* PIEWorld = FindPIEWorldWithLocalPlayer();
	if (!PIEWorld)
	{
		return;
	}

	APlayerController* ClientPC = nullptr;
	for (FConstPlayerControllerIterator It = PIEWorld->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (PC && PC->IsLocalController())
		{
			ClientPC = PC;
			break;
		}
	}

	if (!ClientPC)
	{
		return;
	}

	// Teleport the server-side pawn first (authoritative position)
	UWorld* ServerWorld = FindPIEServerWorld();
	if (ServerWorld)
	{
		if (APawn* ServerPawn = FindServerPawnForLocalPlayer(ServerWorld, ClientPC))
		{
			ServerPawn->SetActorLocation(CurrentPosition);
		}
	}

	// Teleport the client-side pawn and set control rotation
	if (APawn* ClientPawn = ClientPC->GetPawn())
	{
		ClientPawn->SetActorLocation(CurrentPosition);
	}
	ClientPC->SetControlRotation(CurrentRotation);
}

// ------------------------------------------------------------------
// Screenshot
// ------------------------------------------------------------------

void FClaireonFlythroughManager::TakeScreenshot()
{
	const FString Filename = FString::Printf(TEXT("flythrough_%03d"), ScreenshotCounter++);
	PendingScreenshotPath = ScreenshotSubDirectory / (Filename + TEXT(".png"));

	FScreenshotRequest::RequestScreenshot(PendingScreenshotPath, /*bShowUI=*/false, /*bAddFilenameSuffix=*/false);
}

// ------------------------------------------------------------------
// Event processing
// ------------------------------------------------------------------

void FClaireonFlythroughManager::ProcessWaypointEvents(int32 WaypointIndex)
{
	if (!Waypoints.IsValidIndex(WaypointIndex))
	{
		return;
	}

	const FFlythroughWaypoint& Wp = Waypoints[WaypointIndex];
	if (Wp.Events.Num() == 0 && Wp.PauseDuration <= 0.0f)
	{
		return;
	}

	PendingEvents = Wp.Events;
	PendingEventIndex = 0;
	PauseTimeRemaining = Wp.PauseDuration;

	// Process non-screenshot events immediately
	while (PendingEventIndex < PendingEvents.Num())
	{
		const FString& Evt = PendingEvents[PendingEventIndex];

		if (Evt == TEXT("screenshot"))
		{
			// Need to wait frames — switch to WaitingForEvent
			State = EFlythroughState::WaitingForEvent;
			EventFrameCounter = 0;
			return;
		}
		else if (Evt == TEXT("start_trace"))
		{
			if (!bTraceRecording)
			{
				const FString TraceDir = Config.ScreenshotDirectory.IsEmpty()
					? FPaths::ProjectSavedDir() / TEXT("Profiling")
					: Config.ScreenshotDirectory;

				const FString TraceFilename = TraceDir / FString::Printf(TEXT("flythrough_%s.utrace"),
					*FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));

				IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
				PlatformFile.CreateDirectoryTree(*FPaths::GetPath(TraceFilename));

				bTraceRecording = FTraceAuxiliary::Start(
					FTraceAuxiliary::EConnectionType::File,
					*TraceFilename,
					TEXT("cpu,frame,bookmark"),
					nullptr);

				if (bTraceRecording)
				{
					TraceFilePath = TraceFilename;
				}
			}
			++PendingEventIndex;
		}
		else if (Evt == TEXT("stop_trace"))
		{
			if (bTraceRecording)
			{
				TraceFilePath = FTraceAuxiliary::GetTraceDestinationString();
				FTraceAuxiliary::Stop();
				bTraceRecording = false;
			}
			++PendingEventIndex;
		}
		else if (Evt.StartsWith(TEXT("console_command:")))
		{
			const FString Cmd = Evt.Mid(16); // len("console_command:") == 16
			if (UWorld* PIEWorld = FindPIEWorldWithLocalPlayer())
			{
				GEngine->Exec(PIEWorld, *Cmd);
			}
			++PendingEventIndex;
		}
		else
		{
			++PendingEventIndex;
		}
	}

	// All events processed. Apply pause if needed.
	if (PauseTimeRemaining > 0.0f)
	{
		State = EFlythroughState::Paused;
	}
}

// ------------------------------------------------------------------
// Debug camera cleanup
// ------------------------------------------------------------------

void FClaireonFlythroughManager::DisableDebugCamera()
{
	// Re-enable the movement components and player input that were disabled in Start().
	UWorld* PIEWorld = FindPIEWorldWithLocalPlayer();
	if (!PIEWorld)
	{
		return;
	}

	APlayerController* ClientPC = nullptr;
	for (FConstPlayerControllerIterator It = PIEWorld->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (PC && PC->IsLocalController())
		{
			ClientPC = PC;
			break;
		}
	}

	if (!ClientPC)
	{
		return;
	}

	// Re-enable client pawn movement and input
	EnablePawnMovement(ClientPC->GetPawn());
	ClientPC->EnableInput(ClientPC);

	// Re-enable server pawn movement
	UWorld* ServerWorld = FindPIEServerWorld();
	if (ServerWorld)
	{
		if (APawn* ServerPawn = FindServerPawnForLocalPlayer(ServerWorld, ClientPC))
		{
			EnablePawnMovement(ServerPawn);
		}
	}
}

// ------------------------------------------------------------------
// Start / Stop
// ------------------------------------------------------------------

bool FClaireonFlythroughManager::Start(TArray<FFlythroughWaypoint> InWaypoints, FFlythroughConfig InConfig)
{
	if (InWaypoints.Num() < 2)
	{
		ErrorMessage = TEXT("Need at least 2 waypoints");
		State = EFlythroughState::Error;
		return false;
	}

	if (!GEditor || !GEditor->IsPlaySessionInProgress())
	{
		ErrorMessage = TEXT("PIE is not running");
		State = EFlythroughState::Error;
		return false;
	}

	Waypoints = MoveTemp(InWaypoints);
	Config = MoveTemp(InConfig);

	// Build spline points
	SplinePoints.Reset(Waypoints.Num());
	for (const FFlythroughWaypoint& Wp : Waypoints)
	{
		SplinePoints.Add(Wp.Position);
	}

	// Resolve waypoint rotations
	WaypointRotations.SetNum(Waypoints.Num());
	WaypointUseTangent.SetNum(Waypoints.Num());

	for (int32 i = 0; i < Waypoints.Num(); ++i)
	{
		if (Waypoints[i].Rotation.IsSet())
		{
			WaypointRotations[i] = Waypoints[i].Rotation.GetValue().Quaternion();
			WaypointUseTangent[i] = false;
		}
		else if (Waypoints[i].LookAt.IsSet())
		{
			const FVector Dir = (Waypoints[i].LookAt.GetValue() - Waypoints[i].Position).GetSafeNormal();
			WaypointRotations[i] = Dir.ToOrientationQuat();
			WaypointUseTangent[i] = false;
		}
		else
		{
			WaypointRotations[i] = FQuat::Identity;
			WaypointUseTangent[i] = true;
		}
	}

	// Compute arc lengths per segment
	const int32 NumSegments = SplinePoints.Num() - 1;
	SegmentArcLengthSamples.SetNum(NumSegments);
	CumulativeSegmentDistances.SetNum(NumSegments);
	TotalSplineLength = 0.0f;

	for (int32 Seg = 0; Seg < NumSegments; ++Seg)
	{
		TArray<float>& Samples = SegmentArcLengthSamples[Seg];
		Samples.SetNum(ARC_LENGTH_SAMPLES);

		FVector Prev = EvaluateCatmullRom(Seg, 0.0f);
		float CumDist = 0.0f;

		for (int32 S = 0; S < ARC_LENGTH_SAMPLES; ++S)
		{
			const float T = static_cast<float>(S + 1) / static_cast<float>(ARC_LENGTH_SAMPLES);
			const FVector Curr = EvaluateCatmullRom(Seg, T);
			CumDist += FVector::Dist(Prev, Curr);
			Samples[S] = CumDist;
			Prev = Curr;
		}

		TotalSplineLength += CumDist;
		CumulativeSegmentDistances[Seg] = TotalSplineLength;
	}

	// Compute speeds and durations per segment
	SegmentSpeeds.SetNum(NumSegments);
	SegmentDurations.SetNum(NumSegments);
	TotalEstimatedTime = 0.0;

	for (int32 Seg = 0; Seg < NumSegments; ++Seg)
	{
		const float SegLength = SegmentArcLengthSamples[Seg].Last();

		if (Waypoints[Seg].Duration.IsSet())
		{
			SegmentDurations[Seg] = FMath::Max(Waypoints[Seg].Duration.GetValue(), 0.01f);
			SegmentSpeeds[Seg] = SegLength / SegmentDurations[Seg];
		}
		else if (Waypoints[Seg].Speed.IsSet())
		{
			SegmentSpeeds[Seg] = FMath::Max(Waypoints[Seg].Speed.GetValue(), 1.0f);
			SegmentDurations[Seg] = SegLength / SegmentSpeeds[Seg];
		}
		else
		{
			SegmentSpeeds[Seg] = Config.DefaultSpeed;
			SegmentDurations[Seg] = SegLength / Config.DefaultSpeed;
		}

		TotalEstimatedTime += SegmentDurations[Seg];
		TotalEstimatedTime += Waypoints[Seg].PauseDuration;
	}
	// Add final waypoint pause
	TotalEstimatedTime += Waypoints.Last().PauseDuration;

	// Create screenshot subdirectory
	if (Config.ScreenshotDirectory.IsEmpty())
	{
		Config.ScreenshotDirectory = FPaths::ProjectSavedDir() / TEXT("Screenshots") / TEXT("Flythrough");
	}
	ScreenshotSubDirectory = Config.ScreenshotDirectory / FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.CreateDirectoryTree(*ScreenshotSubDirectory);

	// Find the PIE world with a local player controller — Client netmode creates both
	// a server and client world; only the client world has a local player controller.
	UWorld* PIEWorld = FindPIEWorldWithLocalPlayer();
	if (!PIEWorld)
	{
		ErrorMessage = TEXT("Cannot find PIE world with local player");
		State = EFlythroughState::Error;
		return false;
	}

	APlayerController* PC = nullptr;
	for (FConstPlayerControllerIterator It = PIEWorld->GetPlayerControllerIterator(); It; ++It)
	{
		if (It->Get() && It->Get()->IsLocalController())
		{
			PC = It->Get();
			break;
		}
	}

	if (!PC)
	{
		ErrorMessage = TEXT("Cannot find local player controller in PIE");
		State = EFlythroughState::Error;
		return false;
	}

	UCheatManager* CheatMgr = PC->CheatManager;
	if (Config.bAutoGodMode && CheatMgr)
	{
		CheatMgr->God();
	}

	// Disable movement components on both client and server pawns so they don't fight
	// the flythrough teleportation. In client-server PIE, the server pawn's movement
	// component would apply gravity/velocity and send net corrections to the client.
	DisablePawnMovement(PC->GetPawn());
	PC->DisableInput(PC);

	UWorld* ServerWorld = FindPIEServerWorld();
	if (ServerWorld)
	{
		if (APawn* ServerPawn = FindServerPawnForLocalPlayer(ServerWorld, PC))
		{
			DisablePawnMovement(ServerPawn);
		}
	}

	// Reset runtime state
	CurrentDistance = 0.0f;
	CurrentSegmentIndex = 0;
	NextWaypointIndex = 1;
	ElapsedTime = 0.0;
	StartTime = FPlatformTime::Seconds();
	ScreenshotCounter = 0;
	TimeSinceLastAutoScreenshot = 0.0f;
	PauseTimeRemaining = 0.0f;
	bTraceRecording = false;
	TraceFilePath.Empty();
	ErrorMessage.Empty();

	State = EFlythroughState::Initializing;

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FClaireonFlythroughManager::Tick));

	return true;
}

void FClaireonFlythroughManager::Stop()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	if (bTraceRecording)
	{
		TraceFilePath = FTraceAuxiliary::GetTraceDestinationString();
		FTraceAuxiliary::Stop();
		bTraceRecording = false;
	}

	if (State != EFlythroughState::Idle && State != EFlythroughState::Complete && State != EFlythroughState::Error)
	{
		DisableDebugCamera();
	}

	State = EFlythroughState::Idle;
}

// ------------------------------------------------------------------
// Tick
// ------------------------------------------------------------------

bool FClaireonFlythroughManager::Tick(float DeltaTime)
{
	// Safety: if PIE ended, stop
	if (!GEditor || !GEditor->IsPlaySessionInProgress())
	{
		State = EFlythroughState::Complete;
		if (TickerHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
			TickerHandle.Reset();
		}
		return false;
	}

	ElapsedTime = FPlatformTime::Seconds() - StartTime;

	switch (State)
	{
	case EFlythroughState::Initializing:
	{
		// Set initial camera position via the player pawn
		UpdateCameraPosition(0.0f);

		// Process first waypoint events
		ProcessWaypointEvents(0);

		if (State != EFlythroughState::WaitingForEvent && State != EFlythroughState::Paused)
		{
			State = EFlythroughState::Flying;
		}
		return true;
	}

	case EFlythroughState::Flying:
	{
		// Advance along spline
		if (CurrentSegmentIndex < SegmentSpeeds.Num())
		{
			CurrentDistance += SegmentSpeeds[CurrentSegmentIndex] * DeltaTime;
		}

		// Check for waypoint crossings
		while (NextWaypointIndex < Waypoints.Num()
			&& CurrentDistance >= CumulativeSegmentDistances[NextWaypointIndex - 1])
		{
			// Snap to waypoint distance
			CurrentDistance = CumulativeSegmentDistances[NextWaypointIndex - 1];
			CurrentSegmentIndex = FMath::Min(NextWaypointIndex, SegmentSpeeds.Num() - 1);

			UpdateCameraPosition(CurrentDistance);
			ProcessWaypointEvents(NextWaypointIndex);

			++NextWaypointIndex;

			if (State != EFlythroughState::Flying)
			{
				return true; // event processing changed state (pause or waiting)
			}
		}

		// Check completion
		if (CurrentDistance >= TotalSplineLength)
		{
			CurrentDistance = TotalSplineLength;
			UpdateCameraPosition(CurrentDistance);
			DisableDebugCamera();
			State = EFlythroughState::Complete;
			return false;
		}

		UpdateCameraPosition(CurrentDistance);

		// Auto-screenshot interval
		if (Config.AutoScreenshotInterval > 0.0f)
		{
			TimeSinceLastAutoScreenshot += DeltaTime;
			if (TimeSinceLastAutoScreenshot >= Config.AutoScreenshotInterval)
			{
				TimeSinceLastAutoScreenshot -= Config.AutoScreenshotInterval;
				TakeScreenshot();
			}
		}

		return true;
	}

	case EFlythroughState::Paused:
	{
		PauseTimeRemaining -= DeltaTime;
		if (PauseTimeRemaining <= 0.0f)
		{
			PauseTimeRemaining = 0.0f;
			State = EFlythroughState::Flying;
		}
		return true;
	}

	case EFlythroughState::WaitingForEvent:
	{
		++EventFrameCounter;

		// Frame-counting sequence for screenshots:
		// Frame 0-1: wait for render
		// Frame 2: take screenshot
		// Frame 3-5: wait for file write
		// Frame 6+: check file and resume

		if (EventFrameCounter == 2)
		{
			TakeScreenshot();
		}
		else if (EventFrameCounter >= 5)
		{
			// Check if screenshot file exists or timeout (10 frames max)
			const bool bFileExists = !PendingScreenshotPath.IsEmpty()
				&& FPlatformFileManager::Get().GetPlatformFile().FileExists(*PendingScreenshotPath);

			if (bFileExists || EventFrameCounter >= 10)
			{
				PendingScreenshotPath.Empty();

				// Continue processing remaining events
				++PendingEventIndex;
				while (PendingEventIndex < PendingEvents.Num())
				{
					const FString& Evt = PendingEvents[PendingEventIndex];
					if (Evt == TEXT("screenshot"))
					{
						EventFrameCounter = 0;
						return true; // wait for next screenshot
					}
					// Process non-screenshot events inline
					if (Evt == TEXT("start_trace"))
					{
						if (!bTraceRecording)
						{
							const FString TraceDir = Config.ScreenshotDirectory.IsEmpty()
								? FPaths::ProjectSavedDir() / TEXT("Profiling")
								: Config.ScreenshotDirectory;
							const FString TraceFilename = TraceDir / FString::Printf(TEXT("flythrough_%s.utrace"),
								*FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
							IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
							PF.CreateDirectoryTree(*FPaths::GetPath(TraceFilename));
							bTraceRecording = FTraceAuxiliary::Start(
								FTraceAuxiliary::EConnectionType::File, *TraceFilename, TEXT("cpu,frame,bookmark"), nullptr);
							if (bTraceRecording)
							{
								TraceFilePath = TraceFilename;
							}
						}
					}
					else if (Evt == TEXT("stop_trace"))
					{
						if (bTraceRecording)
						{
							TraceFilePath = FTraceAuxiliary::GetTraceDestinationString();
							FTraceAuxiliary::Stop();
							bTraceRecording = false;
						}
					}
					else if (Evt.StartsWith(TEXT("console_command:")))
					{
						if (UWorld* CmdWorld = FindPIEWorldWithLocalPlayer())
						{
							GEngine->Exec(CmdWorld, *Evt.Mid(16));
						}
					}
					++PendingEventIndex;
				}

				// All events done — apply pause or resume flying
				if (PauseTimeRemaining > 0.0f)
				{
					State = EFlythroughState::Paused;
				}
				else
				{
					State = EFlythroughState::Flying;
				}
			}
		}

		return true;
	}

	case EFlythroughState::Complete:
	case EFlythroughState::Error:
		return false;

	default:
		return true;
	}
}

// ------------------------------------------------------------------
// GetStatus
// ------------------------------------------------------------------

FFlythroughStatusInfo FClaireonFlythroughManager::GetStatus() const
{
	FFlythroughStatusInfo Info;
	Info.State = State;
	Info.Progress = (TotalSplineLength > 0.0f) ? (CurrentDistance / TotalSplineLength) : 0.0f;
	Info.ElapsedTime = ElapsedTime;
	Info.TotalEstimatedTime = TotalEstimatedTime;
	Info.CurrentWaypointIndex = FMath::Min(NextWaypointIndex, Waypoints.Num()) - 1;
	Info.TotalWaypoints = Waypoints.Num();
	Info.Position = CurrentPosition;
	Info.Rotation = CurrentRotation;
	Info.ScreenshotsTaken = ScreenshotCounter;
	Info.bTraceActive = bTraceRecording;
	Info.ScreenshotDirectory = ScreenshotSubDirectory;
	Info.TraceFilePath = TraceFilePath;
	Info.ErrorMessage = ErrorMessage;
	return Info;
}

// ------------------------------------------------------------------
// Singleton
// ------------------------------------------------------------------

FClaireonFlythroughManager* FClaireonFlythroughManager::GetActive()
{
	return ActiveManager.Get();
}

void FClaireonFlythroughManager::DestroyActive()
{
	ActiveManager.Reset();
}

void FClaireonFlythroughManager::SetActive(TUniquePtr<FClaireonFlythroughManager> InManager)
{
	ActiveManager = MoveTemp(InManager);
}
