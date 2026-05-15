// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonTool_MaterialInspect.h"
#include "Tools/ClaireonMaterialTool_Create.h"
#include "Tools/ClaireonMaterialTool_Close.h"
#include "Tools/ClaireonMaterialTool_AddExpression.h"
#include "Tools/ClaireonMaterialTool_ConnectToMaterialOutput.h"
#include "Tools/ClaireonMaterialTool_SetShadingModel.h"
#include "Tools/ClaireonMaterialTool_Compile.h"
#include "Tools/ClaireonMaterialTool_ApplySpec.h"
#include "Tools/ClaireonTool_MaterialInstanceInspect.h"
#include "Tools/ClaireonMaterialInstanceTool_Open.h"
#include "Tools/ClaireonMaterialInstanceTool_Create.h"
#include "Tools/ClaireonMaterialInstanceTool_Close.h"
#include "Tools/ClaireonMaterialInstanceTool_SetParent.h"
#include "Tools/ClaireonMaterialInstanceTool_SetScalarParameter.h"
#include "Tools/ClaireonMaterialInstanceTool_ApplySpec.h"
#include "Tools/ClaireonTool_MaterialApply.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "ClaireonSessionManager.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpression.h"
#include "EditorAssetLibrary.h"

#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Components/StaticMeshComponent.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Editor.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/ConstructorHelpers.h"

// ---------------------------------------------------------------------------
// Test asset paths
// ---------------------------------------------------------------------------

// Stable project material with at least one scalar parameter.
// M_BlendMaster is a long-standing master material in /Game/Art_Lib/MF.
static const TCHAR* TestMaterialPath = TEXT("/Game/Art_Lib/MF/M_BlendMaster");

// Stable project material instance that overrides parameters from its parent.
// MI_StyleTest is a long-standing MIC under /Game/Art_Lib/MM.
static const TCHAR* TestMICPath = TEXT("/Game/Art_Lib/MM/MI_StyleTest");

// Sandbox folder for tests that mutate or create assets. Cleaned up at test end.
static const TCHAR* TestSandboxPackagePath = TEXT("/Game/__ClaireonMaterialTests");

// ---------------------------------------------------------------------------
// File-local helpers
// ---------------------------------------------------------------------------

namespace ClaireonMaterialTestsImpl
{
	/** Build a unique sub-path under the sandbox to avoid collisions across runs. */
	static FString MakeUniqueSandboxPackage(const TCHAR* Tag)
	{
		const FString TimeStamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S_%f"));
		return FString::Printf(TEXT("%s/%s_%s"), TestSandboxPackagePath, Tag, *TimeStamp);
	}

	/** Extract session_id from a tool result's structured data. Returns empty string on failure. */
	static FString GetSessionId(const IClaireonTool::FToolResult& Result)
	{
		if (!Result.Data.IsValid())
		{
			return FString();
		}
		FString SessionId;
		Result.Data->TryGetStringField(TEXT("session_id"), SessionId);
		return SessionId;
	}

	/** Best-effort cleanup of a created asset (suppresses prompts). */
	static void TryDeleteAsset(const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty()) return;
		if (UEditorAssetLibrary::DoesAssetExist(ObjectPath))
		{
			UEditorAssetLibrary::DeleteAsset(ObjectPath);
		}
	}

	/** Close a MIC session by id (decomposed tool variant). */
	static void TryCloseMICSession(const FString& SessionId)
	{
		if (SessionId.IsEmpty()) return;
		ClaireonMaterialInstanceTool_Close CloseTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		CloseTool.Execute(Args);
	}

	/** Close a Material session by id (decomposed tool variant). */
	static void TryCloseMaterialSession(const FString& SessionId)
	{
		if (SessionId.IsEmpty()) return;
		ClaireonMaterialTool_Close CloseTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		CloseTool.Execute(Args);
	}
} // namespace ClaireonMaterialTestsImpl

using namespace ClaireonMaterialTestsImpl;

