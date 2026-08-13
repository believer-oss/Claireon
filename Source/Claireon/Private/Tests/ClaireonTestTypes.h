// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
// Test-only UCLASS fixtures for ClaireonPropertyUtils unit tests. Lives under
// Private/Tests so the types are not part of the module's public surface.
#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Object.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "Engine/DataAsset.h"
#include "Engine/DeveloperSettings.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "GameplayTask.h"
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

	// Hard counterpart to Speaker, so the object-ref canonicalization contract can be
	// pinned for both soft and hard refs against the same fixture.
	UPROPERTY(EditAnywhere, Category = "Test")
	TObjectPtr<UObject> HardSpeaker;
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

	/** FInstancedStruct unwrap fixture (P2-18): reflection sees no payload
	 *  fields, so the reader must go through GetScriptStruct()/GetMemory(). */
	UPROPERTY()
	FInstancedStruct Wrapped;

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

// ---- Native-event fixture (add_function_override parent-body warning tests) ----
// Deliberately BlueprintNativeEvent, not BlueprintImplementableEvent: a BNE carries
// FUNC_Native and a companion _Implementation, which is exactly the "parent has a
// body an event override would shadow" case. The sibling
// AClaireonFunctionOverrideFixtureActor stays BIE-only for the no-warning case.
UCLASS()
class AClaireonNativeEventOverrideFixtureActor : public AActor
{
	GENERATED_BODY()
public:
	/** Non-trivial _Implementation body lives in ClaireonTestTypes.cpp. */
	UFUNCTION(BlueprintNativeEvent, Category = "Test")
	void ApplyNativeDefault();

	/** BNE with a return value, so the override lands as a function graph. */
	UFUNCTION(BlueprintNativeEvent, Category = "Test")
	int32 ComputeNativeValue();

	UPROPERTY()
	int32 NativeCounter = 0;
};

// ---- Editability fixture (bp_set_property gate tests, P1-8) ----
// One property per outcome of the shared DescribeEditorAccess rule, so the gate
// can be tested without depending on which specifiers some engine class happens
// to use this version. bp_set_property writes CDOs and SCS templates, i.e. the
// DEFAULTS context -- so EditDefaultsOnly must be writable through it. The old
// gate rejected exactly that set.
UCLASS()
class AClaireonEditabilityFixtureActor : public AActor
{
	GENERATED_BODY()
public:
	/** DescribeEditorAccess -> "edit". Writable. */
	UPROPERTY(EditDefaultsOnly, Category = "Test")
	bool bEditDefaultsOnlyFlag = false;

	/** DescribeEditorAccess -> "edit". Writable. */
	UPROPERTY(EditAnywhere, Category = "Test")
	int32 EditAnywhereNumber = 0;

	/** DescribeEditorAccess -> "edit_const". Refused without allow_non_editable. */
	UPROPERTY(VisibleAnywhere, Category = "Test")
	int32 VisibleOnlyNumber = 0;
};

// ---- CDO fixture (bp_set_cdo_property tests) ----
// The set_blueprint_cdo_property suite needs a Blueprint whose CDO carries several
// specific shapes: a TArray<struct> with at least one element and a writable primitive
// member (to pin the "<array>[0].<Member>" path the tool builds), a top-level
// user-editable bool, and a TArray<FName> with at least three elements.
//
// Every one of those used to be DISCOVERED by scanning the first 50-100 project
// Blueprints. That was both order-dependent (the window's contents shift as tests
// create fixtures, so whether a qualifying Blueprint appeared was luck) and invasive --
// the discovered asset belongs to the user, and the tests wrote to its CDO and cleared
// its package dirty flag. Parent a throwaway Blueprint to this class instead: the
// constructor populates everything, so the CDO always qualifies.
//
// Only Blueprint-level constructs (SCS components, a child Blueprint) are still built
// test-side, in ClaireonTool_SetBlueprintCDOPropertyTests.cpp -- a native class cannot
// carry an SCS node.
USTRUCT()
struct FClaireonTestCDOStructElem
{
	GENERATED_BODY()

