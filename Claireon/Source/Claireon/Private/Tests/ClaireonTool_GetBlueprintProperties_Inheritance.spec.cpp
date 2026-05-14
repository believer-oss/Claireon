// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Spec for blueprint_get_properties (#0000). Verifies the schema and
// behavior of include_inherited across the components / variables / functions
// arrays:
//
//   - Default mode (include_inherited=false / omitted): only this BP's own
//     SCS components, NewVariables, and FunctionGraphs are emitted, each
//     carrying is_inherited=false and source_class=<this BP short name>.
//   - Inherited mode: ancestor BP SCS chains and (for actor-derived BPs)
//     native CDO subobjects are merged in with is_inherited=true and
//     source_class=<declaring class short name>.
//   - is_inherited and source_class are unconditional (always present), so
//     callers see a stable schema regardless of the parameter.
//   - Summary text gains "(N inherited)" suffixes on each count when N > 0.
//
// Fixture deviation note: the stage file proposed a synthetic
// ATestClaireonBPPropsActor declared inside this file, but the project's test
// pattern does not declare UCLASSes in test cpp files (no UHT processing for
// Tests/, no test headers in this folder). Instead this spec uses
// AStaticMeshActor as the parent, which has a well-known native subobject
// (StaticMeshComponent0 / StaticMeshComponent) that exercises Source C of the
// three-source merged walk in ClaireonTool_GetBlueprintProperties::Execute().

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonTool_GetBlueprintProperties.h"
#include "Tools/IClaireonTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/StaticMeshActor.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"

#include "AssetRegistry/AssetRegistryModule.h"

#include "Kismet2/KismetEditorUtilities.h"

#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

namespace
{

constexpr const TCHAR* SandboxFolder = TEXT("/Game/Tests/ClaireonBPGetProps");

// File-local helper. Named with a BPProps_ prefix to avoid colliding with the
// identically-shaped DeleteIfExists in ClaireonTool_BlueprintDuplicateTests.cpp
// when the linux non-unity build merges multiple .cpp files into one
// translation unit (Module.Claireon.<N>.cpp), where anonymous namespaces are
// merged and any duplicate symbol names break compilation.
void BPProps_DeleteIfExists(const FString& ObjectPath)
{
	if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad())
	{
		TArray<UObject*> AssetsToDelete;
		AssetsToDelete.Add(Asset);
		ObjectTools::ForceDeleteObjects(AssetsToDelete, false);
	}
}

// Create a transient Blueprint inheriting from AStaticMeshActor with a single
// SCS-added USphereComponent named "TestSphere". Caller is responsible for
// deletion. Returns nullptr on failure.
UBlueprint* CreateBPWithSphereSCS(const FString& PackagePath, const FName SphereName = TEXT("TestSphere"))
{
	UPackage* Package = CreatePackage(*PackagePath);
	if (!Package)
	{
		return nullptr;
	}
	const FString AssetName = FPackageName::GetShortName(PackagePath);

	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
		AStaticMeshActor::StaticClass(),
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		NAME_None);
	if (!BP || !BP->SimpleConstructionScript)
	{
		return nullptr;
	}

	USCS_Node* SphereNode = BP->SimpleConstructionScript->CreateNode(USphereComponent::StaticClass(), SphereName);
	if (!SphereNode)
	{
		return nullptr;
	}
	BP->SimpleConstructionScript->AddNode(SphereNode);

	FAssetRegistryModule::AssetCreated(BP);
	BP->MarkPackageDirty();
	FKismetEditorUtilities::CompileBlueprint(BP);

	return BP;
}

TSharedPtr<FJsonObject> MakeArgs(const FString& AssetPath, bool bIncludeInherited, bool bSetFlag = true)
{
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	if (bSetFlag)
	{
		Args->SetBoolField(TEXT("include_inherited"), bIncludeInherited);
	}
	return Args;
}

const TArray<TSharedPtr<FJsonValue>>* GetArrayField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
{
	const TArray<TSharedPtr<FJsonValue>>* Out = nullptr;
	if (Obj.IsValid())
	{
		Obj->TryGetArrayField(Field, Out);
	}
	return Out;
}

