// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSkeletonHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "Animation/Skeleton.h"
#include "Animation/BlendProfile.h"
#include "Animation/AnimCurveMetadata.h"
#include "ReferenceSkeleton.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/AssetUserData.h"
#include "UObject/SoftObjectPath.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonSkeletonHelpers
{
	USkeleton* LoadSkeleton(const FString& AssetPath, FString& OutError)
	{
		if (AssetPath.IsEmpty())
		{
			OutError = TEXT("Missing required parameter: skeleton_path");
			return nullptr;
		}

		auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
		if (!ResolveResult.bSuccess)
		{
			OutError = ResolveResult.Error;
			return nullptr;
		}

		FSoftObjectPath SoftPath(ResolveResult.ResolvedPath.Path);
		UObject* LoadedObj = SoftPath.TryLoad();
		if (!LoadedObj)
		{
			LoadedObj = LoadObject<USkeleton>(nullptr, *ResolveResult.ResolvedPath.Path);
		}
		if (!LoadedObj)
		{
			OutError = FString::Printf(TEXT("Failed to load asset at '%s'"), *ResolveResult.ResolvedPath.Path);
			return nullptr;
		}

		USkeleton* Skeleton = Cast<USkeleton>(LoadedObj);
		if (!Skeleton)
		{
			OutError = FString::Printf(TEXT("Asset '%s' is not a USkeleton (got %s)"),
				*ResolveResult.ResolvedPath.Path, *LoadedObj->GetClass()->GetName());
			return nullptr;
		}
		return Skeleton;
	}

	bool ParseBlendProfileMode(const FString& ModeStr, EBlendProfileMode& OutMode, FString& OutError)
	{
		if (ModeStr.Equals(TEXT("TimeFactor"), ESearchCase::IgnoreCase))
		{
			OutMode = EBlendProfileMode::TimeFactor;
			return true;
		}
		if (ModeStr.Equals(TEXT("WeightFactor"), ESearchCase::IgnoreCase))
		{
			OutMode = EBlendProfileMode::WeightFactor;
			return true;
		}
		if (ModeStr.Equals(TEXT("BlendMask"), ESearchCase::IgnoreCase))
		{
			OutMode = EBlendProfileMode::BlendMask;
			return true;
		}
		OutError = FString::Printf(TEXT("Invalid mode '%s'. Expected one of: TimeFactor, WeightFactor, BlendMask"), *ModeStr);
		return false;
	}

	FString BlendProfileModeToString(EBlendProfileMode Mode)
	{
		switch (Mode)
		{
		case EBlendProfileMode::TimeFactor:   return TEXT("TimeFactor");
		case EBlendProfileMode::WeightFactor: return TEXT("WeightFactor");
		case EBlendProfileMode::BlendMask:    return TEXT("BlendMask");
		default:                              return TEXT("Unknown");
		}
	}

	bool ReadVectorFromJson(const TSharedPtr<FJsonObject>& Obj, const FVector& Default, FVector& OutVec)
	{
		if (!Obj.IsValid())
		{
			OutVec = Default;
			return false;
		}
		OutVec = Default;
		double V = 0.0;
		if (Obj->TryGetNumberField(TEXT("x"), V)) OutVec.X = V;
		if (Obj->TryGetNumberField(TEXT("y"), V)) OutVec.Y = V;
		if (Obj->TryGetNumberField(TEXT("z"), V)) OutVec.Z = V;
		return true;
	}

	bool ReadRotatorFromJson(const TSharedPtr<FJsonObject>& Obj, const FRotator& Default, FRotator& OutRot)
	{
		if (!Obj.IsValid())
		{
			OutRot = Default;
			return false;
		}
		OutRot = Default;
		double V = 0.0;
		if (Obj->TryGetNumberField(TEXT("pitch"), V)) OutRot.Pitch = V;
		if (Obj->TryGetNumberField(TEXT("yaw"), V))   OutRot.Yaw = V;
		if (Obj->TryGetNumberField(TEXT("roll"), V))  OutRot.Roll = V;
		return true;
	}

	// ========================================================================
	// Inspection builders
	// ========================================================================

	TSharedPtr<FJsonObject> BuildOverview(const USkeleton* Skeleton)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("asset_path"), Skeleton->GetPathName());
		Out->SetStringField(TEXT("asset_name"), Skeleton->GetName());

		const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
		Out->SetNumberField(TEXT("bone_count"), Ref.GetNum());
		Out->SetNumberField(TEXT("raw_bone_count"), Ref.GetRawBoneNum());
		Out->SetNumberField(TEXT("virtual_bone_count"), Skeleton->GetVirtualBones().Num());
		Out->SetNumberField(TEXT("socket_count"), Skeleton->Sockets.Num());

		int32 ProfileCount = 0;
		int32 MaskCount = 0;
		for (const TObjectPtr<UBlendProfile>& P : Skeleton->BlendProfiles)
		{
			if (!P) continue;
			if (P->IsBlendMask()) ++MaskCount; else ++ProfileCount;
		}
		Out->SetNumberField(TEXT("blend_profile_count"), ProfileCount);
		Out->SetNumberField(TEXT("blend_mask_count"), MaskCount);

