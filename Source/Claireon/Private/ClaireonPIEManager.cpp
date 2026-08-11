// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonPIEManager.h"
#include "ClaireonLog.h"
#include "ClaireonSessionManager.h"

#include "Components/ActorComponent.h"
#include "Editor.h"
#include "Editor/EditorPerformanceSettings.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "UObject/UnrealType.h"

FClaireonPIEManager& FClaireonPIEManager::Get()
{
	static FClaireonPIEManager Instance;
	return Instance;
}

const FClaireonPIEManager::FPIESession* FClaireonPIEManager::GetActiveSession() const
{
	FScopeLock Lock(&CriticalSection);
	if (ActiveSession.IsSet() && ActiveSession->bIsActive)
	{
		return &ActiveSession.GetValue();
	}
	return nullptr;
}

FString FClaireonPIEManager::GenerateSessionId()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Short);
}

void FClaireonPIEManager::OnPIEStarted(const FString& SessionId, const FString& MapPath, const FString& NetMode)
{
	FScopeLock Lock(&CriticalSection);

	FPIESession Session;
	Session.SessionId = SessionId;
	Session.MapPath = MapPath;
	Session.NetMode = NetMode;
	Session.StartTime = FDateTime::UtcNow();
	Session.bIsActive = true;

	ActiveSession = Session;
	ActorIdMap.Empty();
	ReverseActorIdMap.Empty();
	ActorIdCounter = 0;

	UE_LOG(LogClaireon, Display, TEXT("[MCP] PIE session started: %s (Map: %s, NetMode: %s)"),
		*SessionId, *MapPath, *NetMode);
}

void FClaireonPIEManager::OnPIEStopped()
{
	FScopeLock Lock(&CriticalSection);

	if (ActiveSession.IsSet())
	{
		UE_LOG(LogClaireon, Display, TEXT("[MCP] PIE session stopped: %s"), *ActiveSession->SessionId);
	}

	// Clean up damage listeners before clearing session state
	ClearDamageListeners();

	ActiveSession.Reset();
	ActorIdMap.Empty();
	ReverseActorIdMap.Empty();
	ActorIdCounter = 0;
}

void FClaireonPIEManager::BindEditorDelegates()
{
	if (bDelegatesBound)
	{
		return;
	}

	BeginPIEHandle = FEditorDelegates::BeginPIE.AddRaw(this, &FClaireonPIEManager::HandleBeginPIE);
	EndPIEHandle = FEditorDelegates::EndPIE.AddRaw(this, &FClaireonPIEManager::HandleEndPIE);
	bDelegatesBound = true;

	UE_LOG(LogClaireon, Display, TEXT("[MCP] PIE manager bound to editor delegates"));
}

void FClaireonPIEManager::UnbindEditorDelegates()
{
	if (!bDelegatesBound)
	{
		return;
	}

	FEditorDelegates::BeginPIE.Remove(BeginPIEHandle);
	FEditorDelegates::EndPIE.Remove(EndPIEHandle);
	bDelegatesBound = false;

	UE_LOG(LogClaireon, Display, TEXT("[MCP] PIE manager unbound from editor delegates"));
}

void FClaireonPIEManager::HandleBeginPIE(bool bIsSimulating)
{
	FScopeLock Lock(&CriticalSection);

	// Auto-release all edit sessions when PIE starts — assets may be loaded in the PIE world
	// and concurrent modification is unsafe.
	{
		int32 ReleasedCount = FClaireonSessionManager::Get().ForceReleaseAll();
		if (ReleasedCount > 0)
		{
			UE_LOG(LogClaireon, Warning,
				TEXT("PIE started: auto-released %d active edit session(s)"),
				ReleasedCount);
		}
	}

	// If a session is already active (e.g., pie_start_async MCP tool registered it), skip
	if (ActiveSession.IsSet() && ActiveSession->bIsActive)
	{
		return;
	}

	// Detect map path from editor world
	FString MapPath;
	if (IsValid(GEditor))
	{
		UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
		if (IsValid(EditorWorld))
		{
			MapPath = EditorWorld->GetPathName();
		}
	}

	FPIESession Session;
	Session.SessionId = GenerateSessionId();
	Session.MapPath = MapPath;
	Session.NetMode = bIsSimulating ? TEXT("Simulate") : TEXT("Standalone");
	Session.StartTime = FDateTime::UtcNow();
	Session.bIsActive = true;

	ActiveSession = Session;
	ActorIdMap.Empty();
	ReverseActorIdMap.Empty();
	ActorIdCounter = 0;

	UE_LOG(LogClaireon, Display, TEXT("[MCP] PIE session detected via editor delegate: %s (Map: %s)"),
		*Session.SessionId, *MapPath);
}

