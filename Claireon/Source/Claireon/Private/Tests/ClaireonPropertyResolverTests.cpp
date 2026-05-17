// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonPropertyResolver.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ScopedTransaction.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace ClaireonPropertyResolverTestsHelpers
{

AActor* FindTestActorWithComponents()
{
	if (!GEditor) return nullptr;
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) return nullptr;

	// Find any actor that has a root component and at least one other component
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || !Actor->GetRootComponent()) continue;
		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		if (Components.Num() >= 2) return Actor;
	}
	return nullptr;
}

UBlueprint* LoadBlueprintWithSCSComponents()
{
	UClass* BPClass = UBlueprint::StaticClass();
	TArray<FAssetData> Assets = ClaireonAssetUtils::FindAssetsByClass(BPClass, TEXT(""), 50);
	for (const FAssetData& Asset : Assets)
	{
		FString Error;
		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Asset.GetObjectPathString());
		if (!BP || !BP->SimpleConstructionScript) continue;
		if (BP->SimpleConstructionScript->GetAllNodes().Num() > 0)
		{
			return BP;
		}
	}
	return nullptr;
}

}  // namespace ClaireonPropertyResolverTestsHelpers

// ---------------------------------------------------------------------------
// ResolvePropertyOnActor tests
// ---------------------------------------------------------------------------

UNTEST_UNIT(Claireon, PropertyResolver_Actor, RootProperty)
{
	if (!GEditor) co_return;
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) co_return;

	AActor* Actor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (*It) { Actor = *It; break; }
	}
	UNTEST_ASSERT_PTR(Actor);

	ClaireonPropertyResolver::FResolvedProperty Resolved;
	FString Error;
	bool bOk = ClaireonPropertyResolver::ResolvePropertyOnActor(Actor, TEXT("bHidden"), Resolved, Error);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_EXPECT_STREQ(*Resolved.ResolvedOn, TEXT("Actor"));
	UNTEST_EXPECT_STREQ(*Resolved.RemainingPath, TEXT("bHidden"));
	UNTEST_EXPECT_EQ(Resolved.TargetObject, (UObject*)Actor);
	co_return;
}

UNTEST_UNIT(Claireon, PropertyResolver_Actor, RootComponentFallback)
{
	AActor* Actor = ClaireonPropertyResolverTestsHelpers::FindTestActorWithComponents();
	if (!Actor) co_return; // skip gracefully

	ClaireonPropertyResolver::FResolvedProperty Resolved;
	FString Error;
	bool bOk = ClaireonPropertyResolver::ResolvePropertyOnActor(Actor, TEXT("RelativeLocation"), Resolved, Error);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_EXPECT_STREQ(*Resolved.ResolvedOn, TEXT("RootComponent"));
	UNTEST_EXPECT_EQ(Resolved.TargetObject, (UObject*)Actor->GetRootComponent());
	UNTEST_EXPECT_TRUE(Resolved.Note.Contains(TEXT("RootComponent")));
	co_return;
}

UNTEST_UNIT(Claireon, PropertyResolver_Actor, ComponentFallback)
{
	if (!GEditor) co_return;
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) co_return;

	// Find an actor with a primitive component that has CastShadow
	AActor* TestActor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor) continue;
		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Comp : Components)
		{
			if (Comp && Comp != Actor->GetRootComponent() &&
				Comp->GetClass()->FindPropertyByName(FName(TEXT("CastShadow"))))
			{
				TestActor = Actor;
				break;
			}
		}
		if (TestActor) break;
	}
	if (!TestActor) co_return; // skip gracefully

	ClaireonPropertyResolver::FResolvedProperty Resolved;
	FString Error;
	bool bOk = ClaireonPropertyResolver::ResolvePropertyOnActor(TestActor, TEXT("CastShadow"), Resolved, Error);
	UNTEST_EXPECT_TRUE(bOk);
	if (bOk)
	{
		UNTEST_EXPECT_TRUE(Resolved.ResolvedOn != TEXT("Actor"));
		UNTEST_EXPECT_PTR(Cast<UActorComponent>(Resolved.TargetObject));
	}
	co_return;
}

