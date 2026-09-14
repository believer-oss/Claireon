// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Tests for claireon.bp_set_property and claireon.bp_get_component_details on
// components INHERITED from a parent Blueprint.
//
// A child Blueprint does not own the SCS node of a component it inherits; the
// Details panel edits such a component through the child's
// InheritableComponentHandler override template. Before these tests the tools
// looked only in the child's own SCS, so an inherited component reported
// "Component not found". The dangerous alternative -- resolving the parent's node
// and writing its template -- would silently edit the parent Blueprint, so the
// central assertion here is that the PARENT template is untouched after a write.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonBlueprintGraphTool_GetComponentDetails.h"
#include "Tools/ClaireonBlueprintGraphTool_SetProperty.h"
#include "Tools/IClaireonTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/BoxComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

#include "ClaireonTestAssetDeletion.h"

namespace ClaireonBlueprintGraphTool_SetPropertyInheritedComponentTests_Private
{
	static const TCHAR* ParentBPPath = TEXT("/Game/__MCPTests/BP_SetProp_InheritedParent");
	static const TCHAR* ChildBPPath  = TEXT("/Game/__MCPTests/BP_SetProp_InheritedChild");
	static const TCHAR* ComponentName = TEXT("CollisionBox");

	void CleanupAsset(const FString& AssetPath)
	{
		const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad(); IsValid(Asset))
		{
			TArray<UObject*> AssetsToDelete;
			AssetsToDelete.Add(Asset);
			ClaireonTestAssetDeletion::DeleteObjectsForTest(AssetsToDelete);
		}
	}

	UBlueprint* CreateBP(const FString& AssetPath, UClass* ParentClass)
	{
		UPackage* Package = CreatePackage(*AssetPath);
		if (!IsValid(Package)) return nullptr;

		const FString AssetName = FPackageName::GetShortName(AssetPath);
		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			ParentClass, Package, FName(*AssetName), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), NAME_None);
		if (!IsValid(BP)) return nullptr;

		FAssetRegistryModule::AssetCreated(BP);
		BP->MarkPackageDirty();
		return BP;
	}

	void SaveBP(UBlueprint* BP, const FString& AssetPath)
	{
		const FString PackageFileName = FPackageName::LongPackageNameToFilename(
			AssetPath, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		UPackage::Save(BP->GetOutermost(), BP, *PackageFileName, SaveArgs);
	}

	// Parent: AActor Blueprint with one SCS UBoxComponent named CollisionBox.
	// Child: derives from the parent's generated class and adds nothing, so
	// CollisionBox is purely inherited.
	struct FParentChild
	{
		UBlueprint* Parent = nullptr;
		UBlueprint* Child = nullptr;
		USCS_Node* ParentNode = nullptr;
	};

	FParentChild CreateParentChild()
	{
		FParentChild Out;
		CleanupAsset(ChildBPPath);
		CleanupAsset(ParentBPPath);

		Out.Parent = CreateBP(ParentBPPath, AActor::StaticClass());
		if (!IsValid(Out.Parent)) return Out;

		USimpleConstructionScript* SCS = Out.Parent->SimpleConstructionScript;
		Out.ParentNode = SCS->CreateNode(UBoxComponent::StaticClass(), FName(ComponentName));
		SCS->AddNode(Out.ParentNode);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Out.Parent);
		FKismetEditorUtilities::CompileBlueprint(Out.Parent);
		SaveBP(Out.Parent, ParentBPPath);

		Out.Child = CreateBP(ChildBPPath, Out.Parent->GeneratedClass);
		if (!IsValid(Out.Child)) return Out;
		FKismetEditorUtilities::CompileBlueprint(Out.Child);
		SaveBP(Out.Child, ChildBPPath);
		return Out;
	}

	TSharedPtr<FJsonObject> MakeSetArgs(const TCHAR* AssetPath, const TCHAR* Component, const TCHAR* Property, const TCHAR* Value)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("component_name"), Component);
		Args->SetStringField(TEXT("property_name"), Property);
		Args->SetStringField(TEXT("property_value"), Value);
		return Args;
	}

	TSharedPtr<FJsonObject> MakeDetailsArgs(const TCHAR* AssetPath, const TCHAR* Component)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("component_name"), Component);
		return Args;
	}
}
using namespace ClaireonBlueprintGraphTool_SetPropertyInheritedComponentTests_Private;