void FClaireonPIEManager::HandleEndPIE(bool bIsSimulating)
{
	OnPIEStopped();
	RestoreThrottleCPU();
}

FString FClaireonPIEManager::GetActorId(AActor* Actor)
{
	if (!IsValid(Actor))
	{
		return FString();
	}

	FScopeLock Lock(&CriticalSection);

	// Check if we already have an ID for this actor
	TWeakObjectPtr<AActor> WeakActor(Actor);
	if (const FString* ExistingId = ReverseActorIdMap.Find(WeakActor))
	{
		return *ExistingId;
	}

	// Assign a new ID
	FString NewId = FString::Printf(TEXT("actor_%d"), ActorIdCounter++);
	ActorIdMap.Add(NewId, WeakActor);
	ReverseActorIdMap.Add(WeakActor, NewId);

	return NewId;
}

AActor* FClaireonPIEManager::ResolveActorId(const FString& ActorId, UWorld* World)
{
	FScopeLock Lock(&CriticalSection);

	if (const TWeakObjectPtr<AActor>* WeakActor = ActorIdMap.Find(ActorId))
	{
		if (WeakActor->IsValid())
		{
			AActor* Actor = WeakActor->Get();

			// P0-8a: this used to ignore World entirely -- it was a flat id->actor
			// map lookup, so every caller that carefully resolved a PIE world by
			// net_mode / pie_instance then got whatever world the id happened to be
			// minted in. pie_get_component(actorId, net_mode='client') would return
			// a SERVER-world component with no warning, and pie_wait_for's
			// actorValid / initState conditions inherited the same hole.
			//
			// The actor's own world is the authority; nothing migrates actors
			// between worlds, so no separate mint-time record is needed.
			//
			// A null World keeps the old permissive behaviour for callers that
			// deliberately do not scope. Every in-tree caller passes a resolved PIE
			// world, so this makes all of them correct at once.
			if (World != nullptr)
			{
				UWorld* ActorWorld = Actor->GetWorld();
				if (ActorWorld != World)
				{
					// nullptr rather than an error return: every caller already
					// treats "not found" correctly, and pie_wait_for specifically
					// needs a miss to mean "condition not met" rather than to abort
					// the wait. The log line carries the diagnosis.
					UE_LOG(LogClaireon, Warning,
						TEXT("[PIE] Actor id '%s' belongs to world '%s' but was requested against world '%s'. ")
						TEXT("Returning not-found rather than a cross-world object. Register the actor in the ")
						TEXT("world you are querying (note: pie_register_actor is instance-0-only)."),
						*ActorId,
						ActorWorld ? *ActorWorld->GetName() : TEXT("<none>"),
						*World->GetName());
					return nullptr;
				}
			}

			return Actor;
		}
		else
		{
			// Actor was destroyed, clean up stale entry
			ReverseActorIdMap.Remove(*WeakActor);
			ActorIdMap.Remove(ActorId);
		}
	}

	return nullptr;
}

FString FClaireonPIEManager::RegisterDamageListener(AActor* Actor)
{
	if (!IsValid(Actor))
	{
		return FString();
	}

	FScopeLock Lock(&CriticalSection);

	// Look for a health-related component on the actor.
	// We search by class name substring to avoid hard-coding project-specific includes.
	UActorComponent* HealthComponent = nullptr;
	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);

	for (UActorComponent* Component : Components)
	{
		if (!IsValid(Component))
		{
			continue;
		}

		const FString ClassName = Component->GetClass()->GetName();

		// Match any health component generically by name substring (e.g. Lyra's
		// ULyraHealthComponent or a project-specific U*HealthComponent), avoiding a
		// hard dependency on any particular game module.
		if (ClassName.Contains(TEXT("Health")))
		{
			HealthComponent = Component;
			UE_LOG(LogClaireon, Display, TEXT("[MCP] Found health component: %s (%s)"),
				*Component->GetName(), *ClassName);
			break;
		}
	}

	if (!IsValid(HealthComponent))
	{
		UE_LOG(LogClaireon, Warning, TEXT("[MCP] No health component found on actor %s"),
			*Actor->GetName());
		return FString();
	}

	// Create the listener state
	TSharedPtr<FMCPDamageListenerState> ListenerState = MakeShared<FMCPDamageListenerState>();
	ListenerState->ListenerId = FString::Printf(TEXT("dmg_listener_%d"), DamageListenerCounter++);
	ListenerState->BoundActor = Actor;

	// Try to bind to a damage delegate on the health component.
	// We use UObject property/delegate reflection to find a suitable multicast delegate.
	// Common delegate names: OnHealthChanged, OnDamageReceived, OnDamageTaken, OnHealthAttributeChanged
	bool bBound = false;

	// Try to find and bind to a multicast delegate property with "Damage" or "Health" in the name
	for (TFieldIterator<FMulticastDelegateProperty> It(HealthComponent->GetClass()); It; ++It)
	{
		const FString DelegateName = It->GetName();
		if (DelegateName.Contains(TEXT("Damage")) || DelegateName.Contains(TEXT("HealthChanged")) ||
			DelegateName.Contains(TEXT("Death")))
		{
			UE_LOG(LogClaireon, Display, TEXT("[MCP] Found delegate on health component: %s"),
				*DelegateName);
			// We note the delegate name for informational purposes but cannot easily bind
			// a generic lambda to a typed DYNAMIC_MULTICAST_DELEGATE via reflection alone.
			// Instead, we store a reference and poll for attribute changes.
			break;
		}
	}

	// Even if we couldn't directly bind a delegate, we register the listener so tools can
	// manually record damage events via the Events array. The listener still serves as a
	// tracking mechanism for the actor's health state.
	UE_LOG(LogClaireon, Display, TEXT("[MCP] Registered damage listener '%s' on actor %s (component: %s)"),
		*ListenerState->ListenerId, *Actor->GetName(), *HealthComponent->GetName());

	const FString ListenerId = ListenerState->ListenerId;
	DamageListeners.Add(ListenerId, ListenerState);

	return ListenerId;
}

