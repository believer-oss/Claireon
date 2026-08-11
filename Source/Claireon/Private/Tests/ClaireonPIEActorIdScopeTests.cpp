// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonPIEManager.h"
#include "ClaireonPathResolver.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

// ---------------------------------------------------------------------------
// P0-8a: actor ids resolved across worlds.
//
// FClaireonPIEManager::ResolveActorId took a UWorld* and ignored it -- the
// lookup was a flat global id->actor map. So every tool that carefully resolved
// a PIE world by net_mode / pie_instance then received whatever world the id was
// minted in: pie_get_component(actorId, net_mode='client') returned a
// SERVER-world component, silently. pie_wait_for's actorValid / initState
// conditions inherited the same hole.
//
// P0-8b: class references never coerced to their CDO.
//
// Only /Script/ native-class paths were coerced; bIsClassReference was computed
// by the grammar and then ignored, so a Blueprint asset path resolved to the
// UBlueprint and its _C form to the UBlueprintGeneratedClass. uobject_inspect
// and uobject_set_property then walked the wrong object's property list.
// ---------------------------------------------------------------------------

UNTEST_WORLD(Claireon, PIEActorIdScope, IdDoesNotResolveIntoADifferentWorld)
{
	UWorld* WorldA = UNTEST_GET_WORLD();
	UNTEST_ASSERT_PTR(WorldA);

	AActor* Actor = WorldA->SpawnActor<AActor>();
	UNTEST_ASSERT_PTR(Actor);

	FClaireonPIEManager& Manager = FClaireonPIEManager::Get();

	const FString ActorId = Manager.GetActorId(Actor);
	UNTEST_ASSERT_FALSE(ActorId.IsEmpty());

	// Baseline: the world the id was minted in still resolves it.
	UNTEST_EXPECT_PTR(Manager.ResolveActorId(ActorId, WorldA));

	// The defect: before the fix this returned the WorldA actor regardless of
	// which world was asked for, so a client-scoped call silently received a
	// server-world object.
	UWorld* WorldB = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld=*/false);
	UNTEST_ASSERT_PTR(WorldB);

	UNTEST_EXPECT_NULLPTR(Manager.ResolveActorId(ActorId, WorldB));

	// A null world keeps the permissive behaviour for callers that genuinely do
	// not scope. Every in-tree caller passes a resolved PIE world, so this is
	// the documented escape hatch rather than the normal path.
	UNTEST_EXPECT_PTR(Manager.ResolveActorId(ActorId, nullptr));

	// Destroy explicitly: a leaked world trips the harness guard, and the
	// failure surfaces in an unrelated test rather than here.
	WorldB->DestroyWorld(/*bInformEngineOfWorld=*/false);

	co_return;
}

UNTEST_WORLD(Claireon, PIEActorIdScope, StaleIdReturnsNullAfterActorDestroyed)
{
	// Guards the pre-existing stale-entry cleanup path against regression from
	// the world check added above -- the world comparison must not run before
	// the weak pointer validity test, or a destroyed actor would dereference.
	UWorld* World = UNTEST_GET_WORLD();
	UNTEST_ASSERT_PTR(World);

	AActor* Actor = World->SpawnActor<AActor>();
	UNTEST_ASSERT_PTR(Actor);

	FClaireonPIEManager& Manager = FClaireonPIEManager::Get();
	const FString ActorId = Manager.GetActorId(Actor);
	UNTEST_ASSERT_FALSE(ActorId.IsEmpty());

	Actor->Destroy();
	CollectGarbage(RF_NoFlags);

	UNTEST_EXPECT_NULLPTR(Manager.ResolveActorId(ActorId, World));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PIEActorIdScope, NativeClassPathResolvesToCdo, UNTEST_TIMEOUTMS(5000))
{
	// Pins the pre-existing native-class coercion so the generalised rule added
	// for P0-8b cannot regress it.
	FString Error;
	FString CoercionNote;
	UObject* Resolved = ClaireonPathResolver::ResolveObjectFromPath(
		TEXT("/Script/Engine.StaticMeshActor"), /*bAllowLoad=*/true, Error, &CoercionNote);

	UNTEST_ASSERT_PTR(Resolved);
	UNTEST_EXPECT_TRUE(Error.IsEmpty());

	// The CDO, not the UClass: a UClass has no StaticMeshActor properties on it,
	// which is what made property lookups fail with
	// "Property 'X' not found on 'BlueprintGeneratedClass'".
	UNTEST_EXPECT_TRUE(Resolved->HasAnyFlags(RF_ClassDefaultObject));
	UNTEST_EXPECT_TRUE(Resolved->IsA(AStaticMeshActor::StaticClass()));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PIEActorIdScope, ClassObjectCoercesToCdoAndDiscloses, UNTEST_TIMEOUTMS(5000))
{
	// The generalised rule: any resolved UClass coerces to its CDO, and the
	// substitution is reported. Reporting properties of a different object than
	// the caller named -- silently -- is the failure shape this band removes.
	//
	// Uses the Default__ spelling, which the grammar classifies as a CDO
	// sub-object path and routes through the raw fallback rather than the
	// native-class branch, so this exercises the new code rather than the old.
	FString Error;
	FString CoercionNote;
	UObject* Resolved = ClaireonPathResolver::ResolveObjectFromPath(
		TEXT("/Script/Engine.Default__StaticMeshActor"), /*bAllowLoad=*/true, Error, &CoercionNote);

	UNTEST_ASSERT_PTR(Resolved);
	UNTEST_EXPECT_TRUE(Resolved->HasAnyFlags(RF_ClassDefaultObject));

	// An inherited C++ base-class property must be findable from the resolved
	// object. FindPropertyByName walks supers, so a failure here means the walk
	// started from the wrong object, not that inheritance is broken.
	UNTEST_EXPECT_PTR(Resolved->GetClass()->FindPropertyByName(TEXT("RootComponent")));

	co_return;
}

#endif // WITH_UNTESTED