UNTEST_UNIT(Claireon, PropertyResolver_Actor, ExplicitComponentPrefix)
{
	AActor* Actor = ClaireonPropertyResolverTestsHelpers::FindTestActorWithComponents();
	if (!Actor || !Actor->GetRootComponent()) co_return;

	FString CompName = Actor->GetRootComponent()->GetName();
	FString Path = CompName + TEXT(".Mobility");

	ClaireonPropertyResolver::FResolvedProperty Resolved;
	FString Error;
	bool bOk = ClaireonPropertyResolver::ResolvePropertyOnActor(Actor, Path, Resolved, Error);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_EXPECT_STREQ(*Resolved.TargetObject->GetName(), *Actor->GetRootComponent()->GetName());
	UNTEST_EXPECT_STREQ(*Resolved.RemainingPath, TEXT("Mobility"));
	co_return;
}

UNTEST_UNIT(Claireon, PropertyResolver_Actor, NotFound)
{
	if (!GEditor) co_return;
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) co_return;

	AActor* Actor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (*It) { Actor = *It; break; }
	}
	UNTEST_ASSERT_PTR(Actor);

	ClaireonPropertyResolver::FResolvedProperty Resolved;
	FString Error;
	bool bOk = ClaireonPropertyResolver::ResolvePropertyOnActor(Actor, TEXT("NonExistentProperty12345"), Resolved, Error);
	UNTEST_ASSERT_FALSE(bOk);
	UNTEST_EXPECT_FALSE(Error.IsEmpty());
	UNTEST_EXPECT_TRUE(Error.Contains(Actor->GetClass()->GetName()));
	co_return;
}

// ---------------------------------------------------------------------------
// ReadPropertyOnActor / WritePropertyOnActor tests
// ---------------------------------------------------------------------------

UNTEST_UNIT(Claireon, PropertyResolver_Actor, ReadViaFallback)
{
	AActor* Actor = ClaireonPropertyResolverTestsHelpers::FindTestActorWithComponents();
	if (!Actor) co_return;

	ClaireonPropertyResolver::FResolvedProperty Resolved;
	FString Error;
	FString Value = ClaireonPropertyResolver::ReadPropertyOnActor(Actor, TEXT("Mobility"), Resolved, Error);
	UNTEST_EXPECT_TRUE(Error.IsEmpty());
	UNTEST_EXPECT_FALSE(Value.IsEmpty());
	UNTEST_EXPECT_TRUE(Resolved.ResolvedOn == TEXT("RootComponent") || Resolved.ResolvedOn != TEXT("Actor"));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PropertyResolver_Actor, WriteReadRoundTrip, UNTEST_TIMEOUTMS(5000))
{
	if (!GEditor) co_return;
	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World) co_return;

	AActor* Actor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (*It) { Actor = *It; break; }
	}
	UNTEST_ASSERT_PTR(Actor);

	ClaireonPropertyResolver::FResolvedProperty ReadResolved1, WriteResolved, ReadResolved2, ReadResolved3;
	FString Error;

	// Read original
	FString Original = ClaireonPropertyResolver::ReadPropertyOnActor(Actor, TEXT("bHidden"), ReadResolved1, Error);
	UNTEST_ASSERT_TRUE(Error.IsEmpty());

	{
		FScopedTransaction Tx(FText::FromString(TEXT("Test")));
		Actor->Modify();
		bool bOk = ClaireonPropertyResolver::WritePropertyOnActor(Actor, TEXT("bHidden"), TEXT("true"), WriteResolved, Error);
		UNTEST_ASSERT_TRUE(bOk);

		FString NewVal = ClaireonPropertyResolver::ReadPropertyOnActor(Actor, TEXT("bHidden"), ReadResolved2, Error);
		UNTEST_EXPECT_STRCASEEQ(*NewVal, TEXT("True"));
	}

	GEditor->UndoTransaction();
	FString Restored = ClaireonPropertyResolver::ReadPropertyOnActor(Actor, TEXT("bHidden"), ReadResolved3, Error);
	UNTEST_EXPECT_STREQ(*Restored, *Original);
	co_return;
}

