// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
// Test-only UCLASS fixtures for ClaireonPropertyUtils unit tests. Lives under
// Private/Tests so the types are not part of the module's public surface.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "ClaireonTestTypes.generated.h"

// Holder UCLASS used to verify CreateInstancedArrayElement rejects an array
// whose Inner FObjectProperty does not carry CPF_InstancedReference. Kept
// minimal and free of project-module dependencies so the test stays inside Claireon's module
// dependency surface.
UCLASS()
class UClaireonTestNonInstancedHolder : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<UObject*> NonInstancedArray;
};

USTRUCT()
struct FClaireonUObjectInspectNested
{
	GENERATED_BODY()

	UPROPERTY()
	int32 X = 0;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FClaireonUObjectInspectMulticast, int32, Value);

UCLASS()
class UClaireonUObjectInspectComponent : public USceneComponent
{
	GENERATED_BODY()

public:
	UPROPERTY()
	int32 SomeField = 42;
};

UCLASS()
class UClaireonUObjectInspectFixture : public UObject
{
	GENERATED_BODY()

public:
	/** BlueprintReadOnly target for spec case 4. */
	UPROPERTY(BlueprintReadOnly)
	int32 BpReadOnly = 7;

	/** Plain UPROPERTY (no BP specifier) for spec case 5. */
	UPROPERTY()
	int32 Plain = 11;

	/** TArray<int32> for spec case 8 (array indexing). */
	UPROPERTY()
	TArray<int32> Numbers;

	/** Nested struct for spec case 9. */
	UPROPERTY()
	FClaireonUObjectInspectNested Foo;

	/** Transient field for spec case 11. */
	UPROPERTY(Transient)
	int32 TransientField = 99;

	/** Multicast delegate for spec case 12. */
	UPROPERTY(BlueprintAssignable)
	FClaireonUObjectInspectMulticast OnSomething;

protected:
	/** Protected field for spec case 6. */
	UPROPERTY()
	int32 ProtectedField = 21;

private:
	/** Private field for spec case 7. */
	UPROPERTY()
	int32 PrivateField = 33;
};

UCLASS()
class AClaireonUObjectInspectActorFixture : public AActor
{
	GENERATED_BODY()

public:
	AClaireonUObjectInspectActorFixture();

	UPROPERTY()
	UClaireonUObjectInspectComponent* MyComp;
};

inline AClaireonUObjectInspectActorFixture::AClaireonUObjectInspectActorFixture()
{
	MyComp = CreateDefaultSubobject<UClaireonUObjectInspectComponent>(TEXT("MyComp"));
	RootComponent = MyComp;
}