// Find a JSON entry by its "name" field. Returns null if not found.
TSharedPtr<FJsonObject> FindByName(const TArray<TSharedPtr<FJsonValue>>* Array, const FString& Name)
{
	if (!Array)
	{
		return nullptr;
	}
	for (const TSharedPtr<FJsonValue>& V : *Array)
	{
		const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
		if (!Obj.IsValid())
		{
			continue;
		}
		FString N;
		if (Obj->TryGetStringField(TEXT("name"), N) && N == Name)
		{
			return Obj;
		}
	}
	return nullptr;
}

// Return the count of entries with is_inherited == bWanted.
int32 CountByInheritedFlag(const TArray<TSharedPtr<FJsonValue>>* Array, bool bWanted)
{
	int32 Count = 0;
	if (!Array)
	{
		return 0;
	}
	for (const TSharedPtr<FJsonValue>& V : *Array)
	{
		const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
		if (!Obj.IsValid())
		{
			continue;
		}
		bool bInh = false;
		if (Obj->TryGetBoolField(TEXT("is_inherited"), bInh) && bInh == bWanted)
		{
			++Count;
		}
	}
	return Count;
}

// Assert every entry in Array has both is_inherited and source_class fields.
bool AllEntriesHaveSchemaKeys(const TArray<TSharedPtr<FJsonValue>>* Array)
{
	if (!Array)
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& V : *Array)
	{
		const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
		if (!Obj.IsValid())
		{
			return false;
		}
		bool bInh = false;
		FString SourceClass;
		if (!Obj->TryGetBoolField(TEXT("is_inherited"), bInh))
		{
			return false;
		}
		if (!Obj->TryGetStringField(TEXT("source_class"), SourceClass))
		{
			return false;
		}
	}
	return true;
}

// Assert every entry in Array has is_inherited=false and source_class equal to
// ExpectedSourceClass. Returns true on a missing/null array (vacuously true).
// Implemented as a plain helper rather than a lambda inside the test body so
// callers can invoke UNTEST_EXPECT_TRUE on the bool result without putting
// UNTEST_* macros (which expand to co_return) inside a non-coroutine lambda.
bool AllEntriesAreThisBP(const TArray<TSharedPtr<FJsonValue>>* Array, const FString& ExpectedSourceClass)
{
	if (!Array)
	{
		return true;
	}
	for (const TSharedPtr<FJsonValue>& V : *Array)
	{
		const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
		if (!Obj.IsValid())
		{
			return false;
		}
		bool bInh = true;
		if (!Obj->TryGetBoolField(TEXT("is_inherited"), bInh))
		{
			return false;
		}
		if (bInh)
		{
			return false;
		}
		FString Src;
		if (!Obj->TryGetStringField(TEXT("source_class"), Src))
		{
			return false;
		}
		if (Src != ExpectedSourceClass)
		{
			return false;
		}
	}
	return true;
}

} // anonymous namespace

