// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCameraAssetTool_AddNode.h"

#include "ClaireonSessionManager.h"
#include "Dom/JsonObject.h"
#include "ScopedTransaction.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "Tools/ClaireonCameraAssetHelpers.h"
#include "Tools/ClaireonPropertyUtils.h"

#if WITH_GAMEPLAY_CAMERAS

#include "Core/CameraAsset.h"
#include "Core/CameraNode.h"
#include "Core/CameraRigAsset.h"
#include "Nodes/Common/ArrayCameraNode.h"

#define LOCTEXT_NAMESPACE "ClaireonCameraAssetTool_AddNode"

FString FClaireonCameraAssetTool_AddNode::GetOperation() const { return TEXT("add_node"); }

FString FClaireonCameraAssetTool_AddNode::GetDescription() const
{
	return TEXT("Add a UCameraNode subclass instance to a rig (as root or as a child of an existing array-parent node).");
}

TSharedPtr<FJsonObject> FClaireonCameraAssetTool_AddNode::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("/Game/ path of the camera asset"), true);
	S.AddInteger(TEXT("rig_index"), TEXT("Index of the rig to mutate"), true);
	S.AddString(TEXT("parent_node_id"), TEXT("Node-id path to the parent node; empty to set the rig root"));
	S.AddString(TEXT("node_class"), TEXT("UCameraNode subclass name (e.g. 'BVLookAtCameraNode')"), true);
	S.AddString(TEXT("after_child_node_id"), TEXT("Sibling node-id to insert after; empty/absent appends"));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonCameraAssetTool_AddNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString NodeClassName;
	if (!Arguments->TryGetStringField(TEXT("node_class"), NodeClassName) || NodeClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: node_class"));
	}

	FString ParentNodeId;
	Arguments->TryGetStringField(TEXT("parent_node_id"), ParentNodeId);

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

	UClass* NodeClass = ClaireonCameraAssetHelpers::ResolveNodeClass(NodeClassName);
	if (!NodeClass)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Unknown class '%s' (must be a concrete UCameraNode subclass)"),
			*NodeClassName));
	}
	if (NodeClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Class '%s' is abstract or deprecated"), *NodeClassName));
	}

	ClaireonCameraAssetHelpers::CloseEditorToolkitForAsset(Asset);

	FScopedTransaction Transaction(LOCTEXT("AddCameraNode", "[Claireon] Add Camera Node"));
	Asset->Modify();
	Rig->Modify();

	UCameraNode* NewNode = nullptr;
	FString NewNodeId;

	if (ParentNodeId.IsEmpty())
	{
		if (Rig->RootNode != nullptr)
		{
			Transaction.Cancel();
			return MakeErrorResult(TEXT("rig already has a root; pass parent_node_id explicitly or use remove_node first"));
		}

		// RootNode IS UPROPERTY(Instanced) — SetInstancedSubObject works here.
		FString HelperError;
		UObject* Created = ClaireonPropertyUtils::SetInstancedSubObject(
			Rig, NodeClass, TEXT("RootNode"), HelperError);
		if (!Created)
		{
			Transaction.Cancel();
			return MakeErrorResult(FString::Printf(
				TEXT("SetInstancedSubObject(RootNode) failed: %s"), *HelperError));
		}
		NewNode = Cast<UCameraNode>(Created);
		if (!NewNode)
		{
			Transaction.Cancel();
			return MakeErrorResult(TEXT("Internal error: SetInstancedSubObject returned a non-UCameraNode"));
		}
		NewNodeId = TEXT("Root");
	}
	else
	{
		FString ResolveError;
		UCameraNode* Parent = ClaireonCameraAssetHelpers::ResolveNode(Rig, ParentNodeId, ResolveError);
		if (!Parent)
		{
			Transaction.Cancel();
			return MakeErrorResult(FString::Printf(
				TEXT("Failed to resolve parent_node_id '%s': %s"),
				*ParentNodeId, *ResolveError));
		}

		UArrayCameraNode* ArrayParent = Cast<UArrayCameraNode>(Parent);
		if (!ArrayParent)
		{
			Transaction.Cancel();
			return MakeErrorResult(FString::Printf(
				TEXT("class %s has no array child slot; use set_node_property for named-slot mutation"),
				*Parent->GetClass()->GetName()));
		}

		// UArrayCameraNode::Children is a public TArray<TObjectPtr<UCameraNode>> but is
		// NOT marked UPROPERTY(Instanced), so CreateInstancedArrayElement (which
		// validates CPF_InstancedReference) refuses it. We mutate the public Children
		// array directly. Outer for the new sub-object is the owning UCameraRigAsset
		// (matches the engine's GameplayCamerasTestBuilder pattern at
		// CastChecked<UArrayCameraNode>(CameraNode)->Children.Add(...) with the new
		// node Outer'd to CameraNode->GetOuter() i.e. the rig). Outer'ing to the
		// array parent itself crashes with SEH 0xC0000005 during NewObject because
		// the engine's UCameraNode CDO/factory pipeline doesn't expect a non-rig Outer.
		ArrayParent->Modify();

		UCameraNode* Created = NewObject<UCameraNode>(
			Rig, NodeClass, NAME_None, RF_Transactional);
		if (!Created)
		{
			Transaction.Cancel();
			return MakeErrorResult(TEXT("NewObject<UCameraNode> returned null"));
		}

		ArrayParent->Children.Add(Created);
		int32 FinalIdx = ArrayParent->Children.Num() - 1;

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
			const int32 TargetIdx = AfterIdx + 1;
			if (TargetIdx < 0 || TargetIdx > ArrayParent->Children.Num() - 1)
			{
				// Already at end (TargetIdx >= last); leave as-is.
			}
			else if (TargetIdx != FinalIdx)
			{
				// Move from end (FinalIdx) to TargetIdx.
				TObjectPtr<UCameraNode> Held = ArrayParent->Children[FinalIdx];
				ArrayParent->Children.RemoveAt(FinalIdx);
				ArrayParent->Children.Insert(Held, TargetIdx);
				FinalIdx = TargetIdx;
			}
		}

		NewNode = Created;
		NewNodeId = ClaireonCameraAssetHelpers::ComputeNodeIdForChildOf(ParentNodeId, FinalIdx);
	}

	// PostEditChange cascade: leaf -> rig -> asset. None of these propagate up,
	// so the explicit cascade is required for derived state and editor caches.
	NewNode->PostEditChange();
	Rig->PostEditChange();
	Asset->PostEditChange();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("node_id"), NewNodeId);
	Data->SetStringField(TEXT("class"), NewNode->GetClass()->GetName());
	return MakeSuccessResult(Data,
		FString::Printf(TEXT("Added %s at %s on rig %d of %s"),
			*NewNode->GetClass()->GetName(), *NewNodeId, RigIndex, *Asset->GetPathName()));
}

#undef LOCTEXT_NAMESPACE

#endif // WITH_GAMEPLAY_CAMERAS
