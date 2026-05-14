// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSkeletonTools_Metadata.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimCurveMetadata.h"
#include "Animation/BoneReference.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	TSharedPtr<FJsonObject> BuildMetadataSnapshot(const USkeleton* Skeleton, const FString& LastOperation)
	{
		TSharedPtr<FJsonObject> Out = ClaireonSkeletonHelpers::BuildMetadata(Skeleton);
		Out->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
		Out->SetStringField(TEXT("last_operation"), LastOperation);
		return Out;
	}
}

// ============================================================================
// Animation notify names
// ============================================================================

// --- add -------------------------------------------------------------------

FString ClaireonSkeletonTool_AddAnimationNotify::GetOperation() const { return TEXT("add_animation_notify"); }

FString ClaireonSkeletonTool_AddAnimationNotify::GetDescription() const
{
	return TEXT("Add a skeleton-level animation notify name (editor-only cache). "
				"Once registered, any animation using this skeleton can place a skeleton-style notify with this name.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_AddAnimationNotify::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("notify_name"), TEXT("Notify name to register"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_AddAnimationNotify::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString NotifyNameStr;
	if (!Arguments->TryGetStringField(TEXT("notify_name"), NotifyNameStr) || NotifyNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: notify_name"));

#if WITH_EDITORONLY_DATA
	const FName NotifyName(*NotifyNameStr);
	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonAddAnimNotify", "MCP: Add Animation Notify"));
	Skeleton->Modify();
	Skeleton->AddNewAnimationNotify(NotifyName);
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildMetadataSnapshot(Skeleton,
		FString::Printf(TEXT("Added animation notify '%s'"), *NotifyNameStr));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Added animation notify '%s'"), *NotifyNameStr));
#else
	return MakeErrorResult(TEXT("Animation notify names are editor-only and not available in this build."));
#endif
}

// --- remove -----------------------------------------------------------------

FString ClaireonSkeletonTool_RemoveAnimationNotify::GetOperation() const { return TEXT("remove_animation_notify"); }

FString ClaireonSkeletonTool_RemoveAnimationNotify::GetDescription() const
{
	return TEXT("Remove a skeleton-level animation notify name from the cache.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_RemoveAnimationNotify::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("notify_name"), TEXT("Notify name to remove"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_RemoveAnimationNotify::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString NotifyNameStr;
	if (!Arguments->TryGetStringField(TEXT("notify_name"), NotifyNameStr) || NotifyNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: notify_name"));

#if WITH_EDITORONLY_DATA
	const FName NotifyName(*NotifyNameStr);
	if (!Skeleton->AnimationNotifies.Contains(NotifyName))
	{
		return MakeErrorResult(FString::Printf(TEXT("Animation notify '%s' not found on skeleton"), *NotifyNameStr));
	}
	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonRemoveAnimNotify", "MCP: Remove Animation Notify"));
	Skeleton->Modify();
	Skeleton->RemoveAnimationNotify(NotifyName);
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildMetadataSnapshot(Skeleton,
		FString::Printf(TEXT("Removed animation notify '%s'"), *NotifyNameStr));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Removed animation notify '%s'"), *NotifyNameStr));
#else
	return MakeErrorResult(TEXT("Animation notify names are editor-only and not available in this build."));
#endif
}

// --- rename -----------------------------------------------------------------

FString ClaireonSkeletonTool_RenameAnimationNotify::GetOperation() const { return TEXT("rename_animation_notify"); }

FString ClaireonSkeletonTool_RenameAnimationNotify::GetDescription() const
{
	return TEXT("Rename a skeleton-level animation notify name.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_RenameAnimationNotify::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("old_name"), TEXT("Current notify name"), true);
	S.AddString(TEXT("new_name"), TEXT("New notify name"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_RenameAnimationNotify::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString OldStr, NewStr;
	if (!Arguments->TryGetStringField(TEXT("old_name"), OldStr) || OldStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: old_name"));
	if (!Arguments->TryGetStringField(TEXT("new_name"), NewStr) || NewStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: new_name"));

#if WITH_EDITORONLY_DATA
	const FName OldName(*OldStr);
	const FName NewName(*NewStr);
	if (!Skeleton->AnimationNotifies.Contains(OldName))
	{
		return MakeErrorResult(FString::Printf(TEXT("Animation notify '%s' not found on skeleton"), *OldStr));
	}
	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonRenameAnimNotify", "MCP: Rename Animation Notify"));
	Skeleton->Modify();
	Skeleton->RenameAnimationNotify(OldName, NewName);
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildMetadataSnapshot(Skeleton,
		FString::Printf(TEXT("Renamed animation notify '%s' -> '%s'"), *OldStr, *NewStr));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Renamed animation notify '%s' -> '%s'"), *OldStr, *NewStr));
#else
	return MakeErrorResult(TEXT("Animation notify names are editor-only and not available in this build."));
#endif
}

// ============================================================================
// Curve metadata
// ============================================================================

// --- add -------------------------------------------------------------------

FString ClaireonSkeletonTool_AddCurveMetadata::GetOperation() const { return TEXT("add_curve_metadata"); }

FString ClaireonSkeletonTool_AddCurveMetadata::GetDescription() const
{
	return TEXT("Add a curve metadata entry on the skeleton. Creates an entry with default flags (not material, not morph target, no linked bones, MaxLOD=0xff). "
				"Use skeleton_set_curve_metadata_flags to configure it after creation.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_AddCurveMetadata::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("curve_name"), TEXT("Curve name to add"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_AddCurveMetadata::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString CurveNameStr;
	if (!Arguments->TryGetStringField(TEXT("curve_name"), CurveNameStr) || CurveNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: curve_name"));

	const FName CurveName(*CurveNameStr);

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonAddCurveMeta", "MCP: Add Curve Metadata"));
	const bool bAdded = Skeleton->AddCurveMetaData(CurveName, /*bTransact*/true);
	if (!bAdded)
	{
		return MakeErrorResult(FString::Printf(TEXT("Curve metadata entry '%s' already exists"), *CurveNameStr));
	}
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildMetadataSnapshot(Skeleton,
		FString::Printf(TEXT("Added curve metadata '%s'"), *CurveNameStr));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Added curve metadata '%s'"), *CurveNameStr));
}

// --- remove -----------------------------------------------------------------

FString ClaireonSkeletonTool_RemoveCurveMetadata::GetOperation() const { return TEXT("remove_curve_metadata"); }

FString ClaireonSkeletonTool_RemoveCurveMetadata::GetDescription() const
{
	return TEXT("Remove a curve metadata entry from the skeleton.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_RemoveCurveMetadata::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("curve_name"), TEXT("Curve name to remove"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_RemoveCurveMetadata::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString CurveNameStr;
	if (!Arguments->TryGetStringField(TEXT("curve_name"), CurveNameStr) || CurveNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: curve_name"));

#if WITH_EDITOR
	const FName CurveName(*CurveNameStr);
	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonRemoveCurveMeta", "MCP: Remove Curve Metadata"));
	const bool bOk = Skeleton->RemoveCurveMetaData(CurveName);
	if (!bOk)
	{
		return MakeErrorResult(FString::Printf(TEXT("Curve metadata entry '%s' not found"), *CurveNameStr));
	}
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildMetadataSnapshot(Skeleton,
		FString::Printf(TEXT("Removed curve metadata '%s'"), *CurveNameStr));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Removed curve metadata '%s'"), *CurveNameStr));
#else
	return MakeErrorResult(TEXT("Curve metadata removal is editor-only."));
#endif
}

// --- rename -----------------------------------------------------------------

FString ClaireonSkeletonTool_RenameCurveMetadata::GetOperation() const { return TEXT("rename_curve_metadata"); }

FString ClaireonSkeletonTool_RenameCurveMetadata::GetDescription() const
{
	return TEXT("Rename a curve metadata entry. Preserves the entry's flags/linked bones/max LOD under the new name.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_RenameCurveMetadata::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("old_name"), TEXT("Current curve name"), true);
	S.AddString(TEXT("new_name"), TEXT("New curve name"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_RenameCurveMetadata::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString OldStr, NewStr;
	if (!Arguments->TryGetStringField(TEXT("old_name"), OldStr) || OldStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: old_name"));
	if (!Arguments->TryGetStringField(TEXT("new_name"), NewStr) || NewStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: new_name"));

#if WITH_EDITOR
	const FName OldName(*OldStr);
	const FName NewName(*NewStr);
	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonRenameCurveMeta", "MCP: Rename Curve Metadata"));
	const bool bOk = Skeleton->RenameCurveMetaData(OldName, NewName);
	if (!bOk)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to rename curve metadata '%s' -> '%s' (old not found or new collides)"), *OldStr, *NewStr));
	}
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildMetadataSnapshot(Skeleton,
		FString::Printf(TEXT("Renamed curve metadata '%s' -> '%s'"), *OldStr, *NewStr));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Renamed curve metadata '%s' -> '%s'"), *OldStr, *NewStr));
#else
	return MakeErrorResult(TEXT("Curve metadata rename is editor-only."));
#endif
}

// --- set_flags --------------------------------------------------------------

FString ClaireonSkeletonTool_SetCurveMetadataFlags::GetOperation() const { return TEXT("set_curve_metadata_flags"); }

FString ClaireonSkeletonTool_SetCurveMetadataFlags::GetDescription() const
{
	return TEXT("Update curve metadata fields on an existing entry. Any combination of material/morph_target/max_lod/linked_bones may be supplied; "
				"omitted fields are left unchanged. linked_bones is an array of bone name strings.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_SetCurveMetadataFlags::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("curve_name"), TEXT("Curve metadata entry to edit"), true);
	S.AddBoolean(TEXT("material"), TEXT("Set the material flag"));
	S.AddBoolean(TEXT("morph_target"), TEXT("Set the morph target flag"));
	S.AddInteger(TEXT("max_lod"), TEXT("Max LOD index (0..255, use 255 for 'evaluate at all LODs')"));
	S.AddObject(TEXT("linked_bones"), TEXT("Array of bone name strings that link this curve. Replaces the existing list."));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_SetCurveMetadataFlags::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString CurveNameStr;
	if (!Arguments->TryGetStringField(TEXT("curve_name"), CurveNameStr) || CurveNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: curve_name"));

	const FName CurveName(*CurveNameStr);
	FCurveMetaData* Meta = Skeleton->GetCurveMetaData(CurveName);
	if (!Meta)
	{
		return MakeErrorResult(FString::Printf(TEXT("Curve metadata entry '%s' not found"), *CurveNameStr));
	}

	TArray<FString> Changes;

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonSetCurveMetaFlags", "MCP: Set Curve Metadata Flags"));
	Skeleton->Modify();

	bool bMaterial = false;
	if (Arguments->TryGetBoolField(TEXT("material"), bMaterial))
	{
		Meta->Type.bMaterial = bMaterial;
		Changes.Add(FString::Printf(TEXT("material=%s"), bMaterial ? TEXT("true") : TEXT("false")));
	}
	bool bMorph = false;
	if (Arguments->TryGetBoolField(TEXT("morph_target"), bMorph))
	{
		Meta->Type.bMorphtarget = bMorph;
		Changes.Add(FString::Printf(TEXT("morph_target=%s"), bMorph ? TEXT("true") : TEXT("false")));
	}
	int32 MaxLOD = 0;
	if (Arguments->TryGetNumberField(TEXT("max_lod"), MaxLOD))
	{
		if (MaxLOD < 0 || MaxLOD > 255)
		{
			return MakeErrorResult(TEXT("max_lod must be in [0..255]"));
		}
		Meta->MaxLOD = static_cast<uint8>(MaxLOD);
		Changes.Add(FString::Printf(TEXT("max_lod=%d"), MaxLOD));
	}
	const TArray<TSharedPtr<FJsonValue>>* LinkedArr = nullptr;
	if (Arguments->TryGetArrayField(TEXT("linked_bones"), LinkedArr) && LinkedArr)
	{
		const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
		TArray<FBoneReference> NewLinks;
		NewLinks.Reserve(LinkedArr->Num());
		TArray<FString> Missing;
		for (const TSharedPtr<FJsonValue>& V : *LinkedArr)
		{
			FString BoneStr;
			if (!V.IsValid() || !V->TryGetString(BoneStr) || BoneStr.IsEmpty()) continue;
			const FName BoneName(*BoneStr);
			if (Ref.FindBoneIndex(BoneName) == INDEX_NONE)
			{
				Missing.Add(BoneStr);
				continue;
			}
			FBoneReference BR;
			BR.BoneName = BoneName;
			NewLinks.Add(BR);
		}
		if (Missing.Num() > 0)
		{
			return MakeErrorResult(FString::Printf(TEXT("linked_bones contains unknown bones: %s"), *FString::Join(Missing, TEXT(", "))));
		}
		Meta->LinkedBones = NewLinks;
		Changes.Add(FString::Printf(TEXT("linked_bones=[%d entries]"), NewLinks.Num()));
	}

	if (Changes.Num() == 0)
	{
		return MakeErrorResult(TEXT("No editable fields were supplied (material / morph_target / max_lod / linked_bones)"));
	}

	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildMetadataSnapshot(Skeleton,
		FString::Printf(TEXT("Set curve metadata '%s' flags: %s"), *CurveNameStr, *FString::Join(Changes, TEXT(", "))));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Updated curve metadata '%s' (%d field(s))"), *CurveNameStr, Changes.Num()));
}