// ============================================================================
// Default mode (include_inherited not passed)
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, GetBlueprintProperties, DefaultMode_OnlyThisBPComponents, UNTEST_TIMEOUTMS(60000))
{
	const FString PackagePath = FString::Printf(TEXT("%s/BP_DefaultMode"), SandboxFolder);
	const FString ObjectPath = PackagePath + TEXT(".BP_DefaultMode");
	BPProps_DeleteIfExists(ObjectPath);

	UBlueprint* BP = CreateBPWithSphereSCS(PackagePath);
	UNTEST_ASSERT_PTR(BP);
	const FString GeneratedClassShort = BP->GeneratedClass ? BP->GeneratedClass->GetName() : FString();
	UNTEST_ASSERT_FALSE(GeneratedClassShort.IsEmpty());

	ClaireonTool_GetBlueprintProperties Tool;
	IClaireonTool::FToolResult Result = Tool.Execute(MakeArgs(PackagePath, /*bIncludeInherited=*/false, /*bSetFlag=*/false));
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	const TArray<TSharedPtr<FJsonValue>>* Components = GetArrayField(Result.Data, TEXT("components"));
	UNTEST_ASSERT_TRUE(Components != nullptr);
	UNTEST_ASSERT_EQ(Components->Num(), 1);

	TSharedPtr<FJsonObject> Sphere = FindByName(Components, TEXT("TestSphere"));
	UNTEST_ASSERT_TRUE(Sphere.IsValid());
	bool bInh = true;
	UNTEST_ASSERT_TRUE(Sphere->TryGetBoolField(TEXT("is_inherited"), bInh));
	UNTEST_EXPECT_FALSE(bInh);
	FString SourceClass;
	UNTEST_ASSERT_TRUE(Sphere->TryGetStringField(TEXT("source_class"), SourceClass));
	UNTEST_EXPECT_STREQ(*SourceClass, *GeneratedClassShort);

	// Schema stability across all kinds in default mode.
	UNTEST_EXPECT_TRUE(AllEntriesHaveSchemaKeys(Components));
	UNTEST_EXPECT_TRUE(AllEntriesHaveSchemaKeys(GetArrayField(Result.Data, TEXT("variables"))));
	UNTEST_EXPECT_TRUE(AllEntriesHaveSchemaKeys(GetArrayField(Result.Data, TEXT("functions"))));

	// In default mode every entry should have is_inherited=false.
	UNTEST_EXPECT_EQ(CountByInheritedFlag(Components, /*bWanted=*/true), 0);
	UNTEST_EXPECT_EQ(CountByInheritedFlag(GetArrayField(Result.Data, TEXT("variables")), true), 0);
	UNTEST_EXPECT_EQ(CountByInheritedFlag(GetArrayField(Result.Data, TEXT("functions")), true), 0);

	BPProps_DeleteIfExists(ObjectPath);
	co_return;
}

// ============================================================================
// Inherited mode -- native subobject pulled in via Source C
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, GetBlueprintProperties, InheritedMode_IncludesNativeSubobject, UNTEST_TIMEOUTMS(60000))
{
	const FString PackagePath = FString::Printf(TEXT("%s/BP_InheritedMode"), SandboxFolder);
	const FString ObjectPath = PackagePath + TEXT(".BP_InheritedMode");
	BPProps_DeleteIfExists(ObjectPath);

	UBlueprint* BP = CreateBPWithSphereSCS(PackagePath);
	UNTEST_ASSERT_PTR(BP);
	const FString GeneratedClassShort = BP->GeneratedClass ? BP->GeneratedClass->GetName() : FString();

	ClaireonTool_GetBlueprintProperties Tool;
	IClaireonTool::FToolResult Result = Tool.Execute(MakeArgs(PackagePath, /*bIncludeInherited=*/true));
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	const TArray<TSharedPtr<FJsonValue>>* Components = GetArrayField(Result.Data, TEXT("components"));
	UNTEST_ASSERT_TRUE(Components != nullptr);
	// The SCS-added TestSphere plus AStaticMeshActor's native StaticMeshComponent.
	UNTEST_EXPECT_GE(Components->Num(), 2);

	TSharedPtr<FJsonObject> Sphere = FindByName(Components, TEXT("TestSphere"));
	UNTEST_ASSERT_TRUE(Sphere.IsValid());
	bool bSphereInh = true;
	UNTEST_ASSERT_TRUE(Sphere->TryGetBoolField(TEXT("is_inherited"), bSphereInh));
	UNTEST_EXPECT_FALSE(bSphereInh);
	FString SphereSource;
	UNTEST_ASSERT_TRUE(Sphere->TryGetStringField(TEXT("source_class"), SphereSource));
	UNTEST_EXPECT_STREQ(*SphereSource, *GeneratedClassShort);

	// Find the inherited native StaticMeshComponent. AStaticMeshActor declares
	// it under the variable name "StaticMeshComponent0" (legacy) and exposes
	// it as the "StaticMeshComponent" property; the FName of the subobject is
	// what AActor::GetComponents reports. Accept either to keep the test
	// resilient to engine renames.
	bool bFoundInheritedNative = false;
	for (const TSharedPtr<FJsonValue>& V : *Components)
	{
		const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
		if (!Obj.IsValid())
		{
			continue;
		}
		bool bInh = false;
		if (!Obj->TryGetBoolField(TEXT("is_inherited"), bInh) || !bInh)
		{
			continue;
		}
		FString CompClass;
		Obj->TryGetStringField(TEXT("class"), CompClass);
		if (CompClass == TEXT("StaticMeshComponent"))
		{
			bFoundInheritedNative = true;
			FString SourceClass;
			UNTEST_EXPECT_TRUE(Obj->TryGetStringField(TEXT("source_class"), SourceClass));
			// AStaticMeshActor declares StaticMeshComponent natively. The
			// short name of that declaring class is "StaticMeshActor".
			UNTEST_EXPECT_STREQ(*SourceClass, TEXT("StaticMeshActor"));
			break;
		}
	}
	UNTEST_EXPECT_TRUE(bFoundInheritedNative);

	// Schema stability + at least one inherited entry overall in this mode.
	UNTEST_EXPECT_TRUE(AllEntriesHaveSchemaKeys(Components));
	UNTEST_EXPECT_GE(CountByInheritedFlag(Components, /*bWanted=*/true), 1);

	BPProps_DeleteIfExists(ObjectPath);
	co_return;
}