#if WITH_EDITORONLY_DATA
		Out->SetNumberField(TEXT("animation_notify_name_count"), Skeleton->AnimationNotifies.Num());
#endif
		Out->SetNumberField(TEXT("curve_metadata_count"), Skeleton->GetNumCurveMetaData());

		if (const TArray<UAssetUserData*>* UserDataArray = Skeleton->GetAssetUserDataArray())
		{
			Out->SetNumberField(TEXT("asset_user_data_count"), UserDataArray->Num());
		}
		else
		{
			Out->SetNumberField(TEXT("asset_user_data_count"), 0);
		}
		return Out;
	}

	TSharedPtr<FJsonObject> BuildBones(const USkeleton* Skeleton)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
		const TArray<FMeshBoneInfo>& BoneInfo = Ref.GetRefBoneInfo();

		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(BoneInfo.Num());
		for (int32 i = 0; i < BoneInfo.Num(); ++i)
		{
			TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
			B->SetNumberField(TEXT("index"), i);
			B->SetStringField(TEXT("name"), BoneInfo[i].Name.ToString());
			B->SetNumberField(TEXT("parent_index"), BoneInfo[i].ParentIndex);
			if (BoneInfo[i].ParentIndex >= 0 && BoneInfo[i].ParentIndex < BoneInfo.Num())
			{
				B->SetStringField(TEXT("parent_name"), BoneInfo[BoneInfo[i].ParentIndex].Name.ToString());
			}
			Arr.Add(MakeShared<FJsonValueObject>(B));
		}
		Out->SetArrayField(TEXT("bones"), Arr);
		Out->SetNumberField(TEXT("count"), BoneInfo.Num());
		return Out;
	}

	TSharedPtr<FJsonObject> BuildVirtualBones(const USkeleton* Skeleton)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		const TArray<FVirtualBone>& VBs = Skeleton->GetVirtualBones();
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(VBs.Num());
		for (const FVirtualBone& VB : VBs)
		{
			TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
			B->SetStringField(TEXT("virtual_bone_name"), VB.VirtualBoneName.ToString());
			B->SetStringField(TEXT("source_bone"), VB.SourceBoneName.ToString());
			B->SetStringField(TEXT("target_bone"), VB.TargetBoneName.ToString());
			Arr.Add(MakeShared<FJsonValueObject>(B));
		}
		Out->SetArrayField(TEXT("virtual_bones"), Arr);
		Out->SetNumberField(TEXT("count"), VBs.Num());
		return Out;
	}

	TSharedPtr<FJsonObject> BuildSockets(const USkeleton* Skeleton)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(Skeleton->Sockets.Num());
		for (const TObjectPtr<USkeletalMeshSocket>& Socket : Skeleton->Sockets)
		{
			if (!Socket) continue;
			TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetStringField(TEXT("socket_name"), Socket->SocketName.ToString());
			S->SetStringField(TEXT("bone_name"), Socket->BoneName.ToString());

			TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
			Loc->SetNumberField(TEXT("x"), Socket->RelativeLocation.X);
			Loc->SetNumberField(TEXT("y"), Socket->RelativeLocation.Y);
			Loc->SetNumberField(TEXT("z"), Socket->RelativeLocation.Z);
			S->SetObjectField(TEXT("relative_location"), Loc);

			TSharedPtr<FJsonObject> Rot = MakeShared<FJsonObject>();
			Rot->SetNumberField(TEXT("pitch"), Socket->RelativeRotation.Pitch);
			Rot->SetNumberField(TEXT("yaw"), Socket->RelativeRotation.Yaw);
			Rot->SetNumberField(TEXT("roll"), Socket->RelativeRotation.Roll);
			S->SetObjectField(TEXT("relative_rotation"), Rot);

			TSharedPtr<FJsonObject> Scl = MakeShared<FJsonObject>();
			Scl->SetNumberField(TEXT("x"), Socket->RelativeScale.X);
			Scl->SetNumberField(TEXT("y"), Socket->RelativeScale.Y);
			Scl->SetNumberField(TEXT("z"), Socket->RelativeScale.Z);
			S->SetObjectField(TEXT("relative_scale"), Scl);

			S->SetBoolField(TEXT("force_always_animated"), Socket->bForceAlwaysAnimated);
			Arr.Add(MakeShared<FJsonValueObject>(S));
		}
		Out->SetArrayField(TEXT("sockets"), Arr);
		Out->SetNumberField(TEXT("count"), Arr.Num());
		return Out;
	}

	TSharedPtr<FJsonObject> BuildMetadata(const USkeleton* Skeleton)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();

		// Animation notify names
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
#if WITH_EDITORONLY_DATA
			for (const FName& N : Skeleton->AnimationNotifies)
			{
				Arr.Add(MakeShared<FJsonValueString>(N.ToString()));
			}
