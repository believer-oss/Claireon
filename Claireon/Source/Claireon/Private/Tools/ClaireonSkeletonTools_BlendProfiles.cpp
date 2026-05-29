// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSkeletonTools_BlendProfiles.h"
#include "Animation/Skeleton.h"
#include "Animation/BlendProfile.h"
#include "ReferenceSkeleton.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

namespace
{
	TSharedPtr<FJsonObject> BuildProfileSnapshot(const USkeleton* Skeleton, bool bMasksOnly, const FString& LastOperation)
	{
		TSharedPtr<FJsonObject> Out = ClaireonSkeletonHelpers::BuildBlendProfiles(Skeleton, bMasksOnly);
		Out->SetStringField(TEXT("skeleton_path"), Skeleton->GetPathName());
		Out->SetStringField(TEXT("last_operation"), LastOperation);
		return Out;
	}

	/** Resolve a profile and ensure it matches the expected flavor (mask vs regular). */
	UBlendProfile* RequireBlendProfile(USkeleton* Skeleton, const FString& ProfileName, bool bExpectMask, FString& OutError)
	{
		UBlendProfile* Profile = Skeleton->GetBlendProfile(FName(*ProfileName));
		if (!Profile)
		{
			OutError = FString::Printf(TEXT("Blend %s '%s' not found"),
				bExpectMask ? TEXT("mask") : TEXT("profile"), *ProfileName);
			return nullptr;
		}
		if (bExpectMask && !Profile->IsBlendMask())
		{
			OutError = FString::Printf(TEXT("'%s' exists but is a blend profile (mode=%s), not a blend mask"),
				*ProfileName, *ClaireonSkeletonHelpers::BlendProfileModeToString(Profile->Mode));
			return nullptr;
		}
		if (!bExpectMask && Profile->IsBlendMask())
		{
			OutError = FString::Printf(TEXT("'%s' exists but is a blend mask, not a blend profile"), *ProfileName);
			return nullptr;
		}
		return Profile;
	}
}

// ============================================================================
// skeleton_add_blend_profile
// ============================================================================

FString ClaireonSkeletonTool_AddBlendProfile::GetOperation() const { return TEXT("add_blend_profile"); }