// ============================================================================
// 1. MaterialInspect_Summary
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Material, MaterialInspect_Summary, UNTEST_TIMEOUTMS(15000))
{
	// Discovery: ensure the test material exists; skip soft if missing.
	FString LoadErr;
	UMaterial* Probe = ClaireonMaterialHelpers::LoadMaterialAsset(TestMaterialPath, LoadErr);
	UNTEST_ASSERT_TRUE(Probe != nullptr);

	ClaireonTool_MaterialInspect Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestMaterialPath);
	Args->SetStringField(TEXT("detail"), TEXT("summary"));

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	FString Structure;
	Result.Data->TryGetStringField(TEXT("structure"), Structure);
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("# Material:")));
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("ShadingModel:")));
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("BlendMode:")));
	// Expect at least one parameter row OR the parameter section header.
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("Parameters")));
	co_return;
}

// ============================================================================
// 2. MaterialInspect_Full
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Material, MaterialInspect_Full, UNTEST_TIMEOUTMS(15000))
{
	FString LoadErr;
	UMaterial* Probe = ClaireonMaterialHelpers::LoadMaterialAsset(TestMaterialPath, LoadErr);
	UNTEST_ASSERT_TRUE(Probe != nullptr);
	const int32 ExprCount = Probe->GetExpressions().Num();

	ClaireonTool_MaterialInspect Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestMaterialPath);
	Args->SetStringField(TEXT("detail"), TEXT("full"));

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	FString Structure;
	Result.Data->TryGetStringField(TEXT("structure"), Structure);
	// Full should include the expression list section.
	if (ExprCount > 0)
	{
		UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("## Expressions")));
		UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("- Position:")));
	}
	co_return;
}

// ============================================================================
// 3. MaterialInstanceInspect_Overrides
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Material, MaterialInstanceInspect_Overrides, UNTEST_TIMEOUTMS(15000))
{
	FString LoadErr;
	UMaterialInstanceConstant* Probe = ClaireonMaterialHelpers::LoadMaterialInstanceAsset(TestMICPath, LoadErr);
	UNTEST_ASSERT_TRUE(Probe != nullptr);

	ClaireonTool_MaterialInstanceInspect Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestMICPath);

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	FString Structure;
	Result.Data->TryGetStringField(TEXT("structure"), Structure);
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("# MaterialInstanceConstant:")));
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("## Parent Chain")));
	// Either inherited or override columns must appear in some parameter table.
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("Inherited Value")));
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("Override Value")));
	co_return;
}

// ============================================================================
// 4. MaterialEdit_Lifecycle
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Material, MaterialEdit_Lifecycle, UNTEST_TIMEOUTMS(45000))
{
	// Create a fresh material in the sandbox so the test mutates only its own asset.
	const FString PackagePath = MakeUniqueSandboxPackage(TEXT("Lifecycle"));
	const FString AssetName = TEXT("M_LifecycleTest");
	const FString FullObjectPath = FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *AssetName, *AssetName);

	FString SessionId;

	// open via create
	{
		ClaireonMaterialTool_Create CreateTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("package_path"), PackagePath);
		Args->SetStringField(TEXT("asset_name"), AssetName);
		auto Result = CreateTool.Execute(Args);
		UNTEST_ASSERT_FALSE(Result.bIsError);
		SessionId = GetSessionId(Result);
		UNTEST_ASSERT_FALSE(SessionId.IsEmpty());
	}

	// add_expression: ScalarParameter "Brightness"
	{
		ClaireonMaterialTool_AddExpression AddExprTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("expression_class"), TEXT("ScalarParameter"));
		Args->SetStringField(TEXT("expression_name"), TEXT("Brightness"));
		auto Result = AddExprTool.Execute(Args);
		UNTEST_EXPECT_FALSE(Result.bIsError);
	}

	// connect_to_material_output: BaseColor
	{
		ClaireonMaterialTool_ConnectToMaterialOutput ConnectTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("from_identifier"), TEXT("Brightness"));
		Args->SetStringField(TEXT("attribute"), TEXT("BaseColor"));
		auto Result = ConnectTool.Execute(Args);
		UNTEST_EXPECT_FALSE(Result.bIsError);
	}

	// compile (wait_for_compile)
	{
		ClaireonMaterialTool_Compile CompileTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetBoolField(TEXT("wait_for_compile"), true);
		auto Result = CompileTool.Execute(Args);
		UNTEST_EXPECT_FALSE(Result.bIsError);
	}

	// close
	TryCloseMaterialSession(SessionId);

	// Verify the in-memory material has the new expression.
	{
		FString Err;
		UMaterial* Loaded = ClaireonMaterialHelpers::LoadMaterialAsset(FullObjectPath, Err);
		UNTEST_EXPECT_TRUE(Loaded != nullptr);
		if (Loaded)
		{
			bool bFound = false;
			for (UMaterialExpression* Expr : Loaded->GetExpressions())
			{
				if (Expr && Expr->HasAParameterName() && Expr->GetParameterName() == FName(TEXT("Brightness")))
				{
					bFound = true;
					break;
				}
			}
			UNTEST_EXPECT_TRUE(bFound);
		}
	}

	// Cleanup: delete the temporary asset.
	TryDeleteAsset(FullObjectPath);
	co_return;
}

