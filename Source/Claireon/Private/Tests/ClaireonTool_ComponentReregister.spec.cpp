// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonTool_ComponentReregister.h"
#include "Tools/IClaireonTool.h"
#include "ClaireonTestTypes.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"

namespace ClaireonToolComponentReregisterSpec
{
	// File-local named namespace (not anonymous): anonymous namespaces from
	// separate .cpp merge and collide under unity batching on v2.

	// Path-resolution constraint these fixtures work around:
	//
	// An object is reachable by path only if StaticFindObject can walk its outer
	// chain, which splits on '.'. The Untest world fixture names its package after
	// the test -- "/Untest/TestPackage_Claireon.ComponentReregister.<TestName>" --
	// so the PACKAGE name itself contains dots and nothing inside it can be found
	// by path. That is an artifact of the harness; a real caller passes an asset
	// path or a PIE path like "/Game/Maps/UEDPIE_0_Map.Map:PersistentLevel.Actor_1",
	// both of which parse fine.
	//
	// So: live registered components are created in the transient package and
	// registered with the test world (outer stays /Engine/Transient, path resolves),
	// and actor-shaped cases use the fixture CDO, whose path also resolves.

	IClaireonTool::FToolResult RunReregister(const TSharedPtr<FJsonObject>& Args)
	{
		ClaireonTool_ComponentReregister Tool;
		return Tool.Execute(Args);
	}

	TSharedPtr<FJsonObject> BuildArgs(const FString& ObjectPath)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("object_path"), ObjectPath);
		return Args;
	}

	/** A component that is genuinely registered with a world AND reachable by path. */
	UClaireonUObjectInspectComponent* MakeRegisteredComponent(UWorld* World)
	{
		if (!IsValid(World))
		{
			return nullptr;
		}
		UClaireonUObjectInspectComponent* Component =
			NewObject<UClaireonUObjectInspectComponent>(GetTransientPackage());
		Component->AddToRoot();
		Component->RegisterComponentWithWorld(World);
		return Component;
	}

	void ReleaseComponent(UClaireonUObjectInspectComponent* Component)
	{
		if (IsValid(Component))
		{
			if (Component->IsRegistered())
			{
				Component->UnregisterComponent();
			}
			Component->RemoveFromRoot();
		}
	}

	const TCHAR* kActorCdoPath = TEXT("/Script/Claireon.ClaireonUObjectInspectActorFixture");
}

// ---------------------------------------------------------------------------
// The cycle itself, on a live registered component.
// ---------------------------------------------------------------------------

UNTEST_WORLD(Claireon, ComponentReregister, ReregistersALiveComponent)
{
	using namespace ClaireonToolComponentReregisterSpec;

	UWorld* World = UNTEST_GET_WORLD();
	UNTEST_ASSERT_PTR(World);

	UClaireonUObjectInspectComponent* Component = MakeRegisteredComponent(World);
	UNTEST_ASSERT_PTR(Component);
	UNTEST_ASSERT_TRUE(Component->IsRegistered());

	IClaireonTool::FToolResult Result = RunReregister(BuildArgs(Component->GetPathName()));
	const bool bStillRegistered = Component->IsRegistered();
	ReleaseComponent(Component);

	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());
	UNTEST_EXPECT_TRUE(Result.Data->GetBoolField(TEXT("was_registered")));
	// The whole point: it comes back registered, not left detached.
	UNTEST_EXPECT_TRUE(Result.Data->GetBoolField(TEXT("is_registered")));
	UNTEST_EXPECT_TRUE(bStillRegistered);
	// A registered component has something to resync, so no "did nothing" warning.
	UNTEST_EXPECT_EQ(Result.Warnings.Num(), 0);

	co_return;
}