	// One member per exact-comparable primitive kind the test accepts. Float/double are
	// deliberately absent: their ExportText form is not byte-comparable on read-back.
	UPROPERTY(EditAnywhere, Category = "Test")
	bool bFlag = false;

	UPROPERTY(EditAnywhere, Category = "Test")
	int32 Count = 0;

	UPROPERTY(EditAnywhere, Category = "Test")
	FName Tag;

	UPROPERTY(EditAnywhere, Category = "Test")
	FString Label;
};

UCLASS()
class AClaireonTestCDOStructArrayActor : public AActor
{
	GENERATED_BODY()
public:
	AClaireonTestCDOStructArrayActor()
	{
		// At least one element so FScriptArrayHelper::Num() >= 1 on the CDO.
		FClaireonTestCDOStructElem Elem;
		Elem.bFlag = false;
		Elem.Count = 0;
		Elem.Tag = FName(TEXT("ClaireonTestTag"));
		Elem.Label = TEXT("ClaireonTestValue");
		StructArray.Add(Elem);

		// PrimitiveArrayLeaf_WriteByIndex writes element [2] and then asserts [0] and
		// [1] are untouched, so three is the minimum count that makes it mean anything.
		// Distinct values so a write that hit the wrong index is detectable.
		NameArray.Add(FName(TEXT("ClaireonTestName0")));
		NameArray.Add(FName(TEXT("ClaireonTestName1")));
		NameArray.Add(FName(TEXT("ClaireonTestName2")));
	}

	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<FClaireonTestCDOStructElem> StructArray;

	// Top-level user-editable bool for FindBlueprintWithBoolCDOProperty.
	// EditAnywhere (not VisibleAnywhere) so the property carries CPF_Edit, which is what
	// that helper filters on -- only CPF_Edit properties participate reliably in the
	// editor's Modify/undo flow. Deliberately the ONLY bool declared directly on this
	// class, so the helper can NAME it instead of taking whatever a TFieldIterator
	// happens to hand back first (which would otherwise include AActor's own bools,
	// whose order is a UHT layout detail).
	UPROPERTY(EditAnywhere, Category = "Test")
	bool bClaireonTestBool = false;

	// TArray<FName> for PrimitiveArrayLeaf_WriteByIndex. FName rather than FString
	// because ClaireonPropertyUtils::ReadPropertyByPath exports with PPF_None, so FName
	// round-trips undelimited and the read-back compares exactly.
	UPROPERTY(EditAnywhere, Category = "Test")
	TArray<FName> NameArray;
};

// ---- GameplayTask fixture (latent-node alias / CallFunction promotion tests) ----
// UClaireonTestAsyncAction below is a UBlueprintAsyncActionBase and therefore cannot
// exercise the LatentGameplayTaskCall branch. This one derives from UGameplayTask so
// PickK2NodeClassForFunction's UGameplayTask check has something real to promote.
// Deliberately NOT a UAbilityTask: constructing one standalone wants a live
// UAbilitySystemComponent, and the UGameplayTask base is enough to cover the split's
// "other UGameplayTask" side.
UCLASS()
class UClaireonTestGameplayTask : public UGameplayTask
{
	GENERATED_BODY()
public:
	/** Latent factory: return type drives the node-class promotion. */
	UFUNCTION(BlueprintCallable, Category = "Test", meta = (BlueprintInternalUseOnly = "true"))
	static UClaireonTestGameplayTask* ClaireonTestWaitForThing(UObject* WorldContextObject, float Duration);

	/** Spawn-flavored latent factory. The TSubclassOf parameter named 'Class'
	 *  gives K2Node_LatentGameplayTaskCall its Class pin, which is what drives
	 *  the class-pin -> spawn-param-pin refresh under test (the BeginSpawningActor
	 *  pair is a compile-time concern only, so this fixture omits it). Never
	 *  executed at runtime. */
	UFUNCTION(BlueprintCallable, Category = "Test", meta = (BlueprintInternalUseOnly = "true"))
	static UClaireonTestGameplayTask* ClaireonTestSpawnThing(UObject* WorldContextObject, TSubclassOf<AActor> Class);
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