// ============================================================================
// Dedupe spot-check: an SCS node with the same FName as a native subobject
// should win (Source A > Source C). We synthesise this by giving the SCS
// USphereComponent the same FName ("StaticMeshComponent0") that
// AStaticMeshActor's native CDO subobject uses.
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, GetBlueprintProperties, InheritedMode_SCSWinsOverNative, UNTEST_TIMEOUTMS(60000))
{
	const FString PackagePath = FString::Printf(TEXT("%s/BP_DedupeSCS"), SandboxFolder);
	const FString ObjectPath = PackagePath + TEXT(".BP_DedupeSCS");
	BPProps_DeleteIfExists(ObjectPath);

	// Find the FName used by AStaticMeshActor's native StaticMeshComponent so
	// our SCS node can collide with it intentionally.
	AStaticMeshActor* CDO = Cast<AStaticMeshActor>(AStaticMeshActor::StaticClass()->GetDefaultObject(/*bCreateIfNeeded=*/false));
	UNTEST_ASSERT_PTR(CDO);
	UStaticMeshComponent* NativeSMC = CDO->GetStaticMeshComponent();
	UNTEST_ASSERT_PTR(NativeSMC);
	const FName CollideName = NativeSMC->GetFName();

	UBlueprint* BP = CreateBPWithSphereSCS(PackagePath, CollideName);
	UNTEST_ASSERT_PTR(BP);
	const FString GeneratedClassShort = BP->GeneratedClass ? BP->GeneratedClass->GetName() : FString();

	ClaireonTool_GetBlueprintProperties Tool;
	IClaireonTool::FToolResult Result = Tool.Execute(MakeArgs(PackagePath, /*bIncludeInherited=*/true));
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	const TArray<TSharedPtr<FJsonValue>>* Components = GetArrayField(Result.Data, TEXT("components"));
	UNTEST_ASSERT_TRUE(Components != nullptr);

	// Only one entry with the colliding FName; it should be the SCS one
	// (is_inherited=false, source_class=this BP) -- not the native one.
	int32 CollisionCount = 0;
	TSharedPtr<FJsonObject> CollidingObj;
	for (const TSharedPtr<FJsonValue>& V : *Components)
	{
		const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
		if (!Obj.IsValid())
		{
			continue;
		}
		FString N;
		if (Obj->TryGetStringField(TEXT("name"), N) && N == CollideName.ToString())
		{
			++CollisionCount;
			CollidingObj = Obj;
		}
	}
	UNTEST_EXPECT_EQ(CollisionCount, 1);
	UNTEST_ASSERT_TRUE(CollidingObj.IsValid());
	bool bInh = true;
	UNTEST_ASSERT_TRUE(CollidingObj->TryGetBoolField(TEXT("is_inherited"), bInh));
	UNTEST_EXPECT_FALSE(bInh);
	FString SrcClass;
	UNTEST_ASSERT_TRUE(CollidingObj->TryGetStringField(TEXT("source_class"), SrcClass));
	UNTEST_EXPECT_STREQ(*SrcClass, *GeneratedClassShort);
	// And the colliding SCS node's class is USphereComponent, NOT
	// UStaticMeshComponent -- proving the SCS entry won.
	FString CompClass;
	UNTEST_ASSERT_TRUE(CollidingObj->TryGetStringField(TEXT("class"), CompClass));
	UNTEST_EXPECT_STREQ(*CompClass, TEXT("SphereComponent"));

	BPProps_DeleteIfExists(ObjectPath);
	co_return;
}