UNTEST_WORLD(Claireon, ComponentReregister, PairsPreAndPostSoASecondCallIsSafe)
{
	using namespace ClaireonToolComponentReregisterSpec;

	// UActorComponent::PreEditChange checkf()s when a previous pre had no matching
	// post. An unpaired call would not fail on the first call -- it would take the
	// editor down on the NEXT edit of this component. Calling twice is what pins
	// the RAII pairing.
	UWorld* World = UNTEST_GET_WORLD();
	UNTEST_ASSERT_PTR(World);

	UClaireonUObjectInspectComponent* Component = MakeRegisteredComponent(World);
	UNTEST_ASSERT_PTR(Component);

	const FString ComponentPath = Component->GetPathName();

	IClaireonTool::FToolResult First = RunReregister(BuildArgs(ComponentPath));
	IClaireonTool::FToolResult Second = RunReregister(BuildArgs(ComponentPath));
	const bool bStillRegistered = Component->IsRegistered();
	ReleaseComponent(Component);

	UNTEST_ASSERT_FALSE(First.bIsError);
	UNTEST_ASSERT_FALSE(Second.bIsError);
	UNTEST_EXPECT_TRUE(bStillRegistered);

	co_return;
}

UNTEST_WORLD(Claireon, ComponentReregister, AcceptsNavPropertyByName)
{
	using namespace ClaireonToolComponentReregisterSpec;

	// bCanEverAffectNavigation is the property whose name ConsolidatedPostEditChange
	// dispatches on; naming it is what makes the nav path run at all.
	UWorld* World = UNTEST_GET_WORLD();
	UNTEST_ASSERT_PTR(World);

	UClaireonUObjectInspectComponent* Component = MakeRegisteredComponent(World);
	UNTEST_ASSERT_PTR(Component);

	TSharedPtr<FJsonObject> Args = BuildArgs(Component->GetPathName());
	Args->SetStringField(TEXT("changed_property"), TEXT("bCanEverAffectNavigation"));

	IClaireonTool::FToolResult Result = RunReregister(Args);
	const bool bStillRegistered = Component->IsRegistered();
	ReleaseComponent(Component);

	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());
	UNTEST_EXPECT_EQ(Result.Data->GetStringField(TEXT("changed_property")),
		FString(TEXT("bCanEverAffectNavigation")));
	UNTEST_EXPECT_TRUE(bStillRegistered);
	// Naming a property means the caller is not being nudged toward one.
	UNTEST_EXPECT_FALSE(Result.Hint.IsValid());

	co_return;
}

UNTEST_WORLD(Claireon, ComponentReregister, LatchesTheMissingChangedPropertyHint)
{
	using namespace ClaireonToolComponentReregisterSpec;

	// Success-path guidance must be latched, or a bulk repair loop over many
	// components repeats the same lesson on every call -- the noise profile that
	// forced a latch onto the get_editor_property nudge.
	UWorld* World = UNTEST_GET_WORLD();
	UNTEST_ASSERT_PTR(World);

	UClaireonUObjectInspectComponent* Component = MakeRegisteredComponent(World);
	UNTEST_ASSERT_PTR(Component);

	IClaireonTool::ResetHintLatchForTests();

	const FString ComponentPath = Component->GetPathName();
	IClaireonTool::FToolResult First = RunReregister(BuildArgs(ComponentPath));
	IClaireonTool::FToolResult Second = RunReregister(BuildArgs(ComponentPath));

	ReleaseComponent(Component);
	IClaireonTool::ResetHintLatchForTests();

	UNTEST_ASSERT_FALSE(First.bIsError);
	UNTEST_ASSERT_TRUE(First.Hint.IsValid());
	FString HintError;
	UNTEST_EXPECT_TRUE(IClaireonTool::ValidateHint(First.Hint, HintError));
	UNTEST_EXPECT_EQ(First.Hint->GetStringField(TEXT("tool")), FString(TEXT("component_reregister")));

	UNTEST_ASSERT_FALSE(Second.bIsError);
	UNTEST_EXPECT_FALSE(Second.Hint.IsValid());

	co_return;
}

