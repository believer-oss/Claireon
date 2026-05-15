// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSkeletonTools_Inspect.h"
#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ============================================================================
// claireon.skeleton_inspect
// ============================================================================

FString ClaireonSkeletonTool_Inspect::GetName() const { return TEXT("claireon.skeleton_inspect"); }

FString ClaireonSkeletonTool_Inspect::GetDescription() const
{
	return TEXT("Inspect a USkeleton asset. Default 'overview' returns counts for bones/virtual bones/sockets/blend profiles/blend masks/metadata. "
				"Use focus='bones' | 'virtual_bones' | 'sockets' | 'metadata' | 'blend_profiles' | 'blend_masks' to drill into one section. "
				"Metadata covers three subsystems: skeleton-level animation notify names, curve metadata (linked bones/LOD/flags), and asset user data.");
}

TSharedPtr<FJsonObject> ClaireonSkeletonTool_Inspect::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("skeleton_path"), TEXT("Unreal asset path to the USkeleton (e.g. /Game/Characters/Foo/Foo_Skeleton)"), true);
	S.AddEnum(TEXT("focus"), TEXT("Which section to return in full detail. Default: overview"),
		{TEXT("overview"), TEXT("bones"), TEXT("virtual_bones"), TEXT("sockets"), TEXT("metadata"), TEXT("blend_profiles"), TEXT("blend_masks")});
	return S.Build();
}

IClaireonTool::FToolResult ClaireonSkeletonTool_Inspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SkeletonPath;
	Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath);

	FString LoadError;
	USkeleton* Skeleton = ClaireonSkeletonHelpers::LoadSkeleton(SkeletonPath, LoadError);
	if (!Skeleton)
	{
		return MakeErrorResult(LoadError);
	}

	FString Focus = TEXT("overview");
	Arguments->TryGetStringField(TEXT("focus"), Focus);

	TSharedPtr<FJsonObject> Result = ClaireonSkeletonHelpers::BuildOverview(Skeleton);
	Result->SetStringField(TEXT("focus"), Focus);

	if (Focus == TEXT("bones"))
	{
		Result->SetObjectField(TEXT("bones_section"), ClaireonSkeletonHelpers::BuildBones(Skeleton));
	}
	else if (Focus == TEXT("virtual_bones"))
	{
		Result->SetObjectField(TEXT("virtual_bones_section"), ClaireonSkeletonHelpers::BuildVirtualBones(Skeleton));
	}
	else if (Focus == TEXT("sockets"))
	{
		Result->SetObjectField(TEXT("sockets_section"), ClaireonSkeletonHelpers::BuildSockets(Skeleton));
	}
	else if (Focus == TEXT("metadata"))
	{
		Result->SetObjectField(TEXT("metadata_section"), ClaireonSkeletonHelpers::BuildMetadata(Skeleton));
	}
	else if (Focus == TEXT("blend_profiles"))
	{
		Result->SetObjectField(TEXT("blend_profiles_section"), ClaireonSkeletonHelpers::BuildBlendProfiles(Skeleton, /*bMasksOnly*/false));
	}
	else if (Focus == TEXT("blend_masks"))
	{
		Result->SetObjectField(TEXT("blend_masks_section"), ClaireonSkeletonHelpers::BuildBlendProfiles(Skeleton, /*bMasksOnly*/true));
	}
	// overview: counts already in Result

	return MakeSuccessResult(Result, FString::Printf(TEXT("Inspected skeleton '%s' (focus=%s)"), *Skeleton->GetName(), *Focus));
}