// ============================================================================
// 5. MaterialEdit_ApplySpec_Parity
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Material, MaterialEdit_ApplySpec_Parity, UNTEST_TIMEOUTMS(60000))
{
	// We compare per-op vs apply_spec final state on two freshly-created materials.
	const FString PackagePath = MakeUniqueSandboxPackage(TEXT("Parity"));
	const FString PerOpName = TEXT("M_PerOp");
	const FString SpecName = TEXT("M_Spec");
	const FString PerOpObj = FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *PerOpName, *PerOpName);
	const FString SpecObj = FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *SpecName, *SpecName);

	FString PerOpSessionId;

	// Create per-op material and run a 3-step sequence.
	{
		ClaireonMaterialTool_Create CreateTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("package_path"), PackagePath);
		Args->SetStringField(TEXT("asset_name"), PerOpName);
		auto Result = CreateTool.Execute(Args);
		UNTEST_ASSERT_FALSE(Result.bIsError);
		PerOpSessionId = GetSessionId(Result);
		UNTEST_ASSERT_FALSE(PerOpSessionId.IsEmpty());
	}

	// add ScalarParameter "Bright"
	{
		ClaireonMaterialTool_AddExpression AddExprTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), PerOpSessionId);
		Args->SetStringField(TEXT("expression_class"), TEXT("ScalarParameter"));
		Args->SetStringField(TEXT("expression_name"), TEXT("Bright"));
		AddExprTool.Execute(Args);
	}
	// connect Bright -> EmissiveColor
	{
		ClaireonMaterialTool_ConnectToMaterialOutput ConnectTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), PerOpSessionId);
		Args->SetStringField(TEXT("from_identifier"), TEXT("Bright"));
		Args->SetStringField(TEXT("attribute"), TEXT("EmissiveColor"));
		ConnectTool.Execute(Args);
	}
	// shading model -> Unlit
	{
		ClaireonMaterialTool_SetShadingModel SetShadingTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), PerOpSessionId);
		Args->SetStringField(TEXT("shading_model"), TEXT("MSM_Unlit"));
		SetShadingTool.Execute(Args);
	}
	TryCloseMaterialSession(PerOpSessionId);

	// Build matching apply_spec for the spec material.
	{
		ClaireonMaterialTool_Create CreateTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("package_path"), PackagePath);
		Args->SetStringField(TEXT("asset_name"), SpecName);
		auto Result = CreateTool.Execute(Args);
		UNTEST_ASSERT_FALSE(Result.bIsError);
		const FString CreateSession = GetSessionId(Result);
		// Close the create-session so apply_spec opens its own.
		TryCloseMaterialSession(CreateSession);
	}

	{
		ClaireonMaterialTool_ApplySpec ApplySpecTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), SpecObj);

		TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();

		// expressions: one ScalarParameter "Bright"
		TArray<TSharedPtr<FJsonValue>> Exprs;
		{
			TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("id"), TEXT("e1"));
			E->SetStringField(TEXT("class"), TEXT("ScalarParameter"));
			E->SetStringField(TEXT("parameter_name"), TEXT("Bright"));
			Exprs.Add(MakeShared<FJsonValueObject>(E));
		}
		Spec->SetArrayField(TEXT("expressions"), Exprs);

		// attribute_connections: e1 -> EmissiveColor
		TArray<TSharedPtr<FJsonValue>> AttrConns;
		{
			TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
			C->SetStringField(TEXT("from_id"), TEXT("e1"));
			C->SetStringField(TEXT("attribute"), TEXT("EmissiveColor"));
			AttrConns.Add(MakeShared<FJsonValueObject>(C));
		}
		Spec->SetArrayField(TEXT("attribute_connections"), AttrConns);

		// shading_model
		Spec->SetStringField(TEXT("shading_model"), TEXT("MSM_Unlit"));

		// no compile / save (we only compare in-memory state).
		Spec->SetBoolField(TEXT("compile_at_end"), false);
		Spec->SetBoolField(TEXT("save_at_end"), false);

		Args->SetObjectField(TEXT("spec"), Spec);

		auto Result = ApplySpecTool.Execute(Args);
		UNTEST_EXPECT_FALSE(Result.bIsError);
	}

	// Compare final state.
	{
		FString E1, E2;
		UMaterial* PerOp = ClaireonMaterialHelpers::LoadMaterialAsset(PerOpObj, E1);
		UMaterial* Spec = ClaireonMaterialHelpers::LoadMaterialAsset(SpecObj, E2);
		UNTEST_ASSERT_TRUE(PerOp != nullptr);
		UNTEST_ASSERT_TRUE(Spec != nullptr);

		// Same expression count.
		UNTEST_EXPECT_TRUE(PerOp->GetExpressions().Num() == Spec->GetExpressions().Num());

		// Both have a parameter named "Bright".
		auto HasBright = [](UMaterial* M)
		{
			for (UMaterialExpression* Expr : M->GetExpressions())
			{
				if (Expr && Expr->HasAParameterName() && Expr->GetParameterName() == FName(TEXT("Bright")))
				{
					return true;
				}
			}
			return false;
		};
		UNTEST_EXPECT_TRUE(HasBright(PerOp));
		UNTEST_EXPECT_TRUE(HasBright(Spec));

		// Same shading model.
		UNTEST_EXPECT_TRUE(PerOp->GetShadingModels().GetFirstShadingModel() ==
		                   Spec->GetShadingModels().GetFirstShadingModel());
	}

	TryDeleteAsset(PerOpObj);
	TryDeleteAsset(SpecObj);
	co_return;
}

