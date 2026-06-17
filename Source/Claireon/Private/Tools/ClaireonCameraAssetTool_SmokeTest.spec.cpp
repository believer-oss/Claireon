// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Misc/EngineVersionComparison.h"

#if WITH_GAMEPLAY_CAMERAS

#include "Tools/ClaireonCameraAssetTool_AddNode.h"
#include "Tools/ClaireonCameraAssetTool_AddRig.h"
#include "Tools/ClaireonCameraAssetTool_Create.h"
#include "Tools/ClaireonCameraAssetTool_Duplicate.h"
#include "Tools/ClaireonCameraAssetTool_GetNodeProperty.h"
#include "Tools/ClaireonCameraAssetTool_ListNodes.h"
#include "Tools/ClaireonCameraAssetTool_ListRigs.h"
#include "Tools/ClaireonCameraAssetTool_Save.h"
#include "Tools/ClaireonCameraAssetTool_SetNodeProperty.h"

#include "Core/CameraAsset.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Misc/AutomationTest.h"
#include "PackageTools.h"
#include "UObject/Package.h"

namespace
{
	/** Best-effort cleanup of a /Game/Tests/<X> asset; ignores absence. */
	void CASmokeSpec_DeleteIfExists(const FString& Path)
	{
		if (UEditorAssetLibrary::DoesAssetExist(Path))
		{
			UEditorAssetLibrary::DeleteAsset(Path);
		}
	}

	/** Unload the in-memory package so subsequent LoadObject re-pulls from disk. */
	bool CASmokeSpec_UnloadPackage(FAutomationTestBase& Test, const FString& Path)
	{
		if (UPackage* Pkg = FindPackage(nullptr, *Path))
		{
			FText UnloadErr;
			const bool bUnloaded = UPackageTools::UnloadPackages({ Pkg }, UnloadErr, /*bUnloadDirtyPackages=*/true);
			if (!bUnloaded)
			{
				Test.AddError(FString::Printf(TEXT("UnloadPackages failed: %s"), *UnloadErr.ToString()));
				return false;
			}
		}
		return true;
	}

	IClaireonTool::FToolResult CASmokeSpec_Create(const FString& Path)
	{
		FClaireonCameraAssetTool_Create Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), Path);
		return Tool.Execute(Args);
	}

	IClaireonTool::FToolResult CASmokeSpec_AddRig(const FString& Path, const FString& RigName)
	{
		FClaireonCameraAssetTool_AddRig Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), Path);
		Args->SetStringField(TEXT("rig_name"), RigName);
		return Tool.Execute(Args);
	}

	IClaireonTool::FToolResult CASmokeSpec_AddNode(
		const FString& Path,
		int32 RigIndex,
		const FString& ParentNodeId,
		const FString& NodeClass)
	{
		FClaireonCameraAssetTool_AddNode Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), Path);
		Args->SetNumberField(TEXT("rig_index"), RigIndex);
		Args->SetStringField(TEXT("parent_node_id"), ParentNodeId);
		Args->SetStringField(TEXT("node_class"), NodeClass);
		return Tool.Execute(Args);
	}

	IClaireonTool::FToolResult CASmokeSpec_SetNodeProperty(
		const FString& Path,
		int32 RigIndex,
		const FString& NodeId,
		const FString& PropertyPath,
		const FString& Value)
	{
		FClaireonCameraAssetTool_SetNodeProperty Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), Path);
		Args->SetNumberField(TEXT("rig_index"), RigIndex);
		Args->SetStringField(TEXT("node_id"), NodeId);
		Args->SetStringField(TEXT("property_path"), PropertyPath);
		Args->SetStringField(TEXT("value"), Value);
		return Tool.Execute(Args);
	}

	IClaireonTool::FToolResult CASmokeSpec_GetNodeProperty(
		const FString& Path,
		int32 RigIndex,
		const FString& NodeId,
		const FString& PropertyPath)
	{
		FClaireonCameraAssetTool_GetNodeProperty Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), Path);
		Args->SetNumberField(TEXT("rig_index"), RigIndex);
		Args->SetStringField(TEXT("node_id"), NodeId);
		Args->SetStringField(TEXT("property_path"), PropertyPath);
		return Tool.Execute(Args);
	}

	IClaireonTool::FToolResult CASmokeSpec_Save(const FString& Path)
	{
		FClaireonCameraAssetTool_Save Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), Path);
		return Tool.Execute(Args);
	}

	IClaireonTool::FToolResult CASmokeSpec_ListNodes(const FString& Path, int32 RigIndex)
	{
		FClaireonCameraAssetTool_ListNodes Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), Path);
		Args->SetNumberField(TEXT("rig_index"), RigIndex);
		return Tool.Execute(Args);
	}

	IClaireonTool::FToolResult CASmokeSpec_ListRigs(const FString& Path)
	{
		FClaireonCameraAssetTool_ListRigs Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), Path);
		return Tool.Execute(Args);
	}

	IClaireonTool::FToolResult CASmokeSpec_Duplicate(const FString& Src, const FString& Dst)
	{
		FClaireonCameraAssetTool_Duplicate Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("source_path"), Src);
		Args->SetStringField(TEXT("dest_path"), Dst);
		return Tool.Execute(Args);
	}

	/** Numerically compare two ExportText-formatted floats. Floats may serialize as "7.5", "7.500000", etc. */
	bool CASmokeSpec_FloatsApproxEqual(const FString& A, float Target)
	{
		const float Numeric = FCString::Atof(*A);
		return FMath::IsNearlyEqual(Numeric, Target, /*Tolerance=*/1e-3f);
	}
} // namespace