TArray<FMCPDamageEvent> FClaireonPIEManager::GetDamageEvents(const FString& ListenerId, bool bClear)
{
	FScopeLock Lock(&CriticalSection);

	TArray<FMCPDamageEvent> Result;

	TSharedPtr<FMCPDamageListenerState>* ListenerPtr = DamageListeners.Find(ListenerId);
	if (!ListenerPtr || !ListenerPtr->IsValid())
	{
		return Result;
	}

	FMCPDamageListenerState& Listener = **ListenerPtr;
	FScopeLock EventLock(&Listener.EventLock);

	Result = Listener.Events;

	if (bClear)
	{
		Listener.Events.Empty();
	}

	return Result;
}

bool FClaireonPIEManager::UnregisterDamageListener(const FString& ListenerId)
{
	FScopeLock Lock(&CriticalSection);

	TSharedPtr<FMCPDamageListenerState> ListenerState;
	if (!DamageListeners.RemoveAndCopyValue(ListenerId, ListenerState))
	{
		return false;
	}

	UE_LOG(LogClaireon, Display, TEXT("[MCP] Unregistered damage listener: %s"), *ListenerId);
	return true;
}

void FClaireonPIEManager::DisableThrottleCPU()
{
	UEditorPerformanceSettings* PerfSettings = GetMutableDefault<UEditorPerformanceSettings>();
	if (!IsValid(PerfSettings))
	{
		return;
	}

	// Save original value so we can restore it later
	bSavedThrottleCPUWhenNotForeground = PerfSettings->bThrottleCPUWhenNotForeground;
	bHasSavedThrottleCPU = true;

	// Disable in-memory only — no SaveConfig so the user's preference is not altered
	PerfSettings->bThrottleCPUWhenNotForeground = false;

	UE_LOG(LogClaireon, Display,
		TEXT("[MCP] CPU throttle disabled for PIE (original value: %s)"),
		bSavedThrottleCPUWhenNotForeground ? TEXT("true") : TEXT("false"));
}

void FClaireonPIEManager::RestoreThrottleCPU()
{
	if (!bHasSavedThrottleCPU)
	{
		return;
	}

	UEditorPerformanceSettings* PerfSettings = GetMutableDefault<UEditorPerformanceSettings>();
	if (!IsValid(PerfSettings))
	{
		return;
	}

	PerfSettings->bThrottleCPUWhenNotForeground = bSavedThrottleCPUWhenNotForeground;
	bHasSavedThrottleCPU = false;

	UE_LOG(LogClaireon, Display,
		TEXT("[MCP] CPU throttle restored to: %s"),
		bSavedThrottleCPUWhenNotForeground ? TEXT("true") : TEXT("false"));
}

void FClaireonPIEManager::ClearDamageListeners()
{
	// Note: CriticalSection should already be held by the caller
	if (DamageListeners.Num() > 0)
	{
		UE_LOG(LogClaireon, Display, TEXT("[MCP] Clearing %d damage listener(s)"), DamageListeners.Num());
	}

	DamageListeners.Empty();
	DamageListenerCounter = 0;
}