// ============================================================================
// 6. MaterialInstanceEdit_Create
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Material, MaterialInstanceEdit_Create, UNTEST_TIMEOUTMS(30000))
{
	const FString PackagePath = MakeUniqueSandboxPackage(TEXT("MICCreate"));
	const FString MicName = TEXT("MIC_Created");
	const FString MicObj = FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *MicName, *MicName);

	// Use the test material as the parent.
	FString SessionId;

	{
		ClaireonMaterialInstanceTool_Create CreateTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("package_path"), PackagePath);
		Args->SetStringField(TEXT("asset_name"), MicName);
		Args->SetStringField(TEXT("parent_path"), TestMaterialPath);
		auto Result = CreateTool.Execute(Args);
		UNTEST_ASSERT_FALSE(Result.bIsError);
		SessionId = GetSessionId(Result);
		UNTEST_ASSERT_FALSE(SessionId.IsEmpty());
	}

	// Try to set a scalar override -- accept either success or failure (parent
	// may not have a parameter by that name); this exercises the dispatch path.
	{
		ClaireonMaterialInstanceTool_SetScalarParameter SetScalarTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("parameter_name"), TEXT("ProbeScalar_DoesNotMatter"));
		Args->SetNumberField(TEXT("value"), 0.5);
		SetScalarTool.Execute(Args); // non-fatal, may fail by design
	}

	TryCloseMICSession(SessionId);

	// Verify the MIC exists with the expected parent.
	{
		FString Err;
		UMaterialInstanceConstant* Loaded = ClaireonMaterialHelpers::LoadMaterialInstanceAsset(MicObj, Err);
		UNTEST_EXPECT_TRUE(Loaded != nullptr);
		if (Loaded && Loaded->Parent)
		{
			UNTEST_EXPECT_TRUE(Loaded->Parent->GetPathName().EndsWith(TEXT("M_BlendMaster")));
		}
	}

	TryDeleteAsset(MicObj);
	co_return;
}

