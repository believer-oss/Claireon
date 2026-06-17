// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCameraAssetTool_MoveNode.h"

#include "ClaireonSessionManager.h"
#include "Dom/JsonObject.h"
#include "ScopedTransaction.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "Tools/ClaireonCameraAssetHelpers.h"

#if WITH_GAMEPLAY_CAMERAS

#include "Core/CameraAsset.h"
#include "Core/CameraNode.h"
#include "Core/CameraRigAsset.h"
#include "Nodes/Common/ArrayCameraNode.h"

#define LOCTEXT_NAMESPACE "ClaireonCameraAssetTool_MoveNode"

FString FClaireonCameraAssetTool_MoveNode::GetOperation() const { return TEXT("move_node"); }

FString FClaireonCameraAssetTool_MoveNode::GetDescription() const
{
	return TEXT("Re-parent or re-order a UCameraNode under a UArrayCameraNode parent within the same rig.");
}

TSharedPtr<FJsonObject> FClaireonCameraAssetTool_MoveNode::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("/Game/ path of the camera asset"), true);
	S.AddInteger(TEXT("rig_index"), TEXT("Index of the rig to mutate"), true);
	S.AddString(TEXT("node_id"), TEXT("Node-id path of the node to move"), true);
	S.AddString(TEXT("new_parent_id"), TEXT("Node-id path of the new array-parent"), true);
	S.AddString(TEXT("after_child_node_id"), TEXT("Sibling node-id to insert after; empty/absent appends"));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonCameraAssetTool_MoveNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	double RigIndexNum = 0.0;
	if (!Arguments->TryGetNumberField(TEXT("rig_index"), RigIndexNum))
	{
		return MakeErrorResult(TEXT("Missing required parameter: rig_index"));
	}
	const int32 RigIndex = static_cast<int32>(RigIndexNum);

	FString NodeId;
	if (!Arguments->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: node_id"));
	}

	FString NewParentId;
	if (!Arguments->TryGetStringField(TEXT("new_parent_id"), NewParentId) || NewParentId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: new_parent_id"));
	}

	FString AfterChildNodeId;
	Arguments->TryGetStringField(TEXT("after_child_node_id"), AfterChildNodeId);

	const FString Canon = FClaireonSessionManager::CanonicalizePath(AssetPath);
	if (Canon.IsEmpty())
	{
		return MakeErrorResult(TEXT("Invalid asset_path (must start with /Game/)"));
	}

	UCameraAsset* Asset = LoadObject<UCameraAsset>(nullptr, *Canon);
	if (!Asset)
	{
		return MakeErrorResult(FString::Printf(TEXT("Camera asset not found: %s"), *Canon));
	}

	const TArray<UCameraRigAsset*> Rigs = ClaireonCameraAssetHelpers::GetCameraRigs(Asset);
	if (!Rigs.IsValidIndex(RigIndex))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("rig_index %d out of bounds (asset has %d rig(s))"),
			RigIndex, Rigs.Num()));
	}
	UCameraRigAsset* Rig = Rigs[RigIndex];
	if (!Rig)
	{
		return MakeErrorResult(FString::Printf(TEXT("Rig at index %d is null"), RigIndex));
	}

	FString ResolveError;
	UCameraNode* Node = ClaireonCameraAssetHelpers::ResolveNode(Rig, NodeId, ResolveError);
	if (!Node)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Failed to resolve node_id '%s': %s"), *NodeId, *ResolveError));
	}

	UCameraNode* NewParent = ClaireonCameraAssetHelpers::ResolveNode(Rig, NewParentId, ResolveError);
	if (!NewParent)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Failed to resolve new_parent_id '%s': %s"),
			*NewParentId, *ResolveError));
	}

	UArrayCameraNode* NewArrayParent = Cast<UArrayCameraNode>(NewParent);
	if (!NewArrayParent)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("new_parent class %s is not UArrayCameraNode; named-slot mutation goes through set_node_property"),
			*NewParent->GetClass()->GetName()));
	}

	if (Node == NewParent)
	{
		return MakeErrorResult(TEXT("Cannot move a node into itself"));
	}

	ClaireonCameraAssetHelpers::CloseEditorToolkitForAsset(Asset);

	FScopedTransaction Transaction(LOCTEXT("MoveCameraNode", "[Claireon] Move Camera Node"));
	Asset->Modify();
	Rig->Modify();

	const bool bIsRoot = (Node == Rig->RootNode);

	// 1. Detach from current parent.
	if (bIsRoot)
	{
		Rig->RootNode = nullptr;
	}
	else
	{
		const FString CurParentPath = ClaireonCameraAssetHelpers::GetParentPath(NodeId);
		if (CurParentPath.IsEmpty())
		{
			Transaction.Cancel();
			return MakeErrorResult(FString::Printf(
				TEXT("Could not derive parent path from node_id '%s'"), *NodeId));
		}

		FString CurParentError;
		UCameraNode* CurParent = ClaireonCameraAssetHelpers::ResolveNode(Rig, CurParentPath, CurParentError);
		if (!CurParent)
		{
			Transaction.Cancel();
			return MakeErrorResult(FString::Printf(
				TEXT("Failed to resolve current parent '%s': %s"),
				*CurParentPath, *CurParentError));
		}
		UArrayCameraNode* CurArrayParent = Cast<UArrayCameraNode>(CurParent);
		if (!CurArrayParent)
		{
			Transaction.Cancel();
			return MakeErrorResult(FString::Printf(
				TEXT("cannot move from named slot on class %s; use set_node_property"),
				*CurParent->GetClass()->GetName()));
		}
		CurArrayParent->Modify();
		const int32 RemovedCount = CurArrayParent->Children.Remove(Node);
		if (RemovedCount == 0)
		{
			Transaction.Cancel();
			return MakeErrorResult(FString::Printf(
				TEXT("Node '%s' was not found in current parent's Children array"), *NodeId));
		}
	}

	// 2. Re-parent the UObject so Outer is the owning rig (matches engine pattern
	// in GameplayCamerasTestBuilder where UArrayCameraNode children are Outer'd
	// to the rig, not the array parent). REN_DontCreateRedirectors avoids a
	// UObjectRedirector being created for the move.
	Node->Rename(nullptr, Rig, REN_DontCreateRedirectors);

	// 3. Insert into new parent.
	NewArrayParent->Modify();
	int32 InsertIdx = NewArrayParent->Children.Num(); // append by default
	if (!AfterChildNodeId.IsEmpty())
	{
		const int32 AfterIdx = ClaireonCameraAssetHelpers::ParseTrailingIndex(AfterChildNodeId);
		if (AfterIdx < 0)
		{
			Transaction.Cancel();
			return MakeErrorResult(FString::Printf(
				TEXT("after_child_node_id '%s' has no trailing [N] index"),
				*AfterChildNodeId));
		}
		InsertIdx = AfterIdx + 1;
	}
	InsertIdx = FMath::Clamp(InsertIdx, 0, NewArrayParent->Children.Num());
	NewArrayParent->Children.Insert(Node, InsertIdx);

	// 4. PostEditChange cascade.
	Node->PostEditChange();
	Rig->PostEditChange();
	Asset->PostEditChange();

	const FString FinalNodeId = ClaireonCameraAssetHelpers::ComputeNodeIdForChildOf(NewParentId, InsertIdx);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), FinalNodeId);
	return MakeSuccessResult(Data,
		FString::Printf(TEXT("Moved %s -> %s on rig %d of %s"),
			*NodeId, *FinalNodeId, RigIndex, *Asset->GetPathName()));
}

#undef LOCTEXT_NAMESPACE

#endif // WITH_GAMEPLAY_CAMERAS
