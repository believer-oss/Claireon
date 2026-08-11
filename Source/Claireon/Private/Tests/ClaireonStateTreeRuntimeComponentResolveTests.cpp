// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// P1-1: statetree_runtime_inspect / statetree_runtime_send_event could not see
// UStateTreeAIComponent.
//
// Both tools matched on the class NAME containing "StateTreeComponent". The
// literal "StateTreeAIComponent" does not contain "StateTreeComponent" -- the
// "AI" splits the substring -- so the default match could never succeed for the
// one subclass every AI controller in this project actually uses. The resolver
// matches by type now.
//
// The components are created but deliberately NOT registered:
// UActorComponent::PostInitProperties already adds them to the owner's
// OwnedComponents (which is what GetComponents walks), and
// UStateTreeComponent::InitializeComponent logs an Error when no StateTree asset
// is set. Registration would buy nothing and cost a spurious error.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonStateTreeComponentResolver.h"
#include "Tools/ClaireonTool_StateTreeRuntimeSendEvent.h"
#include "Components/SceneComponent.h"
#include "Components/StateTreeAIComponent.h"
#include "Components/StateTreeComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

// File-local namespace (NOT raw `namespace { ... }`) to avoid unity-batched
// symbol collisions across other Tests TUs.
namespace ClaireonStateTreeResolveTests
{
AActor* SpawnBareActor(UWorld* World)
{
	if (World == nullptr)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
}
} // namespace ClaireonStateTreeResolveTests

// ---------------------------------------------------------------------------
// The regression: the AI subclass resolves with no override.
// ---------------------------------------------------------------------------

UNTEST_WORLD(Claireon, StateTreeComponentResolve, AIComponentResolvesWithNoOverride)
{
	using namespace ClaireonStateTreeResolveTests;

	UWorld* World = UNTEST_GET_WORLD();
	UNTEST_ASSERT_PTR(World);

	AActor* Actor = SpawnBareActor(World);
	UNTEST_ASSERT_PTR(Actor);

	UStateTreeAIComponent* AIComponent = NewObject<UStateTreeAIComponent>(Actor);
	UNTEST_ASSERT_PTR(AIComponent);

	UActorComponent* Resolved =
		ClaireonStateTreeComponentResolver::FindStateTreeComponent(Actor, FString());

	UNTEST_ASSERT_PTR(Resolved);
	UNTEST_ASSERT_TRUE(Resolved == AIComponent);
	co_return;
}

// ---------------------------------------------------------------------------
// The base class must keep resolving -- the fix is a widening, not a swap.
// ---------------------------------------------------------------------------

UNTEST_WORLD(Claireon, StateTreeComponentResolve, BaseComponentStillResolves)
{
	using namespace ClaireonStateTreeResolveTests;

	UWorld* World = UNTEST_GET_WORLD();
	UNTEST_ASSERT_PTR(World);

	AActor* Actor = SpawnBareActor(World);
	UNTEST_ASSERT_PTR(Actor);

	UStateTreeComponent* Component = NewObject<UStateTreeComponent>(Actor);
	UNTEST_ASSERT_PTR(Component);

	UActorComponent* Resolved =
		ClaireonStateTreeComponentResolver::FindStateTreeComponent(Actor, FString());

	UNTEST_ASSERT_PTR(Resolved);
	UNTEST_ASSERT_TRUE(Resolved == Component);
	co_return;
}

// ---------------------------------------------------------------------------
// component_class stays a working escape hatch, and stays a name substring so
// it can still address a component that is not a UStateTreeComponent.
// ---------------------------------------------------------------------------

UNTEST_WORLD(Claireon, StateTreeComponentResolve, ComponentClassOverrideStillMatchesByName)
{
	using namespace ClaireonStateTreeResolveTests;

	UWorld* World = UNTEST_GET_WORLD();
	UNTEST_ASSERT_PTR(World);

	AActor* Actor = SpawnBareActor(World);
	UNTEST_ASSERT_PTR(Actor);

	UStateTreeAIComponent* AIComponent = NewObject<UStateTreeAIComponent>(Actor);
	UNTEST_ASSERT_PTR(AIComponent);

	UActorComponent* Resolved =
		ClaireonStateTreeComponentResolver::FindStateTreeComponent(Actor, TEXT("StateTreeAIComponent"));

	UNTEST_ASSERT_PTR(Resolved);
	UNTEST_ASSERT_TRUE(Resolved == AIComponent);
	co_return;
}

// ---------------------------------------------------------------------------
// No State Tree component: nullptr, and the enumeration a caller needs to see
// why. Both tools put DescribeComponents into their not-found error.
// ---------------------------------------------------------------------------

UNTEST_WORLD(Claireon, StateTreeComponentResolve, NoComponentEnumeratesWhatWasThere)
{
	using namespace ClaireonStateTreeResolveTests;

	UWorld* World = UNTEST_GET_WORLD();
	UNTEST_ASSERT_PTR(World);

	AActor* Actor = SpawnBareActor(World);
	UNTEST_ASSERT_PTR(Actor);

	USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("ClaireonResolveTestRoot"));
	UNTEST_ASSERT_PTR(Root);

	UActorComponent* Resolved =
		ClaireonStateTreeComponentResolver::FindStateTreeComponent(Actor, FString());
	UNTEST_ASSERT_TRUE(Resolved == nullptr);

	const FString Description = ClaireonStateTreeComponentResolver::DescribeComponents(Actor);
	UNTEST_ASSERT_TRUE(Description.Contains(TEXT("ClaireonResolveTestRoot")));
	UNTEST_ASSERT_TRUE(Description.Contains(TEXT("SceneComponent")));
	co_return;
}

// ---------------------------------------------------------------------------
// send_event's schema must expose the same escape hatch inspect has. Without
// it, a mis-resolved component left that tool with no workaround at all.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, StateTreeComponentResolve, SendEventDeclaresComponentClass, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_StateTreeRuntimeSendEvent Tool;
	const TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_TRUE(Schema.IsValid());

	const TSharedPtr<FJsonObject>* Props = nullptr;
	UNTEST_ASSERT_TRUE(Schema->TryGetObjectField(TEXT("properties"), Props) && Props != nullptr);
	UNTEST_ASSERT_TRUE((*Props)->HasField(TEXT("component_class")));
	co_return;
}

#endif // WITH_UNTESTED
