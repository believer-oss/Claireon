// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
// Out-of-line bodies for test-only fixtures declared in ClaireonTestTypes.h.

#include "ClaireonTestTypes.h"

UClaireonTestAsyncAction* UClaireonTestAsyncAction::ClaireonTestAsyncDelay(UObject* /*WorldContextObject*/, float /*Duration*/)
{
	return NewObject<UClaireonTestAsyncAction>();
}

UClaireonTestGameplayTask* UClaireonTestGameplayTask::ClaireonTestWaitForThing(
	UObject* /*WorldContextObject*/, float /*Duration*/)
{
	return NewObject<UClaireonTestGameplayTask>();
}

UClaireonTestGameplayTask* UClaireonTestGameplayTask::ClaireonTestSpawnThing(
	UObject* /*WorldContextObject*/, TSubclassOf<AActor> /*Class*/)
{
	return NewObject<UClaireonTestGameplayTask>();
}

// Non-trivial bodies on purpose: these exist so the override tools see a parent
// implementation that an event override would shadow.
void AClaireonNativeEventOverrideFixtureActor::ApplyNativeDefault_Implementation()
{
	NativeCounter += 1;
	SetActorHiddenInGame(NativeCounter % 2 == 0);
}

int32 AClaireonNativeEventOverrideFixtureActor::ComputeNativeValue_Implementation()
{
	return NativeCounter * 7 + 1;
}
