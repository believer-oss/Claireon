// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCameraAssetTool_SetNodeProperty.h"

#include "ClaireonSessionManager.h"
#include "Core/CameraAsset.h"
#include "Core/CameraNode.h"
#include "Core/CameraRigAsset.h"
#include "Dom/JsonObject.h"
#include "ScopedTransaction.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "Tools/ClaireonCameraAssetHelpers.h"
#include "Tools/ClaireonPropertyUtils.h"

#define LOCTEXT_NAMESPACE "ClaireonCameraAssetTool_SetNodeProperty"

FString FClaireonCameraAssetTool_SetNodeProperty::GetOperation() const { return TEXT("set_node_property"); }

FString FClaireonCameraAssetTool_SetNodeProperty::GetDescription() const
{
	return TEXT("Write a UPROPERTY on a UCameraNode by dotted property path; wraps the change in a transaction and runs the PostEditChange cascade.");
}

TSharedPtr<FJsonObject> FClaireonCameraAssetTool_SetNodeProperty::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("/Game/ path of the camera asset"), true);
	S.AddInteger(TEXT("rig_index"), TEXT("Index of the rig containing the node"), true);
	S.AddString(TEXT("node_id"), TEXT("Node-id path of the target node"), true);
	S.AddString(TEXT("property_path"), TEXT("Dotted property path (e.g. 'InterpSpeed' or 'BlendSettings.Easing')"), true);
	S.AddString(TEXT("value"), TEXT("New value as exported text (parsed via ImportText_Direct)"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonCameraAssetTool_SetNodeProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString PropertyPath;
	if (!Arguments->TryGetStringField(TEXT("property_path"), PropertyPath) || PropertyPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_path"));
	}

	FString ValueArg;
	if (!Arguments->TryGetStringField(TEXT("value"), ValueArg))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

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

	TArrayView<const TObjectPtr<UCameraRigAsset>> Rigs = Asset->GetCameraRigs();
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

	ClaireonCameraAssetHelpers::CloseEditorToolkitForAsset(Asset);

	FScopedTransaction Transaction(LOCTEXT("SetNodeProperty", "[Claireon] Set Node Property"));
	Asset->Modify();
	Rig->Modify();
	Node->Modify();

	FString WriteError;
	if (!ClaireonPropertyUtils::WritePropertyByPath(Node, PropertyPath, ValueArg, WriteError))
	{
		Transaction.Cancel();
		return MakeErrorResult(FString::Printf(
			TEXT("WritePropertyByPath('%s') failed: %s"), *PropertyPath, *WriteError));
	}

	// PostEditChange cascade: leaf -> rig -> asset. None of these propagate up,
	// so the explicit cascade is required for derived state and editor caches.
	Node->PostEditChange();
	Rig->PostEditChange();
	Asset->PostEditChange();
	Asset->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("success"), true);
	return MakeSuccessResult(Data,
		FString::Printf(TEXT("Set %s.%s on rig %d of %s"),
			*NodeId, *PropertyPath, RigIndex, *Asset->GetPathName()));
}

#undef LOCTEXT_NAMESPACE
