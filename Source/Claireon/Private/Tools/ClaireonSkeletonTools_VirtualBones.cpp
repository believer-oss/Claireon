// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSkeletonTools_VirtualBones.h"
#include "Animation/Skeleton.h"
#include "ReferenceSkeleton.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

namespace
{
	/** Report helper: return a state_view-like snapshot after an edit. */
	TSharedPtr<FJsonObject> BuildVirtualBoneSnapshot(const USkeleton* Skeleton, const FString& LastOperation)
	{
		TSharedPtr<FJsonObject> Out = ClaireonSkeletonHelpers::BuildVirtualBones(Skeleton);
		Out->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
		Out->SetStringField(TEXT("last_operation"), LastOperation);
		return Out;
	}
}

// ============================================================================
// skeleton_add_virtual_bone
// ============================================================================

FString ClaireonSkeletonTool_AddVirtualBone::GetOperation() const { return TEXT("add_virtual_bone"); }

FString ClaireonSkeletonTool_AddVirtualBone::GetDescription() const
{
	return TEXT("Add a virtual bone between two existing bones on the skeleton. "
				"If virtual_bone_name is omitted the engine generates a name from source+target (auto-prefixed with 'VB '). "
				"If provided, the name is auto-prefixed with 'VB ' if not already.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_AddVirtualBone::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("source_bone"), TEXT("Name of the source bone (origin end of the virtual bone)"), true);
	S.AddString(TEXT("target_bone"), TEXT("Name of the target bone (the bone the virtual bone points to)"), true);
	S.AddString(TEXT("virtual_bone_name"), TEXT("Optional explicit name for the new virtual bone. Will be auto-prefixed with 'VB ' if missing."));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_AddVirtualBone::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString SourceBoneStr, TargetBoneStr, VBNameStr;
	if (!Arguments->TryGetStringField(TEXT("source_bone"), SourceBoneStr) || SourceBoneStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: source_bone"));
	if (!Arguments->TryGetStringField(TEXT("target_bone"), TargetBoneStr) || TargetBoneStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: target_bone"));
	Arguments->TryGetStringField(TEXT("virtual_bone_name"), VBNameStr);

	const FName SourceBone(*SourceBoneStr);
	const FName TargetBone(*TargetBoneStr);

	const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
	if (Ref.FindBoneIndex(SourceBone) == INDEX_NONE)
		return MakeErrorResult(FString::Printf(TEXT("Source bone '%s' not found on skeleton"), *SourceBoneStr));
	if (Ref.FindBoneIndex(TargetBone) == INDEX_NONE)
		return MakeErrorResult(FString::Printf(TEXT("Target bone '%s' not found on skeleton"), *TargetBoneStr));

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonAddVirtualBone", "MCP: Add Virtual Bone"));

	FName CreatedName;
	bool bOk = false;
	if (!VBNameStr.IsEmpty())
	{
		FString Prefixed = VBNameStr;
		if (!VirtualBoneNameHelpers::CheckVirtualBonePrefix(Prefixed))
		{
			Prefixed = VirtualBoneNameHelpers::AddVirtualBonePrefix(Prefixed);
		}
		const FName Named(*Prefixed);
		bOk = Skeleton->AddNewNamedVirtualBone(SourceBone, TargetBone, Named);
		CreatedName = Named;
	}
	else
	{
		bOk = Skeleton->AddNewVirtualBone(SourceBone, TargetBone, CreatedName);
	}

	if (!bOk)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Failed to add virtual bone (source='%s' target='%s' name='%s'). "
				 "A virtual bone between this source/target may already exist, or the requested name is taken."),
			*SourceBoneStr, *TargetBoneStr, *VBNameStr));
	}

	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildVirtualBoneSnapshot(Skeleton,
		FString::Printf(TEXT("Added virtual bone '%s' (source='%s' target='%s')"),
			*CreatedName.ToString(), *SourceBoneStr, *TargetBoneStr));
	Out->SetStringField(TEXT("created_virtual_bone_name"), CreatedName.ToString());
	return MakeSuccessResult(Out, FString::Printf(TEXT("Added virtual bone '%s'"), *CreatedName.ToString()));
}

// ============================================================================
// skeleton_remove_virtual_bones
// ============================================================================

FString ClaireonSkeletonTool_RemoveVirtualBones::GetOperation() const { return TEXT("remove_virtual_bones"); }