// ============================================================================
// 7. MaterialInstanceEdit_ParentCycle
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Material, MaterialInstanceEdit_ParentCycle, UNTEST_TIMEOUTMS(30000))
{
	const FString PackagePath = MakeUniqueSandboxPackage(TEXT("Cycle"));
	const FString MicAName = TEXT("MIC_A");
	const FString MicAObj = FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *MicAName, *MicAName);

	FString SessionA;

	// Create MIC_A with M_BlendMaster as parent.
	{
		ClaireonMaterialInstanceTool_Create CreateTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("package_path"), PackagePath);
		Args->SetStringField(TEXT("asset_name"), MicAName);
		Args->SetStringField(TEXT("parent_path"), TestMaterialPath);
		auto Result = CreateTool.Execute(Args);
		UNTEST_ASSERT_FALSE(Result.bIsError);
		SessionA = GetSessionId(Result);
		UNTEST_ASSERT_FALSE(SessionA.IsEmpty());
	}

	// Direct cycle attempt: set MIC_A's parent to itself.
	{
		ClaireonMaterialInstanceTool_SetParent SetParentTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionA);
		Args->SetStringField(TEXT("parent_path"), MicAObj);
		auto Result = SetParentTool.Execute(Args);
		UNTEST_EXPECT_TRUE(Result.bIsError);
		UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("cycle")));
	}

	// Verify parent unchanged (still M_BlendMaster).
	{
		FString Err;
		UMaterialInstanceConstant* Loaded = ClaireonMaterialHelpers::LoadMaterialInstanceAsset(MicAObj, Err);
		UNTEST_ASSERT_TRUE(Loaded != nullptr);
		UNTEST_ASSERT_TRUE(Loaded->Parent != nullptr);
		UNTEST_EXPECT_TRUE(Loaded->Parent->GetPathName().EndsWith(TEXT("M_BlendMaster")));
	}

	TryCloseMICSession(SessionA);
	TryDeleteAsset(MicAObj);
	co_return;
}

// ============================================================================
// 8. MaterialApply_Actor
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Material, MaterialApply_Actor, UNTEST_TIMEOUTMS(30000))
{
	if (!GEditor)
	{
		UNTEST_ASSERT_TRUE(false);
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();
	UNTEST_ASSERT_TRUE(World != nullptr);

	// Spawn a temporary StaticMeshActor.
	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags |= RF_Transient;
	const FString UniqueLabel = FString::Printf(TEXT("ClaireonMatTest_%s"),
		*FDateTime::UtcNow().ToString(TEXT("%H%M%S_%f")));
	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(),
		FTransform::Identity, SpawnParams);
	UNTEST_ASSERT_TRUE(Actor != nullptr);
	Actor->SetActorLabel(UniqueLabel);

	// Attach a placeholder static mesh to populate at least one material slot.
	UStaticMeshComponent* SMC = Actor->GetStaticMeshComponent();
	UNTEST_ASSERT_TRUE(SMC != nullptr);

	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UNTEST_ASSERT_TRUE(Mesh != nullptr);
	SMC->SetStaticMesh(Mesh);
	const int32 SlotCount = SMC->GetNumMaterials();
	UNTEST_ASSERT_TRUE(SlotCount > 0);

	// Apply the test material to slot 0.
	ClaireonTool_MaterialApply Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("material_path"), TestMaterialPath);
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetStringField(TEXT("kind"), TEXT("actor"));
	Target->SetStringField(TEXT("actor_name"), UniqueLabel);
	Target->SetStringField(TEXT("component_name"), SMC->GetName());
	Target->SetNumberField(TEXT("element_index"), 0);
	Args->SetObjectField(TEXT("target"), Target);

	auto Result = Tool.Execute(Args);
	UNTEST_EXPECT_FALSE(Result.bIsError);

	if (!Result.bIsError)
	{
		UMaterialInterface* AppliedMat = SMC->GetMaterial(0);
		UNTEST_EXPECT_TRUE(AppliedMat != nullptr);
		if (AppliedMat)
		{
			UNTEST_EXPECT_TRUE(AppliedMat->GetPathName().EndsWith(TEXT("M_BlendMaster")));
		}
		// Response should mention "Prior Material" since SetMaterial captured a prior.
		UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Prior Material")));
	}

	// Cleanup the spawned actor.
	if (IsValid(Actor))
	{
		World->DestroyActor(Actor);
	}
	co_return;
}