// =====================================================================================
// Test: SmokeTest_SyntheticFixtureRoundTrip
// Fully synthetic end-to-end: create -> AddRig -> AddNode (root array) -> AddNode (child)
// -> SetNodeProperty -> Save -> Unload -> Reload -> ListNodes verifies 2 nodes ->
// GetNodeProperty verifies the value round-tripped.
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAssetSmokeTest_SyntheticFixtureRoundTrip,
	"Claireon.CameraAsset.SmokeTest.SyntheticFixtureRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCameraAssetSmokeTest_SyntheticFixtureRoundTrip::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/CA_Synth_Fixture");
	const FString ChildId = TEXT("Root.Children[0]");
	const FString PropName = TEXT("InterpSpeed");
	const FString TargetValue = TEXT("7.5");
	CASmokeSpec_DeleteIfExists(Path);

	// 1. Create.
	{
		const auto R = CASmokeSpec_Create(Path);
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("Create failed: %s"), *R.ErrorMessage));
			return false;
		}
	}
	// 2. AddRig.
	{
		const auto R = CASmokeSpec_AddRig(Path, TEXT("R"));
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("AddRig failed: %s"), *R.ErrorMessage));
			CASmokeSpec_DeleteIfExists(Path);
			return false;
		}
	}
	// 3. AddNode root = UArrayCameraNode.
	{
		const auto R = CASmokeSpec_AddNode(Path, 0, FString(), TEXT("ArrayCameraNode"));
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("AddNode(root) failed: %s"), *R.ErrorMessage));
			CASmokeSpec_DeleteIfExists(Path);
			return false;
		}
	}
	// 4. AddNode child = UBVLookAtCameraNode.
	{
		const auto R = CASmokeSpec_AddNode(Path, 0, TEXT("Root"), TEXT("BVLookAtCameraNode"));
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("AddNode(child) failed: %s"), *R.ErrorMessage));
			CASmokeSpec_DeleteIfExists(Path);
			return false;
		}
	}
	// 5. SetNodeProperty InterpSpeed = "7.5".
	{
		const auto R = CASmokeSpec_SetNodeProperty(Path, 0, ChildId, PropName, TargetValue);
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("SetNodeProperty failed: %s"), *R.ErrorMessage));
			CASmokeSpec_DeleteIfExists(Path);
			return false;
		}
	}
	// 6. Save.
	{
		const auto R = CASmokeSpec_Save(Path);
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("Save returned error: %s"), *R.ErrorMessage));
			CASmokeSpec_DeleteIfExists(Path);
			return false;
		}
		bool bSuccess = false;
		if (!R.Data.IsValid() || !R.Data->TryGetBoolField(TEXT("success"), bSuccess) || !bSuccess)
		{
			AddError(TEXT("Save reported success=false"));
			CASmokeSpec_DeleteIfExists(Path);
			return false;
		}
	}
	// 7. Unload.
	if (!CASmokeSpec_UnloadPackage(*this, Path))
	{
		CASmokeSpec_DeleteIfExists(Path);
		return false;
	}

	// 8. Reload + ListNodes -> 2 entries.
	{
		const auto R = CASmokeSpec_ListNodes(Path, 0);
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("ListNodes(post-reload) failed: %s"), *R.ErrorMessage));
			CASmokeSpec_DeleteIfExists(Path);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* NodesPtr = nullptr;
		if (!R.Data.IsValid() || !R.Data->TryGetArrayField(TEXT("nodes"), NodesPtr) || !NodesPtr)
		{
			AddError(TEXT("ListNodes(post-reload) result missing 'nodes' array"));
			CASmokeSpec_DeleteIfExists(Path);
			return false;
		}
		if (NodesPtr->Num() != 2)
		{
			AddError(FString::Printf(TEXT("Expected 2 nodes post-reload; got %d"), NodesPtr->Num()));
			CASmokeSpec_DeleteIfExists(Path);
			return false;
		}
	}

	// 9. GetNodeProperty -> ~7.5.
	{
		const auto R = CASmokeSpec_GetNodeProperty(Path, 0, ChildId, PropName);
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("GetNodeProperty(post-reload) failed: %s"), *R.ErrorMessage));
			CASmokeSpec_DeleteIfExists(Path);
			return false;
		}
		FString Got;
		if (!R.Data.IsValid() || !R.Data->TryGetStringField(TEXT("value"), Got))
		{
			AddError(TEXT("GetNodeProperty(post-reload) result missing 'value'"));
			CASmokeSpec_DeleteIfExists(Path);
			return false;
		}
		if (!CASmokeSpec_FloatsApproxEqual(Got, 7.5f))
		{
			AddError(FString::Printf(
				TEXT("Persistence failed: post-reload value '%s' (expected ~7.5)"), *Got));
			CASmokeSpec_DeleteIfExists(Path);
			return false;
		}
	}

	CASmokeSpec_DeleteIfExists(Path);
	return true;
}

