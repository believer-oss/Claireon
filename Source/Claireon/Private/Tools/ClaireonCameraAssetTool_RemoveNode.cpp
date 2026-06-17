// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCameraAssetTool_RemoveNode.h"

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

#define LOCTEXT_NAMESPACE "ClaireonCameraAssetTool_RemoveNode"

FString FClaireonCameraAssetTool_RemoveNode::GetOperation() const { return TEXT("remove_node"); }

FString FClaireonCameraAssetTool_RemoveNode::GetDescription() const
{
	return TEXT("Remove a UCameraNode from a rig (from a UArrayCameraNode parent's Children, or clear a rig root with force_root_clear).");
}

TSharedPtr<FJsonObject> FClaireonCameraAssetTool_RemoveNode::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("/Game/ path of the camera asset"), true);
	S.AddInteger(TEXT("rig_index"), TEXT("Index of the rig to mutate"), true);
	S.AddString(TEXT("node_id"), TEXT("Node-id path of the node to remove"), true);
	S.AddBoolean(TEXT("force_root_clear"), TEXT("Permit clearing the rig's root node (default false)"));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonCameraAssetTool_RemoveNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	bool bForceRootClear = false;
	Arguments->TryGetBoolField(TEXT("force_root_clear"), bForceRootClear);

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

	const bool bIsRoot = (Node == Rig->RootNode);
	if (bIsRoot && !bForceRootClear)
	{
		return MakeErrorResult(TEXT("refusing to clear rig root; pass force_root_clear=true if intended"));
	}

	ClaireonCameraAssetHelpers::CloseEditorToolkitForAsset(Asset);

	FScopedTransaction Transaction(LOCTEXT("RemoveCameraNode", "[Claireon] Remove Camera Node"));
	Asset->Modify();
	Rig->Modify();

	if (bIsRoot)
	{
		Rig->RootNode = nullptr;
		Node->MarkAsGarbage();
	}
	else
	{
		const FString ParentPath = ClaireonCameraAssetHelpers::GetParentPath(NodeId);
		if (ParentPath.IsEmpty())
		{
			Transaction.Cancel();
			return MakeErrorResult(FString::Printf(
				TEXT("Could not derive parent path from node_id '%s'"), *NodeId));
		}

		FString ParentResolveError;
		UCameraNode* Parent = ClaireonCameraAssetHelpers::ResolveNode(Rig, ParentPath, ParentResolveError);
		if (!Parent)
		{
			Transaction.Cancel();
			return MakeErrorResult(FString::Printf(
				TEXT("Failed to resolve parent path '%s': %s"),
				*ParentPath, *ParentResolveError));
		}

		UArrayCameraNode* ArrayParent = Cast<UArrayCameraNode>(Parent);
		if (!ArrayParent)
		{
			Transaction.Cancel();
			return MakeErrorResult(FString::Printf(
				TEXT("cannot remove from named slot on class %s; use set_node_property to clear the slot"),
				*Parent->GetClass()->GetName()));
		}

		ArrayParent->Modify();
		const int32 RemovedCount = ArrayParent->Children.Remove(Node);
		if (RemovedCount == 0)
		{
			Transaction.Cancel();
			return MakeErrorResult(FString::Printf(
				TEXT("Node '%s' was not found in parent's Children array"), *NodeId));
		}
		Node->MarkAsGarbage();
	}

	Rig->PostEditChange();
	Asset->PostEditChange();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("success"), true);
	return MakeSuccessResult(Data,
		FString::Printf(TEXT("Removed %s from rig %d of %s"),
			*NodeId, RigIndex, *Asset->GetPathName()));
}

#undef LOCTEXT_NAMESPACE

#endif // WITH_GAMEPLAY_CAMERAS