// ============================================================================
// 9. MaterialApply_Blueprint
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Material, MaterialApply_Blueprint, UNTEST_TIMEOUTMS(60000))
{
	const FString PackagePath = MakeUniqueSandboxPackage(TEXT("BPApply"));
	const FString BPName = TEXT("BP_MatApply");
	const FString BPObj = FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *BPName, *BPName);

	// Create a temp package + blueprint based on AStaticMeshActor (so it has a
	// StaticMeshComponent template available via SCS).
	UPackage* Pkg = CreatePackage(*FString::Printf(TEXT("%s/%s"), *PackagePath, *BPName));
	UNTEST_ASSERT_TRUE(Pkg != nullptr);

	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
		AStaticMeshActor::StaticClass(),
		Pkg,
		FName(*BPName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		NAME_None);
	UNTEST_ASSERT_TRUE(BP != nullptr);
	UNTEST_ASSERT_TRUE(BP->SimpleConstructionScript != nullptr);

	// Find the inherited StaticMeshComponent SCS node name (variable name).
	// AStaticMeshActor has a StaticMeshComponent named "StaticMeshComponent0".
	// Look up the inherited component via the CDO.
	AStaticMeshActor* CDO = Cast<AStaticMeshActor>(BP->GeneratedClass ? BP->GeneratedClass->GetDefaultObject() : nullptr);
	UStaticMeshComponent* InheritedSMC = CDO ? CDO->GetStaticMeshComponent() : nullptr;
	UNTEST_ASSERT_TRUE(InheritedSMC != nullptr);

	// Inherited components are not SCS nodes; the apply tool's blueprint branch
	// requires an SCS UMeshComponent. Add a fresh UStaticMeshComponent SCS node
	// so the apply tool can locate it.
	const FName NewCompName(TEXT("AppliedMeshComp"));
	USCS_Node* NewNode = BP->SimpleConstructionScript->CreateNode(UStaticMeshComponent::StaticClass(), NewCompName);
	UNTEST_ASSERT_TRUE(NewNode != nullptr);
	BP->SimpleConstructionScript->AddNode(NewNode);

	// Give the SCS template an asset so it has a material slot.
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UNTEST_ASSERT_TRUE(Mesh != nullptr);
	if (UStaticMeshComponent* Template = Cast<UStaticMeshComponent>(NewNode->ComponentTemplate))
	{
		Template->SetStaticMesh(Mesh);
	}
	FKismetEditorUtilities::CompileBlueprint(BP);

	ClaireonTool_MaterialApply Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("material_path"), TestMaterialPath);
	TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetStringField(TEXT("kind"), TEXT("blueprint"));
	Target->SetStringField(TEXT("blueprint_path"), BPObj);
	Target->SetStringField(TEXT("component_name"), NewCompName.ToString());
	Target->SetNumberField(TEXT("element_index"), 0);
	Args->SetObjectField(TEXT("target"), Target);

	auto Result = Tool.Execute(Args);
	UNTEST_EXPECT_FALSE(Result.bIsError);

	if (!Result.bIsError)
	{
		// Verify the SCS template was updated.
		if (UStaticMeshComponent* Template = Cast<UStaticMeshComponent>(NewNode->ComponentTemplate))
		{
			UMaterialInterface* AppliedMat = Template->GetMaterial(0);
			UNTEST_EXPECT_TRUE(AppliedMat != nullptr);
		}
		UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Compile:")));
	}

	// Cleanup.
	TryDeleteAsset(BPObj);
	co_return;
}

