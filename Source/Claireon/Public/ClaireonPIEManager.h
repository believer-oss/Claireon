// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

class AActor;
class UWorld;

/**
 * Represents a single damage event captured by a damage listener.
 */
struct FMCPDamageEvent
{
	/** Timestamp when the damage occurred (seconds since PIE start) */
	double Timestamp = 0.0;

	/** Amount of damage dealt */
	float Damage = 0.0f;

	/** Stable actor ID of the instigator (if available) */
	FString InstigatorId;

	/** Class name of the instigator */
	FString InstigatorClass;

	/** Gameplay tags associated with the damage */
	TArray<FString> Tags;
};

/**
 * State for an active damage listener bound to an actor's health component.
 */
struct FMCPDamageListenerState
{
	/** Unique listener identifier */
	FString ListenerId;

	/** Weak reference to the actor being monitored */
	TWeakObjectPtr<AActor> BoundActor;

	/** Handle for the delegate binding (used for unbinding) */
	FDelegateHandle DelegateHandle;

	/** Accumulated damage events */
	TArray<FMCPDamageEvent> Events;

	/** Lock for thread-safe access to Events */
	mutable FCriticalSection EventLock;
};

/**
 * Singleton state manager for PIE (Play In Editor) sessions.
 * Tracks active sessions, generates stable actor IDs, and provides
 * session lifecycle management for MCP tools.
 */
class FClaireonPIEManager
{
public:
	static FClaireonPIEManager& Get();

	/** PIE session tracking */
	struct FPIESession
	{
		FString SessionId;
		FString MapPath;
		FString NetMode;
		FDateTime StartTime;
		bool bIsActive = false;
	};

	/** Get the current active session info, or nullptr if no session is active */
	const FPIESession* GetActiveSession() const;

	/** Generate a unique session ID */
	FString GenerateSessionId();

	/** Track PIE start */
	void OnPIEStarted(const FString& SessionId, const FString& MapPath, const FString& NetMode);

	/** Track PIE end - clears session and actor ID map */
	void OnPIEStopped();

	/** Get or assign a stable actor ID for the given actor */
	FString GetActorId(AActor* Actor);

	/** Resolve an actor ID back to an actor pointer, or nullptr if invalid/stale */
	AActor* ResolveActorId(const FString& ActorId, UWorld* World);

	// --- Damage Listener Infrastructure ---

	/**
	 * Register a damage listener on an actor. Attempts to find a health-related
	 * component and bind to its damage delegate.
	 * @param Actor The actor to monitor for damage
	 * @return Listener ID if successful, empty string on failure
	 */
	FString RegisterDamageListener(AActor* Actor);

	/**
	 * Retrieve accumulated damage events for a listener.
	 * @param ListenerId The listener to query
	 * @param bClear If true, clears the event list after copying
	 * @return Array of damage events (empty if listener not found)
	 */
	TArray<FMCPDamageEvent> GetDamageEvents(const FString& ListenerId, bool bClear);

	/**
	 * Unregister a damage listener, unbinding the delegate.
	 * @param ListenerId The listener to remove
	 * @return true if the listener was found and removed
	 */
	bool UnregisterDamageListener(const FString& ListenerId);

	/**
	 * Bind to FEditorDelegates::BeginPIE / EndPIE so the manager
	 * detects PIE sessions regardless of how they were started.
	 * Safe to call multiple times — only binds once.
	 */
	void BindEditorDelegates();

	/** Unbind editor delegates (called on module shutdown) */
	void UnbindEditorDelegates();

	/**
	 * Disable CPU throttling for the duration of PIE by clearing
	 * bThrottleCPUWhenNotForeground in-memory (no SaveConfig).
	 * The original value is saved so RestoreThrottleCPU() can undo it.
	 */
	void DisableThrottleCPU();

	/**
	 * Restore the CPU throttle setting to the value it had before the last
	 * call to DisableThrottleCPU(). No-op if DisableThrottleCPU was never called.
	 */
	void RestoreThrottleCPU();

private:
	void HandleBeginPIE(bool bIsSimulating);
	void HandleEndPIE(bool bIsSimulating);

	TOptional<FPIESession> ActiveSession;
	TMap<FString, TWeakObjectPtr<AActor>> ActorIdMap;
	TMap<TWeakObjectPtr<AActor>, FString> ReverseActorIdMap;
	int32 ActorIdCounter = 0;
	mutable FCriticalSection CriticalSection;

	FDelegateHandle BeginPIEHandle;
	FDelegateHandle EndPIEHandle;
	bool bDelegatesBound = false;

	/** Active damage listeners keyed by listener ID */
	TMap<FString, TSharedPtr<FMCPDamageListenerState>> DamageListeners;
	int32 DamageListenerCounter = 0;

	/** Clean up all damage listeners */
	void ClearDamageListeners();

	/** Saved value of bThrottleCPUWhenNotForeground before PIE started */
	bool bSavedThrottleCPUWhenNotForeground = true;

	/** Whether we have a saved throttle value to restore */
	bool bHasSavedThrottleCPU = false;
};
