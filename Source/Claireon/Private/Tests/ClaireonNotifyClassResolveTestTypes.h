// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
// Test-only UCLASS fixture for the WI-12 notify-class resolution tests. Lives
// under Private/Tests so the type is not part of the module's public surface.
#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "ClaireonNotifyClassResolveTestTypes.generated.h"

// Native notify-state fixture whose name deliberately contains neither the
// "State" substring nor the "ANS_" prefix, mirroring game classes like
// FSANS_ApplyGameplayEffects that the legacy name heuristic misclassified as
// instant notifies (and therefore failed to resolve under every spelling).
// Kept game-module-free so the tests stay inside Claireon's dependency surface.
UCLASS()
class UClaireonTestFSANSApplyBuff : public UAnimNotifyState
{
	GENERATED_BODY()
};