#endif
			Out->SetArrayField(TEXT("animation_notify_names"), Arr);
		}

		// Curve metadata
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			Skeleton->ForEachCurveMetaData([&Arr](FName CurveName, const FCurveMetaData& Meta)
			{
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("curve_name"), CurveName.ToString());
				Entry->SetBoolField(TEXT("material"), Meta.Type.bMaterial);
				Entry->SetBoolField(TEXT("morph_target"), Meta.Type.bMorphtarget);
				Entry->SetNumberField(TEXT("max_lod"), Meta.MaxLOD);
				TArray<TSharedPtr<FJsonValue>> Bones;
				for (const FBoneReference& B : Meta.LinkedBones)
				{
					Bones.Add(MakeShared<FJsonValueString>(B.BoneName.ToString()));
				}
				Entry->SetArrayField(TEXT("linked_bones"), Bones);
				Arr.Add(MakeShared<FJsonValueObject>(Entry));
			});
			Out->SetArrayField(TEXT("curve_metadata"), Arr);
		}

		// Asset user data
		{
			TArray<TSharedPtr<FJsonValue>> Arr;
			if (const TArray<UAssetUserData*>* UserDataArray = Skeleton->GetAssetUserDataArray())
			{
				for (UAssetUserData* UD : *UserDataArray)
				{
					if (!UD) continue;
					TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetStringField(TEXT("class"), UD->GetClass()->GetPathName());
					Entry->SetStringField(TEXT("class_name"), UD->GetClass()->GetName());
					Arr.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
			Out->SetArrayField(TEXT("asset_user_data"), Arr);
		}
		return Out;
	}

	TSharedPtr<FJsonObject> BuildBlendProfiles(const USkeleton* Skeleton, bool bMasksOnly)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const TObjectPtr<UBlendProfile>& P : Skeleton->BlendProfiles)
		{
			if (!P) continue;
			const bool bIsMask = P->IsBlendMask();
			if (bMasksOnly && !bIsMask) continue;
			if (!bMasksOnly && bIsMask) continue;

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), P->GetName());
			Entry->SetStringField(TEXT("mode"), BlendProfileModeToString(P->Mode));

			TArray<TSharedPtr<FJsonValue>> Entries;
			const int32 NumEntries = P->GetNumBlendEntries();
			for (int32 i = 0; i < NumEntries; ++i)
			{
				const FBlendProfileBoneEntry& BE = P->GetEntry(i);
				TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
				E->SetStringField(TEXT("bone_name"), BE.BoneReference.BoneName.ToString());
				E->SetNumberField(TEXT("scale"), BE.BlendScale);
				Entries.Add(MakeShared<FJsonValueObject>(E));
			}
			Entry->SetArrayField(TEXT("entries"), Entries);
			Entry->SetNumberField(TEXT("entry_count"), NumEntries);
			Arr.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Out->SetArrayField(bMasksOnly ? TEXT("blend_masks") : TEXT("blend_profiles"), Arr);
		Out->SetNumberField(TEXT("count"), Arr.Num());
		return Out;
	}

	USkeletalMeshSocket* FindSocket(USkeleton* Skeleton, FName SocketName)
	{
		return Skeleton ? Skeleton->FindSocket(SocketName) : nullptr;
	}

	UBlendProfile* FindBlendProfile(USkeleton* Skeleton, FName ProfileName)
	{
		return Skeleton ? Skeleton->GetBlendProfile(ProfileName) : nullptr;
	}
}
