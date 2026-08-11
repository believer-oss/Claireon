// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Templates/SubclassOf.h"

class FJsonObject;
class UWorld;
class AActor;
class UAbilitySystemComponent;
class UGameplayEffect;
class UGameplayAbility;
struct FGameplayAttribute;

/**
 * Shared plumbing for the runtime GAS tools (gas_runtime_inspect / gas_apply_effect /
 * gas_remove_effect / gas_grant_ability / gas_activate_ability / gas_set_tags /
 * gas_set_attribute).
 *
 * Engine GAS API only -- no dependency on any host-game module, so
 * the whole gas_* changeset stays a Claireon-only PR (see
 * CLAIREON-GAS-TOOLS-SPEC.md section 1). Target resolution goes through
 * ClaireonPIEWorldResolver so client-side actors are reachable; GAS is
 * authority-driven, so absent an explicit net_mode these tools default to the
 * server world (the authoritative ASC).
 */
namespace ClaireonGasToolCommon
{
	/** Resolved runtime target: the PIE world, the actor, and its ASC. */
	struct FGasTarget
	{
		UWorld* World = nullptr;
		AActor* Actor = nullptr;
		UAbilitySystemComponent* ASC = nullptr;
	};

	/**
	 * Add the shared input-schema properties every gas_* tool exposes: the
	 * required `actorId` string plus ClaireonPIEWorldResolver's pie_instance /
	 * net_mode params. Does NOT populate the schema's "required" array -- each
	 * tool owns that (all require actorId; some add more).
	 */
	void AddCommonSchemaParams(const TSharedPtr<FJsonObject>& Properties);

	/**
	 * Resolve the target ASC from tool arguments. Guards that PIE is running,
	 * defaults net_mode to "server" when the caller omitted it (authoritative
	 * ASC), resolves the world via ClaireonPIEWorldResolver, resolves actorId
	 * via FClaireonPIEManager, and fetches the ASC (IAbilitySystemInterface,
	 * falling back to UAbilitySystemGlobals). Returns false with OutError set on
	 * any failure. NOTE: may add a default "net_mode" field to Arguments.
	 */
	bool ResolveTarget(const TSharedPtr<FJsonObject>& Arguments, FGasTarget& OutTarget, FString& OutError);

	/**
	 * Resolve a UGameplayEffect subclass from either `effect_class_path`
	 * (authoritative, e.g. "/Game/.../GE_X.GE_X_C") or `effect_name` (substring
	 * match against loaded UGameplayEffect subclasses; errors if ambiguous or
	 * not loaded). Returns nullptr with OutError set on failure.
	 */
	TSubclassOf<UGameplayEffect> ResolveEffectClass(const TSharedPtr<FJsonObject>& Arguments, FString& OutError);

	/**
	 * Resolve a UGameplayAbility subclass from `ability_class_path` or
	 * `ability_name` (same rules as ResolveEffectClass). Returns nullptr with
	 * OutError set on failure.
	 */
	TSubclassOf<UGameplayAbility> ResolveAbilityClass(const TSharedPtr<FJsonObject>& Arguments, FString& OutError);

	/**
	 * Match a (possibly partial) attribute name against the ASC's attribute
	 * list. Prefers an exact case-insensitive name match, then a unique
	 * substring match. Returns false with OutError set when nothing matches or
	 * the substring is ambiguous.
	 */
	bool ResolveAttribute(UAbilitySystemComponent* ASC, const FString& Name, FGameplayAttribute& OutAttr, FString& OutError);

	/** Human-facing "Set.Name" label for an attribute (Set omitted if unknown). */
	FString AttributeDisplayName(const FGameplayAttribute& Attr);
}