FString ClaireonSkeletonTool_AddBlendProfile::GetDescription() const
{
	return TEXT("Add a new blend profile with the given name and mode. Mode must be 'TimeFactor' or 'WeightFactor'. "
				"For blend masks use skeleton_add_blend_mask instead.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_AddBlendProfile::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("profile_name"), TEXT("Name of the new blend profile"), true);
	S.AddEnum(TEXT("mode"), TEXT("Blend profile mode. Default: TimeFactor"), {TEXT("TimeFactor"), TEXT("WeightFactor")});
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_AddBlendProfile::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString ProfileNameStr;
	if (!Arguments->TryGetStringField(TEXT("profile_name"), ProfileNameStr) || ProfileNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: profile_name"));

	FString ModeStr = TEXT("TimeFactor");
	Arguments->TryGetStringField(TEXT("mode"), ModeStr);
	EBlendProfileMode Mode;
	FString ParseError;
	if (!ClaireonSkeletonHelpers::ParseBlendProfileMode(ModeStr, Mode, ParseError))
	{
		return MakeErrorResult(ParseError);
	}
	if (Mode == EBlendProfileMode::BlendMask)
	{
		return MakeErrorResult(TEXT("Use skeleton_add_blend_mask to create a blend mask (mode=BlendMask not allowed here)."));
	}

	const FName ProfileName(*ProfileNameStr);
	if (Skeleton->GetBlendProfile(ProfileName) != nullptr)
	{
		return MakeErrorResult(FString::Printf(TEXT("Blend profile '%s' already exists"), *ProfileNameStr));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonAddBlendProfile", "MCP: Add Blend Profile"));
	Skeleton->Modify();
	UBlendProfile* NewProfile = Skeleton->CreateNewBlendProfile(ProfileName);
	if (!NewProfile)
	{
		return MakeErrorResult(FString::Printf(TEXT("Engine refused to create blend profile '%s'"), *ProfileNameStr));
	}
	NewProfile->Modify();
	NewProfile->Mode = Mode;
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildProfileSnapshot(Skeleton, /*bMasksOnly*/false,
		FString::Printf(TEXT("Added blend profile '%s' (mode=%s)"), *ProfileNameStr, *ModeStr));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Added blend profile '%s'"), *ProfileNameStr));
}

// ============================================================================
// skeleton_remove_blend_profile
// ============================================================================

FString ClaireonSkeletonTool_RemoveBlendProfile::GetOperation() const { return TEXT("remove_blend_profile"); }

FString ClaireonSkeletonTool_RemoveBlendProfile::GetDescription() const
{
	return TEXT("Remove a blend profile (TimeFactor / WeightFactor). For blend masks removal is disallowed — "
				"see skeleton_remove_blend_mask for the engine-bug explanation.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_RemoveBlendProfile::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("profile_name"), TEXT("Name of the blend profile to remove"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_RemoveBlendProfile::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString ProfileNameStr;
	if (!Arguments->TryGetStringField(TEXT("profile_name"), ProfileNameStr) || ProfileNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: profile_name"));

	FString ReqErr;
	UBlendProfile* Profile = RequireBlendProfile(Skeleton, ProfileNameStr, /*bExpectMask*/false, ReqErr);
	if (!Profile) return MakeErrorResult(ReqErr);

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonRemoveBlendProfile", "MCP: Remove Blend Profile"));
	Skeleton->Modify();
	Skeleton->BlendProfiles.Remove(Profile);
	Profile->MarkAsGarbage();
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildProfileSnapshot(Skeleton, /*bMasksOnly*/false,
		FString::Printf(TEXT("Removed blend profile '%s'"), *ProfileNameStr));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Removed blend profile '%s'"), *ProfileNameStr));
}

// ============================================================================
// skeleton_rename_blend_profile
// ============================================================================

FString ClaireonSkeletonTool_RenameBlendProfile::GetOperation() const { return TEXT("rename_blend_profile"); }

FString ClaireonSkeletonTool_RenameBlendProfile::GetDescription() const
{
    return TEXT("Rename a blend profile (TimeFactor/WeightFactor). For blend masks use skeleton_rename_blend_mask. Session-mode tool: open via skeleton_open first.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_RenameBlendProfile::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("old_name"), TEXT("Current profile name"), true);
	S.AddString(TEXT("new_name"), TEXT("New profile name"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_RenameBlendProfile::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString ReqErr;
	UBlendProfile* OldProfile = RequireBlendProfile(Skeleton, OldStr, /*bExpectMask*/false, ReqErr);
	if (!OldProfile) return MakeErrorResult(ReqErr);
	if (Skeleton->GetBlendProfile(FName(*NewStr)) != nullptr)
	{
		return MakeErrorResult(FString::Printf(TEXT("A blend profile/mask named '%s' already exists"), *NewStr));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonRenameBlendProfile", "MCP: Rename Blend Profile"));
	UBlendProfile* Renamed = Skeleton->RenameBlendProfile(FName(*OldStr), FName(*NewStr));
	if (!Renamed)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to rename blend profile '%s' -> '%s'"), *OldStr, *NewStr));
	}
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildProfileSnapshot(Skeleton, /*bMasksOnly*/false,
		FString::Printf(TEXT("Renamed blend profile '%s' -> '%s'"), *OldStr, *NewStr));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Renamed blend profile '%s' -> '%s'"), *OldStr, *NewStr));
}

// ============================================================================
// skeleton_set_blend_profile_mode
// ============================================================================

FString ClaireonSkeletonTool_SetBlendProfileMode::GetOperation() const { return TEXT("set_blend_profile_mode"); }

FString ClaireonSkeletonTool_SetBlendProfileMode::GetDescription() const
{
	return TEXT("Change the mode of an existing blend profile between TimeFactor and WeightFactor. "
				"Switching to or from BlendMask is NOT supported here (masks have separate lifecycle tools).");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_SetBlendProfileMode::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("profile_name"), TEXT("Blend profile name"), true);
	S.AddEnum(TEXT("mode"), TEXT("New mode"), {TEXT("TimeFactor"), TEXT("WeightFactor")});
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_SetBlendProfileMode::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString ProfileNameStr, ModeStr;
	if (!Arguments->TryGetStringField(TEXT("profile_name"), ProfileNameStr) || ProfileNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: profile_name"));
	if (!Arguments->TryGetStringField(TEXT("mode"), ModeStr) || ModeStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: mode"));

	EBlendProfileMode NewMode;
	FString ParseError;
	if (!ClaireonSkeletonHelpers::ParseBlendProfileMode(ModeStr, NewMode, ParseError))
	{
		return MakeErrorResult(ParseError);
	}
	if (NewMode == EBlendProfileMode::BlendMask)
	{
		return MakeErrorResult(TEXT("Cannot set mode to BlendMask via this tool. Use blend mask lifecycle tools instead."));
	}

	FString ReqErr;
	UBlendProfile* Profile = RequireBlendProfile(Skeleton, ProfileNameStr, /*bExpectMask*/false, ReqErr);
	if (!Profile) return MakeErrorResult(ReqErr);

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonSetBlendProfileMode", "MCP: Set Blend Profile Mode"));
	Skeleton->Modify();
	Profile->Modify();
	Profile->Mode = NewMode;
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildProfileSnapshot(Skeleton, /*bMasksOnly*/false,
		FString::Printf(TEXT("Set blend profile '%s' mode -> %s"), *ProfileNameStr, *ModeStr));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Set blend profile '%s' mode -> %s"), *ProfileNameStr, *ModeStr));
}

// ============================================================================
// skeleton_set_blend_profile_bone_scale
// ============================================================================

FString ClaireonSkeletonTool_SetBlendProfileBoneScale::GetOperation() const { return TEXT("set_blend_profile_bone_scale"); }

FString ClaireonSkeletonTool_SetBlendProfileBoneScale::GetDescription() const
{
	return TEXT("Set the per-bone scale on a blend profile. For TimeFactor, values are typically in [0..1] (0 = instant). "
				"For WeightFactor, values are typically >= 1.0. If recurse=true, the scale is applied to the bone and all descendants.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_SetBlendProfileBoneScale::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("profile_name"), TEXT("Blend profile name"), true);
	S.AddString(TEXT("bone_name"), TEXT("Bone to scale"), true);
	S.AddNumber(TEXT("scale"), TEXT("Per-bone blend scale"), true);
	S.AddBoolean(TEXT("recurse"), TEXT("Apply to children too (default false)"));
	S.AddBoolean(TEXT("create"), TEXT("Create the entry if not present (default true)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_SetBlendProfileBoneScale::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString ProfileNameStr, BoneNameStr;
	if (!Arguments->TryGetStringField(TEXT("profile_name"), ProfileNameStr) || ProfileNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: profile_name"));
	if (!Arguments->TryGetStringField(TEXT("bone_name"), BoneNameStr) || BoneNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: bone_name"));

	double Scale = 1.0;
	if (!Arguments->TryGetNumberField(TEXT("scale"), Scale))
		return MakeErrorResult(TEXT("Missing required parameter: scale"));

	bool bRecurse = false;
	Arguments->TryGetBoolField(TEXT("recurse"), bRecurse);
	bool bCreate = true;
	Arguments->TryGetBoolField(TEXT("create"), bCreate);

	FString ReqErr;
	UBlendProfile* Profile = RequireBlendProfile(Skeleton, ProfileNameStr, /*bExpectMask*/false, ReqErr);
	if (!Profile) return MakeErrorResult(ReqErr);

	const FName BoneName(*BoneNameStr);
	if (Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneName) == INDEX_NONE)
	{
		return MakeErrorResult(FString::Printf(TEXT("Bone '%s' not found on skeleton"), *BoneNameStr));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonSetBlendProfileBone", "MCP: Set Blend Profile Bone Scale"));
	Skeleton->Modify();
	Profile->Modify();
	Profile->SetBoneBlendScale(BoneName, static_cast<float>(Scale), bRecurse, bCreate);
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildProfileSnapshot(Skeleton, /*bMasksOnly*/false,
		FString::Printf(TEXT("Set '%s' bone '%s' scale=%.3f%s"), *ProfileNameStr, *BoneNameStr, Scale, bRecurse ? TEXT(" (recurse)") : TEXT("")));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Set blend profile bone scale on '%s'"), *ProfileNameStr));
}

// ============================================================================
// skeleton_clear_blend_profile_bone_scale
// ============================================================================

FString ClaireonSkeletonTool_ClearBlendProfileBoneScale::GetOperation() const { return TEXT("clear_blend_profile_bone_scale"); }

FString ClaireonSkeletonTool_ClearBlendProfileBoneScale::GetDescription() const
{
	return TEXT("Remove the per-bone entry from a blend profile (reverts the bone to the default scale). "
				"No-op if the profile had no entry for this bone.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_ClearBlendProfileBoneScale::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("profile_name"), TEXT("Blend profile name"), true);
	S.AddString(TEXT("bone_name"), TEXT("Bone entry to clear"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_ClearBlendProfileBoneScale::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString ProfileNameStr, BoneNameStr;
	if (!Arguments->TryGetStringField(TEXT("profile_name"), ProfileNameStr) || ProfileNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: profile_name"));
	if (!Arguments->TryGetStringField(TEXT("bone_name"), BoneNameStr) || BoneNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: bone_name"));

	FString ReqErr;
	UBlendProfile* Profile = RequireBlendProfile(Skeleton, ProfileNameStr, /*bExpectMask*/false, ReqErr);
	if (!Profile) return MakeErrorResult(ReqErr);

	const FName BoneName(*BoneNameStr);
	const int32 EntryIdx = Profile->GetEntryIndex(BoneName);
	if (EntryIdx == INDEX_NONE)
	{
		return MakeErrorResult(FString::Printf(TEXT("Blend profile '%s' has no entry for bone '%s'"), *ProfileNameStr, *BoneNameStr));
	}
	const int32 BoneIdx = Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneName);
	if (BoneIdx == INDEX_NONE)
	{
		return MakeErrorResult(FString::Printf(TEXT("Bone '%s' not found on skeleton"), *BoneNameStr));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonClearBlendProfileBone", "MCP: Clear Blend Profile Bone"));
	Skeleton->Modify();
	Profile->Modify();
	Profile->RemoveEntry(BoneIdx);
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildProfileSnapshot(Skeleton, /*bMasksOnly*/false,
		FString::Printf(TEXT("Cleared '%s' entry for bone '%s'"), *ProfileNameStr, *BoneNameStr));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Cleared blend profile entry '%s'"), *BoneNameStr));
}

// ============================================================================
// Blend masks — add / rename / set_weight / clear_weight (no remove)
// ============================================================================

// --- add_blend_mask ---------------------------------------------------------

FString ClaireonSkeletonTool_AddBlendMask::GetOperation() const { return TEXT("add_blend_mask"); }

FString ClaireonSkeletonTool_AddBlendMask::GetDescription() const
{
	return TEXT("Add a new blend mask (a UBlendProfile with Mode=BlendMask). Mask weights are per-bone alpha in [0..1]; "
				"default per-bone weight is 0 when no entry is present.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_AddBlendMask::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("mask_name"), TEXT("Name of the new blend mask"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_AddBlendMask::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString MaskNameStr;
	if (!Arguments->TryGetStringField(TEXT("mask_name"), MaskNameStr) || MaskNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: mask_name"));

	const FName MaskName(*MaskNameStr);
	if (Skeleton->GetBlendProfile(MaskName) != nullptr)
	{
		return MakeErrorResult(FString::Printf(TEXT("A blend profile/mask named '%s' already exists"), *MaskNameStr));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonAddBlendMask", "MCP: Add Blend Mask"));
	Skeleton->Modify();
	UBlendProfile* NewMask = Skeleton->CreateNewBlendProfile(MaskName);
	if (!NewMask)
	{
		return MakeErrorResult(FString::Printf(TEXT("Engine refused to create blend mask '%s'"), *MaskNameStr));
	}
	NewMask->Modify();
	NewMask->Mode = EBlendProfileMode::BlendMask;
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildProfileSnapshot(Skeleton, /*bMasksOnly*/true,
		FString::Printf(TEXT("Added blend mask '%s'"), *MaskNameStr));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Added blend mask '%s'"), *MaskNameStr));
}

// --- rename_blend_mask ------------------------------------------------------

FString ClaireonSkeletonTool_RenameBlendMask::GetOperation() const { return TEXT("rename_blend_mask"); }

FString ClaireonSkeletonTool_RenameBlendMask::GetDescription() const
{
    return TEXT("Rename a blend mask. Validates that the target is actually a mask (Mode=BlendMask). Session-mode tool: open via skeleton_open first.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_RenameBlendMask::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("old_name"), TEXT("Current mask name"), true);
	S.AddString(TEXT("new_name"), TEXT("New mask name"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_RenameBlendMask::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString ReqErr;
	UBlendProfile* Mask = RequireBlendProfile(Skeleton, OldStr, /*bExpectMask*/true, ReqErr);
	if (!Mask) return MakeErrorResult(ReqErr);
	if (Skeleton->GetBlendProfile(FName(*NewStr)) != nullptr)
	{
		return MakeErrorResult(FString::Printf(TEXT("A blend profile/mask named '%s' already exists"), *NewStr));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonRenameBlendMask", "MCP: Rename Blend Mask"));
	UBlendProfile* Renamed = Skeleton->RenameBlendProfile(FName(*OldStr), FName(*NewStr));
	if (!Renamed)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to rename blend mask '%s' -> '%s'"), *OldStr, *NewStr));
	}
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildProfileSnapshot(Skeleton, /*bMasksOnly*/true,
		FString::Printf(TEXT("Renamed blend mask '%s' -> '%s'"), *OldStr, *NewStr));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Renamed blend mask '%s' -> '%s'"), *OldStr, *NewStr));
}

// --- set_blend_mask_bone_weight ---------------------------------------------

FString ClaireonSkeletonTool_SetBlendMaskBoneWeight::GetOperation() const { return TEXT("set_blend_mask_bone_weight"); }

FString ClaireonSkeletonTool_SetBlendMaskBoneWeight::GetDescription() const
{
	return TEXT("Set the per-bone alpha on a blend mask. Weight should be in [0..1]. "
				"If recurse=true, applies to the bone and its descendants.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_SetBlendMaskBoneWeight::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("mask_name"), TEXT("Blend mask name"), true);
	S.AddString(TEXT("bone_name"), TEXT("Bone to weight"), true);
	S.AddNumber(TEXT("weight"), TEXT("Per-bone mask weight [0..1]"), true);
	S.AddBoolean(TEXT("recurse"), TEXT("Apply to children too (default false)"));
	S.AddBoolean(TEXT("create"), TEXT("Create the entry if not present (default true)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_SetBlendMaskBoneWeight::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString MaskNameStr, BoneNameStr;
	if (!Arguments->TryGetStringField(TEXT("mask_name"), MaskNameStr) || MaskNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: mask_name"));
	if (!Arguments->TryGetStringField(TEXT("bone_name"), BoneNameStr) || BoneNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: bone_name"));

	double Weight = 0.0;
	if (!Arguments->TryGetNumberField(TEXT("weight"), Weight))
		return MakeErrorResult(TEXT("Missing required parameter: weight"));

	bool bRecurse = false;
	Arguments->TryGetBoolField(TEXT("recurse"), bRecurse);
	bool bCreate = true;
	Arguments->TryGetBoolField(TEXT("create"), bCreate);

	FString ReqErr;
	UBlendProfile* Mask = RequireBlendProfile(Skeleton, MaskNameStr, /*bExpectMask*/true, ReqErr);
	if (!Mask) return MakeErrorResult(ReqErr);

	const FName BoneName(*BoneNameStr);
	if (Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneName) == INDEX_NONE)
	{
		return MakeErrorResult(FString::Printf(TEXT("Bone '%s' not found on skeleton"), *BoneNameStr));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonSetBlendMaskBone", "MCP: Set Blend Mask Bone Weight"));
	Skeleton->Modify();
	Mask->Modify();
	Mask->SetBoneBlendScale(BoneName, static_cast<float>(Weight), bRecurse, bCreate);
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildProfileSnapshot(Skeleton, /*bMasksOnly*/true,
		FString::Printf(TEXT("Set mask '%s' bone '%s' weight=%.3f%s"), *MaskNameStr, *BoneNameStr, Weight, bRecurse ? TEXT(" (recurse)") : TEXT("")));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Set blend mask bone weight on '%s'"), *MaskNameStr));
}

// --- clear_blend_mask_bone_weight -------------------------------------------

FString ClaireonSkeletonTool_ClearBlendMaskBoneWeight::GetOperation() const { return TEXT("clear_blend_mask_bone_weight"); }

FString ClaireonSkeletonTool_ClearBlendMaskBoneWeight::GetDescription() const
{
    return TEXT("Clear the per-bone entry from a blend mask (reverts the bone to the mask's default weight of 0). Session-mode tool: open via skeleton_open first.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_ClearBlendMaskBoneWeight::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("mask_name"), TEXT("Blend mask name"), true);
	S.AddString(TEXT("bone_name"), TEXT("Bone entry to clear"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_ClearBlendMaskBoneWeight::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath; Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);
	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton) return MakeErrorResult(LoadError);

	FString MaskNameStr, BoneNameStr;
	if (!Arguments->TryGetStringField(TEXT("mask_name"), MaskNameStr) || MaskNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: mask_name"));
	if (!Arguments->TryGetStringField(TEXT("bone_name"), BoneNameStr) || BoneNameStr.IsEmpty())
		return MakeErrorResult(TEXT("Missing required parameter: bone_name"));

	FString ReqErr;
	UBlendProfile* Mask = RequireBlendProfile(Skeleton, MaskNameStr, /*bExpectMask*/true, ReqErr);
	if (!Mask) return MakeErrorResult(ReqErr);

	const FName BoneName(*BoneNameStr);
	const int32 BoneIdx = Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneName);
	if (BoneIdx == INDEX_NONE)
	{
		return MakeErrorResult(FString::Printf(TEXT("Bone '%s' not found on skeleton"), *BoneNameStr));
	}
	if (Mask->GetEntryIndex(BoneName) == INDEX_NONE)
	{
		return MakeErrorResult(FString::Printf(TEXT("Blend mask '%s' has no entry for bone '%s'"), *MaskNameStr, *BoneNameStr));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SkeletonClearBlendMaskBone", "MCP: Clear Blend Mask Bone"));
	Skeleton->Modify();
	Mask->Modify();
	Mask->RemoveEntry(BoneIdx);
	Skeleton->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = BuildProfileSnapshot(Skeleton, /*bMasksOnly*/true,
		FString::Printf(TEXT("Cleared mask '%s' entry for bone '%s'"), *MaskNameStr, *BoneNameStr));
	return MakeSuccessResult(Out, FString::Printf(TEXT("Cleared blend mask entry '%s'"), *BoneNameStr));
}

// ============================================================================
// skeleton_remove_blend_mask — INTENTIONAL STUB (engine bug)
// ============================================================================

FString ClaireonSkeletonTool_RemoveBlendMask::GetOperation() const { return TEXT("remove_blend_mask"); }

FString ClaireonSkeletonTool_RemoveBlendMask::GetDescription() const
{
	return TEXT("DISABLED. Removing a blend mask corrupts ALL other blend masks on the skeleton due to an engine bug "
				"in the current engine release. This tool is registered only to document that and will always return an error. "
				"If deletion is truly required, delete the mask manually in Persona while the team tracks the engine fix, "
				"and verify every remaining mask is still intact before saving.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_RemoveBlendMask::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton"), true);
	S.AddString(TEXT("mask_name"), TEXT("Name of the blend mask (ignored — call always fails)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_RemoveBlendMask::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString MaskNameStr;
	Arguments->TryGetStringField(TEXT("mask_name"), MaskNameStr);

	return MakeErrorResult(FString::Printf(
		TEXT("DISABLED: skeleton_remove_blend_mask is an intentional no-op. Removing a blend mask "
			 "('%s' requested) corrupts all other blend masks on the skeleton due to an engine bug in the current engine release. "
			 "Asset is unchanged. If deletion is truly required, do it manually in Persona while the team tracks the "
			 "engine fix, and verify all remaining masks survive before saving."),
		MaskNameStr.IsEmpty() ? TEXT("<none>") : *MaskNameStr));
}
