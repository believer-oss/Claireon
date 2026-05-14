// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCameraAssetTool_AddNode.h"
#include "Tools/ClaireonCameraAssetTool_AddRig.h"
#include "Tools/ClaireonCameraAssetTool_Create.h"
#include "Tools/ClaireonCameraAssetTool_GetNodeProperty.h"
#include "Tools/ClaireonCameraAssetTool_ListNodes.h"
#include "Tools/ClaireonCameraAssetTool_ListRigs.h"
#include "Tools/ClaireonCameraAssetTool_MoveNode.h"
#include "Tools/ClaireonCameraAssetTool_RemoveNode.h"
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
	void CANodeMutationSpec_DeleteIfExists(const FString& Path)
	{
		if (UEditorAssetLibrary::DoesAssetExist(Path))
		{
			UEditorAssetLibrary::DeleteAsset(Path);
		}
	}

	TSharedPtr<FJsonObject> CANodeMutationSpec_StringArg(const FString& Key, const FString& Value)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(Key, Value);
		return Args;
	}

	TSharedPtr<FJsonObject> CANodeMutationSpec_TwoStringArgs(
		const FString& K1, const FString& V1,
		const FString& K2, const FString& V2)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(K1, V1);
		Args->SetStringField(K2, V2);
		return Args;
	}

	/** Common create-asset-and-rig setup. Returns true on success. */
	bool CANodeMutationSpec_CreateAssetAndRig(FAutomationTestBase& Test, const FString& Path)
	{
		{
			FClaireonCameraAssetTool_Create Tool;
			const auto Result = Tool.Execute(CANodeMutationSpec_StringArg(TEXT("asset_path"), Path));
			if (Result.bIsError)
			{
				Test.AddError(FString::Printf(TEXT("Create failed: %s"), *Result.ErrorMessage));
				return false;
			}
		}
		{
			FClaireonCameraAssetTool_AddRig Tool;
			const auto Result = Tool.Execute(CANodeMutationSpec_TwoStringArgs(
				TEXT("asset_path"), Path,
				TEXT("rig_name"), TEXT("R")));
			if (Result.bIsError)
			{
				Test.AddError(FString::Printf(TEXT("AddRig failed: %s"), *Result.ErrorMessage));
				return false;
			}
		}
		return true;
	}

	/** Build args for AddNode. parent_node_id and after_child_node_id are optional. */
	TSharedPtr<FJsonObject> CANodeMutationSpec_AddNodeArgs(
		const FString& AssetPath,
		const FString& ParentNodeId,
		const FString& NodeClass,
		const FString& AfterChildNodeId = FString())
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetNumberField(TEXT("rig_index"), 0);
		Args->SetStringField(TEXT("parent_node_id"), ParentNodeId);
		Args->SetStringField(TEXT("node_class"), NodeClass);
		if (!AfterChildNodeId.IsEmpty())
		{
			Args->SetStringField(TEXT("after_child_node_id"), AfterChildNodeId);
		}
		return Args;
	}

	IClaireonTool::FToolResult CANodeMutationSpec_AddNode(
		const FString& AssetPath,
		const FString& ParentNodeId,
		const FString& NodeClass,
		const FString& AfterChildNodeId = FString())
	{
		FClaireonCameraAssetTool_AddNode Tool;
		return Tool.Execute(CANodeMutationSpec_AddNodeArgs(
			AssetPath, ParentNodeId, NodeClass, AfterChildNodeId));
	}

	IClaireonTool::FToolResult CANodeMutationSpec_RemoveNode(
		const FString& AssetPath, const FString& NodeId, bool bForceRootClear = false)
	{
		FClaireonCameraAssetTool_RemoveNode Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetNumberField(TEXT("rig_index"), 0);
		Args->SetStringField(TEXT("node_id"), NodeId);
		Args->SetBoolField(TEXT("force_root_clear"), bForceRootClear);
		return Tool.Execute(Args);
	}

	IClaireonTool::FToolResult CANodeMutationSpec_MoveNode(
		const FString& AssetPath,
		const FString& NodeId,
		const FString& NewParentId,
		const FString& AfterChildNodeId = FString())
	{
		FClaireonCameraAssetTool_MoveNode Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetNumberField(TEXT("rig_index"), 0);
		Args->SetStringField(TEXT("node_id"), NodeId);
		Args->SetStringField(TEXT("new_parent_id"), NewParentId);
		if (!AfterChildNodeId.IsEmpty())
		{
			Args->SetStringField(TEXT("after_child_node_id"), AfterChildNodeId);
		}
		return Tool.Execute(Args);
	}

	IClaireonTool::FToolResult CANodeMutationSpec_SetNodeProperty(
		const FString& AssetPath,
		const FString& NodeId,
		const FString& PropertyPath,
		const FString& Value)
	{
		FClaireonCameraAssetTool_SetNodeProperty Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetNumberField(TEXT("rig_index"), 0);
		Args->SetStringField(TEXT("node_id"), NodeId);
		Args->SetStringField(TEXT("property_path"), PropertyPath);
		Args->SetStringField(TEXT("value"), Value);
		return Tool.Execute(Args);
	}

	IClaireonTool::FToolResult CANodeMutationSpec_GetNodeProperty(
		const FString& AssetPath,
		const FString& NodeId,
		const FString& PropertyPath)
	{
		FClaireonCameraAssetTool_GetNodeProperty Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetNumberField(TEXT("rig_index"), 0);
		Args->SetStringField(TEXT("node_id"), NodeId);
		Args->SetStringField(TEXT("property_path"), PropertyPath);
		return Tool.Execute(Args);
	}

	/** Run ListNodes and return the parsed entries array, or nullptr on failure. */
	const TArray<TSharedPtr<FJsonValue>>* CANodeMutationSpec_ListNodes(
		FAutomationTestBase& Test,
		const FString& AssetPath,
		IClaireonTool::FToolResult& OutResultStorage)
	{
		FClaireonCameraAssetTool_ListNodes Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetNumberField(TEXT("rig_index"), 0);
		OutResultStorage = Tool.Execute(Args);
		if (OutResultStorage.bIsError)
		{
			Test.AddError(FString::Printf(TEXT("ListNodes failed: %s"), *OutResultStorage.ErrorMessage));
			return nullptr;
		}
		if (!OutResultStorage.Data.IsValid())
		{
			Test.AddError(TEXT("ListNodes returned no data"));
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>* Out = nullptr;
		if (!OutResultStorage.Data->TryGetArrayField(TEXT("nodes"), Out) || !Out)
		{
			Test.AddError(TEXT("ListNodes result missing 'nodes' array"));
			return nullptr;
		}
		return Out;
	}
} // namespace