// ============================================================================
// Writing a property on an inherited component creates the CHILD's override
// template with the new value and leaves the PARENT's template untouched.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, SetProperty, InheritedComponent_WritesChildOverrideNotParent, UNTEST_TIMEOUTMS(60000))
{
	FParentChild PC = CreateParentChild();
	UNTEST_ASSERT_PTR(PC.Parent);
	UNTEST_ASSERT_PTR(PC.Child);
	UNTEST_ASSERT_PTR(PC.ParentNode);

	UActorComponent* ParentTemplate = PC.ParentNode->ComponentTemplate;
	UNTEST_ASSERT_PTR(ParentTemplate);
	UNTEST_ASSERT_TRUE(ParentTemplate->CanEverAffectNavigation());

	// Precondition: the child has no override for this component yet.
	const FComponentKey Key(PC.ParentNode);
	UInheritableComponentHandler* HandlerBefore = PC.Child->GetInheritableComponentHandler(/*bCreateIfNecessary=*/false);
	UNTEST_ASSERT_TRUE(!HandlerBefore || HandlerBefore->GetOverridenComponentTemplate(Key) == nullptr);

	ClaireonBlueprintGraphTool_SetProperty Tool;
	IClaireonTool::FToolResult R = Tool.Execute(
		MakeSetArgs(ChildBPPath, ComponentName, TEXT("bCanEverAffectNavigation"), TEXT("false")));
	UNTEST_ASSERT_FALSE(R.bIsError);

	UInheritableComponentHandler* Handler = PC.Child->GetInheritableComponentHandler(/*bCreateIfNecessary=*/false);
	UNTEST_ASSERT_PTR(Handler);
	UActorComponent* Override = Handler->GetOverridenComponentTemplate(Key);
	UNTEST_ASSERT_PTR(Override);
	UNTEST_EXPECT_FALSE(Override->CanEverAffectNavigation());

	// The parent template must be exactly as it was.
	UNTEST_EXPECT_TRUE(ParentTemplate->CanEverAffectNavigation());
	UNTEST_EXPECT_TRUE(Override != ParentTemplate);

	CleanupAsset(ChildBPPath);
	CleanupAsset(ParentBPPath);
	co_return;
}

// ============================================================================
// Inspecting an inherited component succeeds, reports inherited=true, and does
// not create an override template as a side effect of a read.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, GetComponentDetails, InheritedComponent_ReadsWithoutCreatingOverride, UNTEST_TIMEOUTMS(60000))
{
	FParentChild PC = CreateParentChild();
	UNTEST_ASSERT_PTR(PC.Child);
	UNTEST_ASSERT_PTR(PC.ParentNode);

	ClaireonBlueprintGraphTool_GetComponentDetails Tool;
	IClaireonTool::FToolResult R = Tool.Execute(MakeDetailsArgs(ChildBPPath, ComponentName));
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	// The tool nests the component payload under data.component.
	const TSharedPtr<FJsonObject>* Component = nullptr;
	UNTEST_ASSERT_TRUE(R.Data->TryGetObjectField(TEXT("component"), Component));
	bool bInherited = false;
	bool bOverridden = true;
	UNTEST_EXPECT_TRUE((*Component)->TryGetBoolField(TEXT("inherited"), bInherited));
	UNTEST_EXPECT_TRUE(bInherited);
	UNTEST_EXPECT_TRUE((*Component)->TryGetBoolField(TEXT("overridden"), bOverridden));
	UNTEST_EXPECT_FALSE(bOverridden);

	const FComponentKey Key(PC.ParentNode);
	UInheritableComponentHandler* Handler = PC.Child->GetInheritableComponentHandler(/*bCreateIfNecessary=*/false);
	UNTEST_EXPECT_TRUE(!Handler || Handler->GetOverridenComponentTemplate(Key) == nullptr);

	CleanupAsset(ChildBPPath);
	CleanupAsset(ParentBPPath);
	co_return;
}

// ============================================================================
// A component that exists nowhere in the hierarchy still fails cleanly.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, SetProperty, InheritedComponent_UnknownNameStillErrors, UNTEST_TIMEOUTMS(60000))
{
	FParentChild PC = CreateParentChild();
	UNTEST_ASSERT_PTR(PC.Child);

	ClaireonBlueprintGraphTool_SetProperty Tool;
	IClaireonTool::FToolResult R = Tool.Execute(
		MakeSetArgs(ChildBPPath, TEXT("NoSuchComponent"), TEXT("bCanEverAffectNavigation"), TEXT("false")));
	UNTEST_EXPECT_TRUE(R.bIsError);

	CleanupAsset(ChildBPPath);
	CleanupAsset(ParentBPPath);
	co_return;
}

#endif // WITH_UNTESTED