// ---------------------------------------------------------------------------
// ResolvePropertyOnBlueprintCDO tests
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, PropertyResolver_CDO, DirectProperty, UNTEST_TIMEOUTMS(5000))
{
	UBlueprint* Blueprint = ClaireonPropertyResolverTestsHelpers::LoadBlueprintWithSCSComponents();
	if (!Blueprint) co_return; // skip gracefully
	if (!Blueprint->GeneratedClass) co_return;

	UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
	UNTEST_ASSERT_PTR(CDO);

	ClaireonPropertyResolver::FResolvedProperty Resolved;
	FString Error;
	bool bOk = ClaireonPropertyResolver::ResolvePropertyOnBlueprintCDO(Blueprint, TEXT("bCanBeDamaged"), Resolved, Error);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_EXPECT_STREQ(*Resolved.ResolvedOn, TEXT("CDO"));
	UNTEST_EXPECT_EQ(Resolved.TargetObject, CDO);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PropertyResolver_CDO, SCSComponentTemplate, UNTEST_TIMEOUTMS(5000))
{
	UBlueprint* Blueprint = ClaireonPropertyResolverTestsHelpers::LoadBlueprintWithSCSComponents();
	if (!Blueprint || !Blueprint->SimpleConstructionScript) co_return;

	// Find a node with a component template that has CastShadow (UPrimitiveComponent)
	bool bHasSuitableNode = false;
	for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
	{
		if (!Node || !Node->ComponentTemplate) continue;
		if (Node->ComponentTemplate->GetClass()->FindPropertyByName(FName(TEXT("CastShadow"))))
		{
			bHasSuitableNode = true;
			break;
		}
	}
	if (!bHasSuitableNode) co_return; // skip gracefully

	ClaireonPropertyResolver::FResolvedProperty Resolved;
	FString Error;
	bool bOk = ClaireonPropertyResolver::ResolvePropertyOnBlueprintCDO(Blueprint, TEXT("CastShadow"), Resolved, Error);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_EXPECT_TRUE(Resolved.ResolvedOn != TEXT("CDO"));
	UNTEST_EXPECT_PTR(Resolved.TargetObject);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PropertyResolver_CDO, ExplicitSCSPrefix, UNTEST_TIMEOUTMS(5000))
{
	UBlueprint* Blueprint = ClaireonPropertyResolverTestsHelpers::LoadBlueprintWithSCSComponents();
	if (!Blueprint || !Blueprint->SimpleConstructionScript) co_return;

	// Find a scene component SCS node with Mobility
	USCS_Node* TargetNode = nullptr;
	for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
	{
		if (!Node || !Node->ComponentTemplate) continue;
		if (Node->ComponentTemplate->GetClass()->FindPropertyByName(FName(TEXT("Mobility"))))
		{
			TargetNode = Node;
			break;
		}
	}
	if (!TargetNode) co_return; // skip gracefully

	FString VarName = TargetNode->GetVariableName().ToString();
	FString Path = VarName + TEXT(".Mobility");

	ClaireonPropertyResolver::FResolvedProperty Resolved;
	FString Error;
	bool bOk = ClaireonPropertyResolver::ResolvePropertyOnBlueprintCDO(Blueprint, Path, Resolved, Error);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_EXPECT_STREQ(*Resolved.RemainingPath, TEXT("Mobility"));
	co_return;
}

#endif // WITH_UNTESTED