// =====================================================================================
// Test: AddRig_PopulatesIndex
// Create asset, call AddRig with name "TestRig", call ListRigs, verify exactly one
// entry exists with rig_index=0 and rig_name="TestRig". Cleanup.
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAssetNodeMutation_AddRig_PopulatesIndex,
	"Claireon.CameraAsset.NodeMutation.AddRig_PopulatesIndex",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCameraAssetNodeMutation_AddRig_PopulatesIndex::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/CA_NodeMutation_AddRig");
	CANodeMutationSpec_DeleteIfExists(Path);

	// 1. Create asset.
	{
		FClaireonCameraAssetTool_Create Tool;
		const auto Result = Tool.Execute(CANodeMutationSpec_StringArg(TEXT("asset_path"), Path));
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Create failed: %s"), *Result.ErrorMessage));
			return false;
		}
	}

	// 2. AddRig.
	{
		FClaireonCameraAssetTool_AddRig Tool;
		const auto Result = Tool.Execute(CANodeMutationSpec_TwoStringArgs(
			TEXT("asset_path"), Path,
			TEXT("rig_name"), TEXT("TestRig")));
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("AddRig failed: %s"), *Result.ErrorMessage));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
		if (!Result.Data.IsValid())
		{
			AddError(TEXT("AddRig returned no data"));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
		double RigIndex = -1.0;
		if (!Result.Data->TryGetNumberField(TEXT("rig_index"), RigIndex) || static_cast<int32>(RigIndex) != 0)
		{
			AddError(FString::Printf(TEXT("AddRig returned rig_index=%d, expected 0"),
				static_cast<int32>(RigIndex)));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
	}

	// 3. ListRigs.
	{
		FClaireonCameraAssetTool_ListRigs Tool;
		const auto Result = Tool.Execute(CANodeMutationSpec_StringArg(TEXT("asset_path"), Path));
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("ListRigs failed: %s"), *Result.ErrorMessage));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
		if (!Result.Data.IsValid())
		{
			AddError(TEXT("ListRigs returned no data"));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* RigsArrayPtr = nullptr;
		if (!Result.Data->TryGetArrayField(TEXT("rigs"), RigsArrayPtr) || !RigsArrayPtr)
		{
			AddError(TEXT("ListRigs result missing 'rigs' array"));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
		if (RigsArrayPtr->Num() != 1)
		{
			AddError(FString::Printf(TEXT("Expected 1 rig; got %d"), RigsArrayPtr->Num()));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
		const TSharedPtr<FJsonObject>& Entry = (*RigsArrayPtr)[0]->AsObject();
		if (!Entry.IsValid())
		{
			AddError(TEXT("Rig entry not a JSON object"));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
		double EntryIndex = -1.0;
		Entry->TryGetNumberField(TEXT("rig_index"), EntryIndex);
		if (static_cast<int32>(EntryIndex) != 0)
		{
			AddError(FString::Printf(TEXT("ListRigs[0].rig_index=%d, expected 0"),
				static_cast<int32>(EntryIndex)));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
		FString EntryName;
		Entry->TryGetStringField(TEXT("rig_name"), EntryName);
		if (EntryName != TEXT("TestRig"))
		{
			AddError(FString::Printf(TEXT("ListRigs[0].rig_name='%s', expected 'TestRig'"),
				*EntryName));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
	}

	CANodeMutationSpec_DeleteIfExists(Path);
	return true;
}

// =====================================================================================
// Test: AddNode_AsRoot
// Empty rig -> AddNode parent_node_id="" class UArrayCameraNode -> ListNodes returns
// exactly one entry at "Root".
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAssetNodeMutation_AddNode_AsRoot,
	"Claireon.CameraAsset.NodeMutation.AddNode_AsRoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCameraAssetNodeMutation_AddNode_AsRoot::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/CA_NodeMutation_AddRoot");
	CANodeMutationSpec_DeleteIfExists(Path);

	if (!CANodeMutationSpec_CreateAssetAndRig(*this, Path))
	{
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	const auto AddResult = CANodeMutationSpec_AddNode(Path, FString(), TEXT("ArrayCameraNode"));
	if (AddResult.bIsError)
	{
		AddError(FString::Printf(TEXT("AddNode failed: %s"), *AddResult.ErrorMessage));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}
	FString ReturnedId;
	if (!AddResult.Data.IsValid() || !AddResult.Data->TryGetStringField(TEXT("node_id"), ReturnedId))
	{
		AddError(TEXT("AddNode result missing 'node_id'"));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}
	if (ReturnedId != TEXT("Root"))
	{
		AddError(FString::Printf(TEXT("Expected node_id='Root'; got '%s'"), *ReturnedId));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	IClaireonTool::FToolResult ListStore;
	const TArray<TSharedPtr<FJsonValue>>* Nodes = CANodeMutationSpec_ListNodes(*this, Path, ListStore);
	if (!Nodes)
	{
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}
	if (Nodes->Num() != 1)
	{
		AddError(FString::Printf(TEXT("Expected 1 node; got %d"), Nodes->Num()));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}
	FString FirstId;
	(*Nodes)[0]->AsObject()->TryGetStringField(TEXT("node_id"), FirstId);
	if (FirstId != TEXT("Root"))
	{
		AddError(FString::Printf(TEXT("ListNodes[0].node_id='%s', expected 'Root'"), *FirstId));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	CANodeMutationSpec_DeleteIfExists(Path);
	return true;
}

// =====================================================================================
// Test: AddNode_AsChildOfArray
// Array root + AddNode parent_node_id="Root" class UCustomLookAtCameraNode -> ListNodes
// returns 2 entries; child is at "Root.Children[0]".
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAssetNodeMutation_AddNode_AsChildOfArray,
	"Claireon.CameraAsset.NodeMutation.AddNode_AsChildOfArray",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCameraAssetNodeMutation_AddNode_AsChildOfArray::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/CA_NodeMutation_AddChild");
	CANodeMutationSpec_DeleteIfExists(Path);

	if (!CANodeMutationSpec_CreateAssetAndRig(*this, Path))
	{
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	{
		const auto R = CANodeMutationSpec_AddNode(Path, FString(), TEXT("ArrayCameraNode"));
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("AddNode(root) failed: %s"), *R.ErrorMessage));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
	}
	const auto ChildResult = CANodeMutationSpec_AddNode(Path, TEXT("Root"), TEXT("CustomLookAtCameraNode"));
	if (ChildResult.bIsError)
	{
		AddError(FString::Printf(TEXT("AddNode(child) failed: %s"), *ChildResult.ErrorMessage));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}
	FString ReturnedId;
	ChildResult.Data->TryGetStringField(TEXT("node_id"), ReturnedId);
	if (ReturnedId != TEXT("Root.Children[0]"))
	{
		AddError(FString::Printf(TEXT("Expected child node_id='Root.Children[0]'; got '%s'"), *ReturnedId));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	IClaireonTool::FToolResult ListStore;
	const TArray<TSharedPtr<FJsonValue>>* Nodes = CANodeMutationSpec_ListNodes(*this, Path, ListStore);
	if (!Nodes)
	{
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}
	if (Nodes->Num() != 2)
	{
		AddError(FString::Printf(TEXT("Expected 2 nodes; got %d"), Nodes->Num()));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	CANodeMutationSpec_DeleteIfExists(Path);
	return true;
}

// =====================================================================================
// Test: AddNode_NonArrayParentRejects
// Add UCustomPostProcessCameraNode as root -> try AddNode under it -> error containing
// "no array child slot".
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAssetNodeMutation_AddNode_NonArrayParentRejects,
	"Claireon.CameraAsset.NodeMutation.AddNode_NonArrayParentRejects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCameraAssetNodeMutation_AddNode_NonArrayParentRejects::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/CA_NodeMutation_NonArrayRejects");
	CANodeMutationSpec_DeleteIfExists(Path);

	if (!CANodeMutationSpec_CreateAssetAndRig(*this, Path))
	{
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	{
		const auto R = CANodeMutationSpec_AddNode(Path, FString(), TEXT("CustomPostProcessCameraNode"));
		if (R.bIsError)
		{
			AddError(FString::Printf(TEXT("AddNode(root) failed: %s"), *R.ErrorMessage));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
	}

	const auto Result = CANodeMutationSpec_AddNode(Path, TEXT("Root"), TEXT("CustomLookAtCameraNode"));
	if (!Result.bIsError)
	{
		AddError(TEXT("AddNode under non-array parent did not error"));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}
	if (!Result.ErrorMessage.Contains(TEXT("no array child slot")))
	{
		AddError(FString::Printf(
			TEXT("Error did not mention 'no array child slot'; got: %s"),
			*Result.ErrorMessage));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	CANodeMutationSpec_DeleteIfExists(Path);
	return true;
}

// =====================================================================================
// Test: RemoveNode_ChildOfArray
// Two children under array root -> remove Root.Children[0] -> ListNodes returns one
// child now at Root.Children[0] (formerly idx 1).
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAssetNodeMutation_RemoveNode_ChildOfArray,
	"Claireon.CameraAsset.NodeMutation.RemoveNode_ChildOfArray",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCameraAssetNodeMutation_RemoveNode_ChildOfArray::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/CA_NodeMutation_RemoveChild");
	CANodeMutationSpec_DeleteIfExists(Path);

	if (!CANodeMutationSpec_CreateAssetAndRig(*this, Path))
	{
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	if (CANodeMutationSpec_AddNode(Path, FString(), TEXT("ArrayCameraNode")).bIsError || CANodeMutationSpec_AddNode(Path, TEXT("Root"), TEXT("CustomLookAtCameraNode")).bIsError || CANodeMutationSpec_AddNode(Path, TEXT("Root"), TEXT("CustomPostProcessCameraNode")).bIsError)
	{
		AddError(TEXT("Setup AddNode call(s) failed"));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	const auto RemoveResult = CANodeMutationSpec_RemoveNode(Path, TEXT("Root.Children[0]"));
	if (RemoveResult.bIsError)
	{
		AddError(FString::Printf(TEXT("RemoveNode failed: %s"), *RemoveResult.ErrorMessage));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	IClaireonTool::FToolResult ListStore;
	const TArray<TSharedPtr<FJsonValue>>* Nodes = CANodeMutationSpec_ListNodes(*this, Path, ListStore);
	if (!Nodes)
	{
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}
	if (Nodes->Num() != 2) // root + 1 surviving child
	{
		AddError(FString::Printf(TEXT("Expected 2 nodes after remove; got %d"), Nodes->Num()));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	// Find the child entry; it should now be at Root.Children[0] and class
	// CustomPostProcessCameraNode (the surviving sibling).
	bool bFoundChild = false;
	for (const TSharedPtr<FJsonValue>& V : *Nodes)
	{
		const TSharedPtr<FJsonObject>& Entry = V->AsObject();
		FString Id;
		Entry->TryGetStringField(TEXT("node_id"), Id);
		if (Id == TEXT("Root.Children[0]"))
		{
			FString Cls;
			Entry->TryGetStringField(TEXT("class"), Cls);
			if (Cls != TEXT("CustomPostProcessCameraNode"))
			{
				AddError(FString::Printf(
					TEXT("Surviving child class='%s', expected 'CustomPostProcessCameraNode'"),
					*Cls));
				CANodeMutationSpec_DeleteIfExists(Path);
				return false;
			}
			bFoundChild = true;
		}
	}
	if (!bFoundChild)
	{
		AddError(TEXT("Did not find a child entry at Root.Children[0] after remove"));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	CANodeMutationSpec_DeleteIfExists(Path);
	return true;
}

// =====================================================================================
// Test: MoveNode_ReorderWithinParent
// Three children under array root -> move Root.Children[2] to after_child_node_id=
// "Root.Children[0]". Implementation semantics: detach Children[2] (array becomes
// [orig0, orig1]), then insert at index AfterIdx+1 = 1 -> [orig0, orig2, orig1].
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAssetNodeMutation_MoveNode_ReorderWithinParent,
	"Claireon.CameraAsset.NodeMutation.MoveNode_ReorderWithinParent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCameraAssetNodeMutation_MoveNode_ReorderWithinParent::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/CA_NodeMutation_MoveReorder");
	CANodeMutationSpec_DeleteIfExists(Path);

	if (!CANodeMutationSpec_CreateAssetAndRig(*this, Path))
	{
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	// Add root + three distinct-class children so we can identify them post-move.
	if (CANodeMutationSpec_AddNode(Path, FString(), TEXT("ArrayCameraNode")).bIsError || CANodeMutationSpec_AddNode(Path, TEXT("Root"), TEXT("CustomLookAtCameraNode")).bIsError || // orig0
		CANodeMutationSpec_AddNode(Path, TEXT("Root"), TEXT("CustomPostProcessCameraNode")).bIsError ||																				// orig1
		CANodeMutationSpec_AddNode(Path, TEXT("Root"), TEXT("CustomPlayCameraAnimationCameraNode")).bIsError)																		// orig2
	{
		AddError(TEXT("Setup AddNode call(s) failed"));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	const auto MoveResult = CANodeMutationSpec_MoveNode(
		Path, TEXT("Root.Children[2]"), TEXT("Root"), TEXT("Root.Children[0]"));
	if (MoveResult.bIsError)
	{
		AddError(FString::Printf(TEXT("MoveNode failed: %s"), *MoveResult.ErrorMessage));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	IClaireonTool::FToolResult ListStore;
	const TArray<TSharedPtr<FJsonValue>>* Nodes = CANodeMutationSpec_ListNodes(*this, Path, ListStore);
	if (!Nodes)
	{
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}
	if (Nodes->Num() != 4) // root + 3 children
	{
		AddError(FString::Printf(TEXT("Expected 4 nodes; got %d"), Nodes->Num()));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	// Expected final order:
	//   Root.Children[0] = CustomLookAtCameraNode             (orig0, untouched)
	//   Root.Children[1] = CustomPlayCameraAnimationCameraNode (orig2, moved)
	//   Root.Children[2] = CustomPostProcessCameraNode         (orig1, shifted)
	auto FindClassAt = [&](const FString& Id) -> FString
	{
		for (const TSharedPtr<FJsonValue>& V : *Nodes)
		{
			const TSharedPtr<FJsonObject>& Entry = V->AsObject();
			FString EntryId;
			Entry->TryGetStringField(TEXT("node_id"), EntryId);
			if (EntryId == Id)
			{
				FString Cls;
				Entry->TryGetStringField(TEXT("class"), Cls);
				return Cls;
			}
		}
		return FString();
	};

	const FString At0 = FindClassAt(TEXT("Root.Children[0]"));
	const FString At1 = FindClassAt(TEXT("Root.Children[1]"));
	const FString At2 = FindClassAt(TEXT("Root.Children[2]"));

	if (At0 != TEXT("CustomLookAtCameraNode") || At1 != TEXT("CustomPlayCameraAnimationCameraNode") || At2 != TEXT("CustomPostProcessCameraNode"))
	{
		AddError(FString::Printf(
			TEXT("Post-move order mismatch: [0]='%s' [1]='%s' [2]='%s' (expected LookAt, PlayCameraAnimation, PostProcess)"),
			*At0, *At1, *At2));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	CANodeMutationSpec_DeleteIfExists(Path);
	return true;
}

// =====================================================================================
// Test: SetGetNodeProperty_RoundTrip
// Array root + UCustomLookAtCameraNode child -> SetNodeProperty(InterpSpeed=7.5) ->
// GetNodeProperty returns "7.5". Save + unload + reload -> GetNodeProperty STILL
// returns "7.5". Verifies the PostEditChange cascade dirties the package and the
// value persists across serialization.
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAssetNodeMutation_SetGetNodeProperty_RoundTrip,
	"Claireon.CameraAsset.NodeMutation.SetGetNodeProperty_RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCameraAssetNodeMutation_SetGetNodeProperty_RoundTrip::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/CA_PropMut");
	CANodeMutationSpec_DeleteIfExists(Path);

	if (!CANodeMutationSpec_CreateAssetAndRig(*this, Path))
	{
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	if (CANodeMutationSpec_AddNode(Path, FString(), TEXT("ArrayCameraNode")).bIsError || CANodeMutationSpec_AddNode(Path, TEXT("Root"), TEXT("CustomLookAtCameraNode")).bIsError)
	{
		AddError(TEXT("Setup AddNode call(s) failed"));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	const FString ChildId = TEXT("Root.Children[0]");
	const FString PropName = TEXT("InterpSpeed");
	const FString TargetValue = TEXT("7.5");

	{
		const auto SetResult = CANodeMutationSpec_SetNodeProperty(Path, ChildId, PropName, TargetValue);
		if (SetResult.bIsError)
		{
			AddError(FString::Printf(TEXT("SetNodeProperty failed: %s"), *SetResult.ErrorMessage));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
	}

	{
		const auto GetResult = CANodeMutationSpec_GetNodeProperty(Path, ChildId, PropName);
		if (GetResult.bIsError)
		{
			AddError(FString::Printf(TEXT("GetNodeProperty(pre-save) failed: %s"), *GetResult.ErrorMessage));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
		FString GotValue;
		if (!GetResult.Data.IsValid() || !GetResult.Data->TryGetStringField(TEXT("value"), GotValue))
		{
			AddError(TEXT("GetNodeProperty(pre-save) result missing 'value'"));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
		if (GotValue != TargetValue)
		{
			AddError(FString::Printf(
				TEXT("Pre-save value mismatch: got '%s', expected '%s'"),
				*GotValue, *TargetValue));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
	}

	// Persist + force reload by unloading the package; subsequent LoadObject
	// inside the tool will re-pull the package from disk, which is the
	// persistence contract we want to exercise.
	if (!UEditorAssetLibrary::SaveAsset(Path, /*bOnlyIfIsDirty=*/false))
	{
		AddError(FString::Printf(TEXT("SaveAsset('%s') returned false"), *Path));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}
	if (UPackage* Pkg = FindPackage(nullptr, *Path))
	{
		FText UnloadErr;
		const bool bUnloaded = UPackageTools::UnloadPackages({ Pkg }, UnloadErr, /*bUnloadDirtyPackages=*/true);
		if (!bUnloaded)
		{
			AddError(FString::Printf(TEXT("UnloadPackages failed: %s"), *UnloadErr.ToString()));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
	}

	{
		const auto GetResult = CANodeMutationSpec_GetNodeProperty(Path, ChildId, PropName);
		if (GetResult.bIsError)
		{
			AddError(FString::Printf(TEXT("GetNodeProperty(post-reload) failed: %s"), *GetResult.ErrorMessage));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
		FString GotValue;
		if (!GetResult.Data.IsValid() || !GetResult.Data->TryGetStringField(TEXT("value"), GotValue))
		{
			AddError(TEXT("GetNodeProperty(post-reload) result missing 'value'"));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
		if (GotValue != TargetValue)
		{
			AddError(FString::Printf(
				TEXT("Persistence failed: post-reload value '%s', expected '%s'"),
				*GotValue, *TargetValue));
			CANodeMutationSpec_DeleteIfExists(Path);
			return false;
		}
	}

	CANodeMutationSpec_DeleteIfExists(Path);
	return true;
}

// =====================================================================================
// Test: SetNodeProperty_BadPath
// Array root + UCustomLookAtCameraNode child -> SetNodeProperty(BogusProperty=1) ->
// expect bIsError=true. Then verify the node is unchanged by reading another
// known property (InterpSpeed, default 5.0) and confirming the default value.
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAssetNodeMutation_SetNodeProperty_BadPath,
	"Claireon.CameraAsset.NodeMutation.SetNodeProperty_BadPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCameraAssetNodeMutation_SetNodeProperty_BadPath::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/CA_PropErr");
	CANodeMutationSpec_DeleteIfExists(Path);

	if (!CANodeMutationSpec_CreateAssetAndRig(*this, Path))
	{
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	if (CANodeMutationSpec_AddNode(Path, FString(), TEXT("ArrayCameraNode")).bIsError || CANodeMutationSpec_AddNode(Path, TEXT("Root"), TEXT("CustomLookAtCameraNode")).bIsError)
	{
		AddError(TEXT("Setup AddNode call(s) failed"));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	const FString ChildId = TEXT("Root.Children[0]");

	const auto SetResult = CANodeMutationSpec_SetNodeProperty(Path, ChildId, TEXT("BogusProperty"), TEXT("1"));
	if (!SetResult.bIsError)
	{
		AddError(TEXT("SetNodeProperty with bogus path did not error"));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	// Verify a real property is untouched (UCustomLookAtCameraNode::InterpSpeed default = 5.0f).
	const auto GetResult = CANodeMutationSpec_GetNodeProperty(Path, ChildId, TEXT("InterpSpeed"));
	if (GetResult.bIsError)
	{
		AddError(FString::Printf(TEXT("GetNodeProperty(InterpSpeed) failed: %s"), *GetResult.ErrorMessage));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}
	FString GotValue;
	if (!GetResult.Data.IsValid() || !GetResult.Data->TryGetStringField(TEXT("value"), GotValue))
	{
		AddError(TEXT("GetNodeProperty(InterpSpeed) result missing 'value'"));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}
	// ExportText for floats may render "5.0", "5.000000", etc. Convert + compare numerically.
	const float Numeric = FCString::Atof(*GotValue);
	if (!FMath::IsNearlyEqual(Numeric, 5.0f))
	{
		AddError(FString::Printf(
			TEXT("InterpSpeed unexpectedly changed after bad-path write: '%s' (-> %f, expected 5.0)"),
			*GotValue, Numeric));
		CANodeMutationSpec_DeleteIfExists(Path);
		return false;
	}

	CANodeMutationSpec_DeleteIfExists(Path);
	return true;
}