// =====================================================================================
// Test: SmokeTest_DuplicatePrototypeAndAddNode
// Tied to real content (CA_FD_Player_Prototype_Ranged). If the prototype isn't in this
// worktree, AddWarning + pass — the synthetic test is the hard guarantee.
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAssetSmokeTest_DuplicatePrototypeAndAddNode,
	"Claireon.CameraAsset.SmokeTest.DuplicatePrototypeAndAddNode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCameraAssetSmokeTest_DuplicatePrototypeAndAddNode::RunTest(const FString& /*Parameters*/)
{
	const FString SourcePath = TEXT("/Game/BP/Camera/Fellowdivers/CA_FD_Player_Prototype_Ranged");
	const FString DupPath = TEXT("/Game/Tests/CA_Smoke_Prototype");
	const FString PropName = TEXT("InterpSpeed");
	const FString TargetValue = TEXT("7.5");

	// 1. Verify prototype exists; if not, soft-pass with warning.
	if (!UEditorAssetLibrary::DoesAssetExist(SourcePath))
	{
		AddWarning(FString::Printf(
			TEXT("Prototype not found at %s; skipping prototype-duplicate smoke test "
				 "(synthetic-fixture test still covers the round-trip)."),
			*SourcePath));
		return true;
	}

	CASmokeSpec_DeleteIfExists(DupPath);

	// 2. Duplicate.
	{
		const auto R = CASmokeSpec_Duplicate(SourcePath, DupPath);
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("Duplicate failed: %s"), *R.ErrorMessage));
			return false;
		}
	}

	// 3. ListRigs -> pick rig 0.
	int32 RigIndex = 0;
	{
		const auto R = CASmokeSpec_ListRigs(DupPath);
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("ListRigs failed: %s"), *R.ErrorMessage));
			CASmokeSpec_DeleteIfExists(DupPath);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* RigsPtr = nullptr;
		if (!R.Data.IsValid() || !R.Data->TryGetArrayField(TEXT("rigs"), RigsPtr) || !RigsPtr)
		{
			AddError(TEXT("ListRigs result missing 'rigs' array"));
			CASmokeSpec_DeleteIfExists(DupPath);
			return false;
		}
		if (RigsPtr->Num() == 0)
		{
			AddWarning(TEXT("Prototype has no rigs; skipping the rest of the smoke test."));
			CASmokeSpec_DeleteIfExists(DupPath);
			return true;
		}
	}

	// 4. ListNodes -> find an ArrayCameraNode container we can attach a child to.
	FString ArrayParentId;
	{
		const auto R = CASmokeSpec_ListNodes(DupPath, RigIndex);
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("ListNodes failed: %s"), *R.ErrorMessage));
			CASmokeSpec_DeleteIfExists(DupPath);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* NodesPtr = nullptr;
		if (!R.Data.IsValid() || !R.Data->TryGetArrayField(TEXT("nodes"), NodesPtr) || !NodesPtr)
		{
			AddError(TEXT("ListNodes result missing 'nodes' array"));
			CASmokeSpec_DeleteIfExists(DupPath);
			return false;
		}
		for (const TSharedPtr<FJsonValue>& V : *NodesPtr)
		{
			const TSharedPtr<FJsonObject>& Entry = V->AsObject();
			if (!Entry.IsValid())
			{
				continue;
			}
			FString Cls;
			Entry->TryGetStringField(TEXT("class"), Cls);
			if (Cls == TEXT("ArrayCameraNode"))
			{
				Entry->TryGetStringField(TEXT("node_id"), ArrayParentId);
				break;
			}
		}
	}
	if (ArrayParentId.IsEmpty())
	{
		AddWarning(TEXT("Prototype has no UArrayCameraNode container; skipping AddNode "
						"step (synthetic-fixture test still covers the round-trip)."));
		CASmokeSpec_DeleteIfExists(DupPath);
		return true;
	}

	// 5. AddNode UBVLookAtCameraNode under the array container.
	FString NewChildId;
	{
		const auto R = CASmokeSpec_AddNode(DupPath, RigIndex, ArrayParentId, TEXT("BVLookAtCameraNode"));
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("AddNode failed: %s"), *R.ErrorMessage));
			CASmokeSpec_DeleteIfExists(DupPath);
			return false;
		}
		if (!R.Data.IsValid() || !R.Data->TryGetStringField(TEXT("node_id"), NewChildId))
		{
			AddError(TEXT("AddNode result missing 'node_id'"));
			CASmokeSpec_DeleteIfExists(DupPath);
			return false;
		}
	}

	// 6. SetNodeProperty InterpSpeed = "7.5".
	{
		const auto R = CASmokeSpec_SetNodeProperty(DupPath, RigIndex, NewChildId, PropName, TargetValue);
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("SetNodeProperty failed: %s"), *R.ErrorMessage));
			CASmokeSpec_DeleteIfExists(DupPath);
			return false;
		}
	}

	// 7. Save.
	{
		const auto R = CASmokeSpec_Save(DupPath);
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("Save returned error: %s"), *R.ErrorMessage));
			CASmokeSpec_DeleteIfExists(DupPath);
			return false;
		}
		bool bSuccess = false;
		if (!R.Data.IsValid() || !R.Data->TryGetBoolField(TEXT("success"), bSuccess) || !bSuccess)
		{
			AddError(TEXT("Save reported success=false on duplicated prototype"));
			CASmokeSpec_DeleteIfExists(DupPath);
			return false;
		}
	}

	// 8. Unload.
	if (!CASmokeSpec_UnloadPackage(*this, DupPath))
	{
		CASmokeSpec_DeleteIfExists(DupPath);
		return false;
	}

	// 9. Reload + verify the new node persisted with InterpSpeed ~7.5.
	{
		const auto R = CASmokeSpec_GetNodeProperty(DupPath, RigIndex, NewChildId, PropName);
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("GetNodeProperty(post-reload) failed: %s"), *R.ErrorMessage));
			CASmokeSpec_DeleteIfExists(DupPath);
			return false;
		}
		FString Got;
		if (!R.Data.IsValid() || !R.Data->TryGetStringField(TEXT("value"), Got))
		{
			AddError(TEXT("GetNodeProperty(post-reload) result missing 'value'"));
			CASmokeSpec_DeleteIfExists(DupPath);
			return false;
		}
		if (!CASmokeSpec_FloatsApproxEqual(Got, 7.5f))
		{
			AddError(FString::Printf(
				TEXT("Persistence failed: post-reload value '%s' (expected ~7.5)"), *Got));
			CASmokeSpec_DeleteIfExists(DupPath);
			return false;
		}
	}

	CASmokeSpec_DeleteIfExists(DupPath);
	return true;
}

#endif // WITH_GAMEPLAY_CAMERAS