// ============================================================================
// 10. MaterialInstanceEdit_ApplySpec_Parity
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Material, MaterialInstanceEdit_ApplySpec_Parity, UNTEST_TIMEOUTMS(45000))
{
	const FString PackagePath = MakeUniqueSandboxPackage(TEXT("MICParity"));
	const FString PerOpName = TEXT("MIC_PerOp");
	const FString SpecName = TEXT("MIC_Spec");
	const FString PerOpObj = FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *PerOpName, *PerOpName);
	const FString SpecObj = FString::Printf(TEXT("%s/%s.%s"), *PackagePath, *SpecName, *SpecName);

	// Per-op MIC: create with parent, set scalar override.
	FString PerOpSession;
	{
		ClaireonMaterialInstanceTool_Create CreateTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("package_path"), PackagePath);
		Args->SetStringField(TEXT("asset_name"), PerOpName);
		Args->SetStringField(TEXT("parent_path"), TestMaterialPath);
		auto Result = CreateTool.Execute(Args);
		UNTEST_ASSERT_FALSE(Result.bIsError);
		PerOpSession = GetSessionId(Result);
		UNTEST_ASSERT_FALSE(PerOpSession.IsEmpty());
	}
	{
		ClaireonMaterialInstanceTool_SetScalarParameter SetScalarTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), PerOpSession);
		Args->SetStringField(TEXT("parameter_name"), TEXT("ParityProbe"));
		Args->SetNumberField(TEXT("value"), 0.42);
		SetScalarTool.Execute(Args); // accept failure if parameter doesn't exist on parent
	}
	TryCloseMICSession(PerOpSession);

	// Spec MIC: create then apply_spec with the same set.
	{
		ClaireonMaterialInstanceTool_Create CreateTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("package_path"), PackagePath);
		Args->SetStringField(TEXT("asset_name"), SpecName);
		Args->SetStringField(TEXT("parent_path"), TestMaterialPath);
		auto Result = CreateTool.Execute(Args);
		UNTEST_ASSERT_FALSE(Result.bIsError);
		const FString CreateSession = GetSessionId(Result);
		TryCloseMICSession(CreateSession);
	}
	{
		ClaireonMaterialInstanceTool_ApplySpec ApplySpecTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), SpecObj);

		TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Pars;
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("name"), TEXT("ParityProbe"));
			P->SetStringField(TEXT("type"), TEXT("scalar"));
			P->SetNumberField(TEXT("value"), 0.42);
			Pars.Add(MakeShared<FJsonValueObject>(P));
		}
		Spec->SetArrayField(TEXT("parameters"), Pars);
		Spec->SetBoolField(TEXT("save_at_end"), false);
		Args->SetObjectField(TEXT("spec"), Spec);

		auto Result = ApplySpecTool.Execute(Args);
		// apply_spec must not crash; failures here indicate divergence between
		// per-op and spec applicator paths.
		UNTEST_EXPECT_FALSE(Result.bIsError);
	}

	// Compare the two MICs' scalar override arrays for parity.
	{
		FString E1, E2;
		UMaterialInstanceConstant* PerOp = ClaireonMaterialHelpers::LoadMaterialInstanceAsset(PerOpObj, E1);
		UMaterialInstanceConstant* Spec = ClaireonMaterialHelpers::LoadMaterialInstanceAsset(SpecObj, E2);
		UNTEST_ASSERT_TRUE(PerOp != nullptr);
		UNTEST_ASSERT_TRUE(Spec != nullptr);

		// Both should reference the same parent and have matching scalar override counts.
		UNTEST_EXPECT_TRUE(PerOp->Parent == Spec->Parent);
		UNTEST_EXPECT_TRUE(PerOp->ScalarParameterValues.Num() == Spec->ScalarParameterValues.Num());
	}

	TryDeleteAsset(PerOpObj);
	TryDeleteAsset(SpecObj);
	co_return;
}

#endif // WITH_UNTESTED