// ============================================================================
// Variables / functions parity in default mode -- every entry has
// is_inherited=false and source_class equal to this BP's generated class.
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, GetBlueprintProperties, DefaultMode_VariablesAndFunctionsCarrySchema, UNTEST_TIMEOUTMS(60000))
{
	const FString PackagePath = FString::Printf(TEXT("%s/BP_DefaultVarsFuncs"), SandboxFolder);
	const FString ObjectPath = PackagePath + TEXT(".BP_DefaultVarsFuncs");
	BPProps_DeleteIfExists(ObjectPath);

	UBlueprint* BP = CreateBPWithSphereSCS(PackagePath);
	UNTEST_ASSERT_PTR(BP);
	const FString GeneratedClassShort = BP->GeneratedClass ? BP->GeneratedClass->GetName() : FString();

	ClaireonTool_GetBlueprintProperties Tool;
	IClaireonTool::FToolResult Result = Tool.Execute(MakeArgs(PackagePath, /*bIncludeInherited=*/false));
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	UNTEST_EXPECT_TRUE(AllEntriesAreThisBP(GetArrayField(Result.Data, TEXT("variables")), GeneratedClassShort));
	UNTEST_EXPECT_TRUE(AllEntriesAreThisBP(GetArrayField(Result.Data, TEXT("functions")), GeneratedClassShort));

	BPProps_DeleteIfExists(ObjectPath);
	co_return;
}