// ---------------------------------------------------------------------------
// Target resolution.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, ComponentReregister, ResolvesComponentByNameOnAnActor, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolComponentReregisterSpec;

	AClaireonUObjectInspectActorFixture* CDO =
		GetMutableDefault<AClaireonUObjectInspectActorFixture>();
	UNTEST_ASSERT_PTR(CDO);
	UNTEST_ASSERT_PTR(CDO->MyComp);

	TSharedPtr<FJsonObject> Args = BuildArgs(kActorCdoPath);
	Args->SetStringField(TEXT("component_name"), CDO->MyComp->GetName());

	IClaireonTool::FToolResult Result = RunReregister(Args);

	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());
	UNTEST_EXPECT_EQ(Result.Data->GetStringField(TEXT("component_path")),
		CDO->MyComp->GetPathName());

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ComponentReregister, ErrorsOnActorWithoutComponentNameAndListsCandidates, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolComponentReregisterSpec;

	AClaireonUObjectInspectActorFixture* CDO =
		GetMutableDefault<AClaireonUObjectInspectActorFixture>();
	UNTEST_ASSERT_PTR(CDO);
	UNTEST_ASSERT_PTR(CDO->MyComp);

	IClaireonTool::FToolResult Result = RunReregister(BuildArgs(kActorCdoPath));

	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("component_name")));
	// An error that names the candidates is one the caller can act on.
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(CDO->MyComp->GetName()));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ComponentReregister, ErrorsOnUnknownComponentName, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolComponentReregisterSpec;

	TSharedPtr<FJsonObject> Args = BuildArgs(kActorCdoPath);
	Args->SetStringField(TEXT("component_name"), TEXT("NoSuchComponent"));

	IClaireonTool::FToolResult Result = RunReregister(Args);

	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("NoSuchComponent")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ComponentReregister, ErrorsOnUnknownChangedProperty, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolComponentReregisterSpec;

	// A typo'd property name must not read as a clean success: the cycle would run
	// but the handler the caller wanted would silently not.
	AClaireonUObjectInspectActorFixture* CDO =
		GetMutableDefault<AClaireonUObjectInspectActorFixture>();
	UNTEST_ASSERT_PTR(CDO);
	UNTEST_ASSERT_PTR(CDO->MyComp);

	TSharedPtr<FJsonObject> Args = BuildArgs(CDO->MyComp->GetPathName());
	Args->SetStringField(TEXT("changed_property"), TEXT("bCanEverAffectNavigashun"));

	IClaireonTool::FToolResult Result = RunReregister(Args);

	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("bCanEverAffectNavigashun")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ComponentReregister, ErrorsOnNonComponentObject, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolComponentReregisterSpec;

	UClaireonUObjectInspectFixture* Fixture =
		NewObject<UClaireonUObjectInspectFixture>(GetTransientPackage());
	Fixture->AddToRoot();

	IClaireonTool::FToolResult Result = RunReregister(BuildArgs(Fixture->GetPathName()));

	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("actor component")));

	Fixture->RemoveFromRoot();
	co_return;
}

// ---------------------------------------------------------------------------
// Honesty about what did not happen.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, ComponentReregister, WarnsWhenComponentWasNotRegistered, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonToolComponentReregisterSpec;

	// A CDO's default subobject is never registered, so there is no reregister context
	// to take and the nav handler's registered-only path does not run. Reporting a
	// clean success there would be a lie.
	AClaireonUObjectInspectActorFixture* CDO =
		GetMutableDefault<AClaireonUObjectInspectActorFixture>();
	UNTEST_ASSERT_PTR(CDO);
	UNTEST_ASSERT_PTR(CDO->MyComp);
	UNTEST_ASSERT_FALSE(CDO->MyComp->IsRegistered());

	IClaireonTool::FToolResult Result = RunReregister(BuildArgs(CDO->MyComp->GetPathName()));

	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_EXPECT_FALSE(Result.Data->GetBoolField(TEXT("was_registered")));
	UNTEST_EXPECT_TRUE(Result.Warnings.Num() > 0);

	co_return;
}

#endif // WITH_UNTESTED
