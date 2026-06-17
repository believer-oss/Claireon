// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCameraAssetTool_ListNodes.h"

#include "ClaireonSessionManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "Tools/ClaireonCameraAssetHelpers.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "UObject/UnrealType.h"

#if WITH_GAMEPLAY_CAMERAS

#include "Core/CameraAsset.h"
#include "Core/CameraNode.h"
#include "Core/CameraRigAsset.h"
#include "Core/ObjectChildrenView.h"

namespace
{
	/**
	 * Find which UPROPERTY on Parent points at Child, returning a path segment
	 * suitable for appending to the parent's node_id (e.g. "Children[2]" or
	 * "DrivingBlend"). Iterates Parent's class properties; first match wins.
	 */
	FString CALN_ResolveChildSegment(UCameraNode* Parent, UCameraNode* Child, int32 FallbackIndex)
	{
		if (!Parent || !Child)
		{
			return FString::Printf(TEXT("Children[%d]"), FallbackIndex);
		}

		UClass* ParentClass = Parent->GetClass();
		for (TFieldIterator<FProperty> It(ParentClass); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop)
			{
				continue;
			}

			// Singular FObjectProperty pointing at Child.
			if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
			{
				UObject* Value = ObjProp->GetObjectPropertyValue(
					ObjProp->ContainerPtrToValuePtr<void>(Parent));
				if (Value == Child)
				{
					return ObjProp->GetName();
				}
				continue;
			}

			// TArray<TObjectPtr<UCameraNode>> containing Child.
			if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
			{
				FObjectProperty* InnerObj = CastField<FObjectProperty>(ArrayProp->Inner);
				if (!InnerObj)
				{
					continue;
				}
				FScriptArrayHelper Helper(ArrayProp,
					ArrayProp->ContainerPtrToValuePtr<void>(Parent));
				for (int32 i = 0; i < Helper.Num(); ++i)
				{
					UObject* Element = InnerObj->GetObjectPropertyValue(Helper.GetRawPtr(i));
					if (Element == Child)
					{
						return FString::Printf(TEXT("%s[%d]"), *ArrayProp->GetName(), i);
					}
				}
			}
		}

		// Fallback: best-effort name when the parent uses non-UPROPERTY storage
		// for OnGetChildren(). Surfaces under "Children[N]" so the path is still
		// well-formed even if it cannot round-trip through ResolveNode().
		return FString::Printf(TEXT("Children[%d]"), FallbackIndex);
	}

	void CALN_WalkNode(
		UCameraNode* Node,
		const FString& NodeId,
		const FString& ParentId,
		int32 ChildIndex,
		TArray<TSharedPtr<FJsonValue>>& Out)
	{
		if (!Node)
		{
			return;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("node_id"), NodeId);
		Entry->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		Entry->SetStringField(TEXT("parent_id"), ParentId);
		if (ChildIndex >= 0)
		{
			Entry->SetNumberField(TEXT("child_index"), ChildIndex);
		}
		Entry->SetObjectField(TEXT("properties"),
			ClaireonPropertyUtils::GetAllProperties(Node, /*Filter=*/FString(), /*Depth=*/0));

		Out.Add(MakeShared<FJsonValueObject>(Entry));

		const FCameraNodeChildrenView Children = Node->GetChildren();
		int32 i = 0;
		for (TObjectPtr<UCameraNode> Child : Children)
		{
			if (Child)
			{
				const FString Segment = CALN_ResolveChildSegment(Node, Child, i);
				const FString ChildPath = NodeId + TEXT(".") + Segment;
				CALN_WalkNode(Child, ChildPath, NodeId, i, Out);
			}
			++i;
		}
	}
} // namespace

FString FClaireonCameraAssetTool_ListNodes::GetOperation() const { return TEXT("list_nodes"); }

FString FClaireonCameraAssetTool_ListNodes::GetDescription() const
{
	return TEXT("Recursively walk a UCameraRigAsset's node tree and return node_id, class, parent, and a property summary for each node.");
}

TSharedPtr<FJsonObject> FClaireonCameraAssetTool_ListNodes::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("/Game/ path of the camera asset"), true);
	S.AddInteger(TEXT("rig_index"), TEXT("Index of the rig to walk (defaults to 0)"));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonCameraAssetTool_ListNodes::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	int32 RigIndex = 0;
	if (Arguments->HasField(TEXT("rig_index")))
	{
		double NumValue = 0.0;
		if (Arguments->TryGetNumberField(TEXT("rig_index"), NumValue))
		{
			RigIndex = static_cast<int32>(NumValue);
		}
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

	TArray<TSharedPtr<FJsonValue>> NodesJson;
	if (Rig->RootNode)
	{
		CALN_WalkNode(Rig->RootNode, TEXT("Root"), /*ParentId=*/FString(), /*ChildIndex=*/-1, NodesJson);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("nodes"), NodesJson);
	Data->SetNumberField(TEXT("count"), NodesJson.Num());
	return MakeSuccessResult(Data,
		FString::Printf(TEXT("Listed %d node(s) on rig %d of %s"),
			NodesJson.Num(), RigIndex, *Asset->GetPathName()));
}

#endif // WITH_GAMEPLAY_CAMERAS