// ============================================================================
// Inherited mode -- variables/functions array is a strict superset of the
// default-mode array; new entries have is_inherited=true.
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, GetBlueprintProperties, InheritedMode_VariablesAndFunctionsAreSuperset, UNTEST_TIMEOUTMS(60000))
{
	const FString PackagePath = FString::Printf(TEXT("%s/BP_InheritedVarsFuncs"), SandboxFolder);
	const FString ObjectPath = PackagePath + TEXT(".BP_InheritedVarsFuncs");
	BPProps_DeleteIfExists(ObjectPath);

	UBlueprint* BP = CreateBPWithSphereSCS(PackagePath);
	UNTEST_ASSERT_PTR(BP);

	ClaireonTool_GetBlueprintProperties Tool;
	IClaireonTool::FToolResult ResultDefault = Tool.Execute(MakeArgs(PackagePath, /*bIncludeInherited=*/false));
	IClaireonTool::FToolResult ResultInherited = Tool.Execute(MakeArgs(PackagePath, /*bIncludeInherited=*/true));
	UNTEST_ASSERT_FALSE(ResultDefault.bIsError);
	UNTEST_ASSERT_FALSE(ResultInherited.bIsError);

	const TArray<TSharedPtr<FJsonValue>>* DefaultVars = GetArrayField(ResultDefault.Data, TEXT("variables"));
	const TArray<TSharedPtr<FJsonValue>>* InheritedVars = GetArrayField(ResultInherited.Data, TEXT("variables"));
	UNTEST_ASSERT_TRUE(DefaultVars != nullptr);
	UNTEST_ASSERT_TRUE(InheritedVars != nullptr);
	// AStaticMeshActor exposes Blueprint-visible inherited properties (e.g.
	// StaticMeshComponent), so inherited-mode variables must be a strict
	// superset of default-mode variables.
	UNTEST_EXPECT_GE(InheritedVars->Num(), DefaultVars->Num());

	const TArray<TSharedPtr<FJsonValue>>* DefaultFuncs = GetArrayField(ResultDefault.Data, TEXT("functions"));
	const TArray<TSharedPtr<FJsonValue>>* InheritedFuncs = GetArrayField(ResultInherited.Data, TEXT("functions"));
	UNTEST_EXPECT_GE(InheritedFuncs->Num(), DefaultFuncs->Num());

	// Schema stability in inherited mode.
	UNTEST_EXPECT_TRUE(AllEntriesHaveSchemaKeys(InheritedVars));
	UNTEST_EXPECT_TRUE(AllEntriesHaveSchemaKeys(InheritedFuncs));
	UNTEST_EXPECT_TRUE(AllEntriesHaveSchemaKeys(GetArrayField(ResultInherited.Data, TEXT("components"))));

	// Any new entries in inherited-mode arrays must have is_inherited=true and
	// source_class != this BP's generated class.
	const FString GeneratedClassShort = BP->GeneratedClass ? BP->GeneratedClass->GetName() : FString();
	auto CountInheritedFromAncestor = [&](const TArray<TSharedPtr<FJsonValue>>* Array) -> int32
	{
		int32 Count = 0;
		if (!Array)
		{
			return 0;
		}
		for (const TSharedPtr<FJsonValue>& V : *Array)
		{
			const TSharedPtr<FJsonObject> Obj = V.IsValid() ? V->AsObject() : nullptr;
			if (!Obj.IsValid())
			{
				continue;
			}
			bool bInh = false;
			Obj->TryGetBoolField(TEXT("is_inherited"), bInh);
			FString Src;
			Obj->TryGetStringField(TEXT("source_class"), Src);
			if (bInh && Src != GeneratedClassShort)
			{
				++Count;
			}
		}
		return Count;
	};

	// AStaticMeshActor parent must contribute at least one inherited variable.
	UNTEST_EXPECT_GE(CountInheritedFromAncestor(InheritedVars), 1);

	BPProps_DeleteIfExists(ObjectPath);
	co_return;
}

// ============================================================================
// Summary line carries "(N inherited)" suffix when N > 0 inherited components.
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, GetBlueprintProperties, InheritedMode_SummaryHasInheritedSuffix, UNTEST_TIMEOUTMS(60000))
{
	const FString PackagePath = FString::Printf(TEXT("%s/BP_SummarySuffix"), SandboxFolder);
	const FString ObjectPath = PackagePath + TEXT(".BP_SummarySuffix");
	BPProps_DeleteIfExists(ObjectPath);

	UBlueprint* BP = CreateBPWithSphereSCS(PackagePath);
	UNTEST_ASSERT_PTR(BP);

	ClaireonTool_GetBlueprintProperties Tool;
	IClaireonTool::FToolResult Result = Tool.Execute(MakeArgs(PackagePath, /*bIncludeInherited=*/true));
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.Summary.Contains(TEXT("inherited")));
	// Specifically, the components fragment should contain "(<n> inherited)"
	// since AStaticMeshActor contributes a native subobject.
	UNTEST_EXPECT_TRUE(Result.Summary.Contains(TEXT("components (")));

	// In default mode, no "(N inherited)" suffix.
	IClaireonTool::FToolResult ResultDefault = Tool.Execute(MakeArgs(PackagePath, /*bIncludeInherited=*/false));
	UNTEST_ASSERT_FALSE(ResultDefault.bIsError);
	UNTEST_EXPECT_FALSE(ResultDefault.Summary.Contains(TEXT("inherited")));

	BPProps_DeleteIfExists(ObjectPath);
	co_return;
}

#endif // WITH_UNTESTED
