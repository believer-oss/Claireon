// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
// Test-only UCLASS fixtures for ClaireonPropertyUtils unit tests. Lives under
// Private/Tests so the types are not part of the module's public surface.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "Engine/DataAsset.h"
#include "Engine/DeveloperSettings.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "ClaireonTestTypes.generated.h"

// Holder UCLASS used to verify CreateInstancedArrayElement rejects an array
// whose Inner FObjectProperty does not carry CPF_InstancedReference. Kept
// minimal and game-module-free so the test stays inside Claireon's module
// dependency surface.
UCLASS()
class UClaireonTestNonInstancedHolder : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<UObject*> NonInstancedArray;
};

// Minimal DataAsset fixture for the data_asset tool spec. A UPrimaryDataAsset subclass
// with a string + soft-object-ref property, so create / set-property paths can be
// exercised without depending on any game class. Kept game-module-free and inside Claireon's
// module surface.
UCLASS()
class UClaireonSpecDataAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "Test")
	FString Description;

	UPROPERTY(EditAnywhere, Category = "Test")
	TSoftObjectPtr<UObject> Speaker;
};

// Minimal developer-settings fixture for the data_asset dev-settings tool spec. A
// UDeveloperSettings subclass with config section "Game" and one config property, so the
// dev_settings get path can be exercised without depending on any game settings class.
UCLASS(Config=Game)
class UClaireonSpecDeveloperSettings : public UDeveloperSettings
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Config, Category = "Test")
	int32 SampleSetting = 0;
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

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FClaireonTestAsyncCompleted);

// ---- Instanced-array fixtures (append_cdo_array_instanced tests) ----
UCLASS()
class UClaireonAppendInstancedElementBase : public UObject { GENERATED_BODY() };

UCLASS()
class UClaireonAppendInstancedElement : public UClaireonAppendInstancedElementBase { GENERATED_BODY() };

UCLASS()
class UClaireonAppendInstancedBase : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Instanced, Category = "Test")
	TArray<UClaireonAppendInstancedElementBase*> Collectors;

	UPROPERTY(EditAnywhere, Category = "Test")
	bool bIgnoreSelf = false;
};

// ---- Instanced-slot fixtures (set_blueprint_cdo_property instanced tests) ----
UCLASS()
class UClaireonInstancedSlotValue : public UObject { GENERATED_BODY() };

UCLASS()
class UClaireonInstancedSlotHolder : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Instanced, Category = "Test")
	UObject* DefaultTargetingInstance = nullptr;
};

// ---- Function-override fixture (add_function_override tests) ----
// BlueprintImplementableEvent so the fixture stays header-only (no _Implementation needed)
// while still being overridable by add_function_override.
UCLASS()
class AClaireonFunctionOverrideFixtureActor : public AActor
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintImplementableEvent, Category = "Test")
	void SelectDropLocation();

	UFUNCTION(BlueprintImplementableEvent, Category = "Test")
	void GetRewardData();

	UFUNCTION(BlueprintImplementableEvent, Category = "Test")
	void ChooseStrategyForSpawner();
};

// ---- Async-action fixture (apply_blueprint_delta async-node tests) ----
UCLASS()
class UClaireonTestAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable)
	FClaireonTestAsyncCompleted OnComplete;

	UFUNCTION(BlueprintCallable, meta = (BlueprintInternalUseOnly = "true", WorldContext = "WorldContextObject"))
	static UClaireonTestAsyncAction* ClaireonTestAsyncDelay(UObject* WorldContextObject, float Duration);
};
