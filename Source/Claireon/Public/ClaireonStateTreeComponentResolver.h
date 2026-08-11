// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class AActor;
class UActorComponent;

/**
 * Shared State Tree component lookup for the runtime tools
 * (statetree_runtime_inspect, statetree_runtime_send_event).
 *
 * Both tools used to match on the class NAME containing "StateTreeComponent",
 * which can never match UStateTreeAIComponent -- the "AI" splits the substring.
 * Every AI controller in this project uses the AI subclass, so the default match
 * failed on exactly the actors the tools exist to inspect. Matching is by type
 * now, so any UStateTreeComponent subclass resolves.
 */
namespace ClaireonStateTreeComponentResolver
{
/**
 * Returns the first State Tree component on Actor, or nullptr.
 *
 * With OptionalComponentClass empty, matches any UStateTreeComponent subclass.
 * With it set, keeps the historical case-insensitive class-name substring
 * match, so it stays an escape hatch for picking one component out of several
 * (or for a component that is not a UStateTreeComponent at all).
 */
UActorComponent* FindStateTreeComponent(AActor* Actor, const FString& OptionalComponentClass);

/** "\n  - Name (Class)" per component. For the not-found error, which is the
 *  only place a caller can see what WAS on the actor. */
FString DescribeComponents(AActor* Actor);
} // namespace ClaireonStateTreeComponentResolver