FString ClaireonSkeletonTool_RemoveVirtualBones::GetDescription() const
{
	return TEXT("Remove one or more virtual bones by name. Batch-safe: pass an array of names. "
				"Also refreshes any blend-profile entries that referenced the removed bones.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_RemoveVirtualBones::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	// FToolSchemaBuilder lacks an AddArray; we describe the expected shape in the description.
	S.AddObject(TEXT("virtual_bone_names"), TEXT("Array of virtual bone names to remove (strings). Must include the 'VB ' prefix. REQUIRED."), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_RemoveVirtualBones::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	const TArray<TSharedPtr<FJsonValue>>* RawArr = nullptr;
	if (!Arguments->TryGetArrayField(TEXT("virtual_bone_names"), RawArr) || !RawArr || RawArr->Num() == 0)
	{
		return MakeErrorResult(TEXT("Missing required parameter: virtual_bone_names (array of strings)"));
	}

	TArray<FName> Names;
	Names.Reserve(RawArr->Num());
	for (const TSharedPtr<FJsonValue>& V : *RawArr)
	{
		FString S;
		if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty())
		{
			Names.Add(FName(*S));
		}
	}
	if (Names.Num() == 0)
	{
		return MakeErrorResult(TEXT("virtual_bone_names array contained no valid entries"));
	}

	// Validate names exist
	const TArray<FVirtualBone>& Existing = Skeleton->GetVirtualBones();
	TArray<FString> Missing;
	for (const FName& N : Names)
	{
		bool bFound = false;
		for (const FVirtualBone& VB : Existing)
		{
			if (VB.VirtualBoneName == N) { bFound = true; break; }
		}
		if (!bFound) Missing.Add(N.ToString());
	}
	if (Missing.Num() > 0)
	{
		return MakeErrorResult(FString::Printf(TEXT("Virtual bone(s) not found: %s"), *FString::Join(Missing, TEXT(", "))));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonRemoveVirtualBones", "MCP: Remove Virtual Bones"));
	Skeleton->RemoveVirtualBones(Names);
	Skeleton->MarkPackageDirty();

	TArray<FString> Removed;
	for (const FName& N : Names) Removed.Add(N.ToString());

	TSharedPtr<FJsonObject> Out = BuildVirtualBoneSnapshot(Skeleton,
		FString::Printf(TEXT("Removed %d virtual bone(s): %s"), Removed.Num(), *FString::Join(Removed, TEXT(", "))));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Removed %d virtual bone(s)"), Removed.Num()));
}

// ============================================================================
// skeleton_rename_virtual_bone
// ============================================================================

FString ClaireonSkeletonTool_RenameVirtualBone::GetOperation() const { return TEXT("rename_virtual_bone"); }

FString ClaireonSkeletonTool_RenameVirtualBone::GetDescription() const
{
	return TEXT("Rename a virtual bone. The new name will be auto-prefixed with 'VB ' if missing. "
				"Any other virtual bones that referenced this one as their source will be updated accordingly.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_RenameVirtualBone::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("old_name"), TEXT("Current virtual bone name (include 'VB ' prefix)"), true);
	S.AddString(TEXT("new_name"), TEXT("New virtual bone name. Will be auto-prefixed with 'VB ' if missing."), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_RenameVirtualBone::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString NewPrefixed = NewStr;
	if (!VirtualBoneNameHelpers::CheckVirtualBonePrefix(NewPrefixed))
	{
		NewPrefixed = VirtualBoneNameHelpers::AddVirtualBonePrefix(NewPrefixed);
	}

	const FName OldName(*OldStr);
	const FName NewName(*NewPrefixed);

	// Validate existence of OldName + non-collision of NewName.
	const TArray<FVirtualBone>& Existing = Skeleton->GetVirtualBones();
	bool bOldFound = false;
	for (const FVirtualBone& VB : Existing)
	{
		if (VB.VirtualBoneName == OldName) { bOldFound = true; }
		if (VB.VirtualBoneName == NewName)
		{
			return MakeErrorResult(FString::Printf(TEXT("Target virtual bone name '%s' is already in use"), *NewPrefixed));
		}
	}
	if (!bOldFound)
	{
		return MakeErrorResult(FString::Printf(TEXT("Virtual bone '%s' not found"), *OldStr));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonRenameVirtualBone", "MCP: Rename Virtual Bone"));
	Skeleton->RenameVirtualBone(OldName, NewName);
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildVirtualBoneSnapshot(Skeleton,
		FString::Printf(TEXT("Renamed virtual bone '%s' -> '%s'"), *OldStr, *NewPrefixed));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Renamed virtual bone '%s' -> '%s'"), *OldStr, *NewPrefixed));
}
