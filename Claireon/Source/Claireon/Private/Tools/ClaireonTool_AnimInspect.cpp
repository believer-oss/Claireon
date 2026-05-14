// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AnimInspect.h"
#include "Tools/ClaireonAnimHelpers.h"
#include "ClaireonLog.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "AnimationModifiersAssetUserData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"

FString ClaireonTool_AnimInspect::GetName() const
{
	return TEXT("claireon.anim_inspect");
}

FString ClaireonTool_AnimInspect::GetDescription() const
{
	return TEXT("Read the structure of an animation asset (AnimSequence, AnimMontage, or AnimComposite) by path. Stateless / read-only / non-session: never mutates and requires no open session. Auto-detects asset type. Returns notifies, curves, sync markers, and montage-specific data. Use detail_level='summary' or 'full'.");
}

TSharedPtr<FJsonObject> ClaireonTool_AnimInspect::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"),
		TEXT("Unreal asset path to the animation (e.g. /Game/Char/STELLA/Anim/AM_Combo)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// detail_level - optional
	TSharedPtr<FJsonObject> DetailProp = MakeShared<FJsonObject>();
	DetailProp->SetStringField(TEXT("type"), TEXT("string"));
	DetailProp->SetStringField(TEXT("description"),
		TEXT("Level of detail: 'summary' for counts and names only, 'full' for all properties (default: full)"));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("summary")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("full")));
		DetailProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("detail_level"), DetailProp);

	// focus - optional
	TSharedPtr<FJsonObject> FocusProp = MakeShared<FJsonObject>();
	FocusProp->SetStringField(TEXT("type"), TEXT("string"));
	FocusProp->SetStringField(TEXT("description"),
		TEXT("Show only a specific aspect of the animation. If omitted, shows everything."));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("notifies")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("curves")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("sections")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("slots")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("sync_markers")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("properties")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("modifiers")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("metadata")));
		FocusProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("focus"), FocusProp);

	// notify_index - optional
	TSharedPtr<FJsonObject> NotifyIdxProp = MakeShared<FJsonObject>();
	NotifyIdxProp->SetStringField(TEXT("type"), TEXT("integer"));
	NotifyIdxProp->SetStringField(TEXT("description"),
		TEXT("Focus on a single notify by index (0-based). Shows all properties of that notify including sub-object details."));
	Properties->SetObjectField(TEXT("notify_index"), NotifyIdxProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_AnimInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// 1. Parse asset_path (required)
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	// 2. Parse optional params
	FString DetailLevel = TEXT("full");
	Arguments->TryGetStringField(TEXT("detail_level"), DetailLevel);
	const bool bFullDetail = (DetailLevel != TEXT("summary"));

	FString Focus;
	Arguments->TryGetStringField(TEXT("focus"), Focus);

	int32 NotifyIndex = -1;
	Arguments->TryGetNumberField(TEXT("notify_index"), NotifyIndex);

	// 3. Load asset (auto-detects type)
	FString AssetType;
	FString Error;
	UAnimSequenceBase* Anim = ClaireonAnimHelpers::LoadAnimAsset(AssetPath, AssetType, Error);
	if (!Anim)
	{
		return MakeErrorResult(Error);
	}

	// 4. If notify_index is specified, focus on that single notify
	if (NotifyIndex >= 0)
	{
		if (NotifyIndex >= Anim->Notifies.Num())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("notify_index %d is out of range. Asset has %d notifies (valid: 0-%d)."),
				NotifyIndex, Anim->Notifies.Num(),
				FMath::Max(0, Anim->Notifies.Num() - 1)));
		}

		const FString NotifyDetail = ClaireonAnimHelpers::FormatSingleNotify(Anim, NotifyIndex);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		Data->SetStringField(TEXT("asset_type"), AssetType);
		Data->SetNumberField(TEXT("notify_index"), NotifyIndex);
		Data->SetNumberField(TEXT("total_notifies"), Anim->Notifies.Num());
		Data->SetStringField(TEXT("structure"), NotifyDetail);

		const FString Summary = FString::Printf(TEXT("%s notify[%d] detail"),
			*FPaths::GetBaseFilename(AssetPath), NotifyIndex);

		return MakeSuccessResult(Data, Summary);
	}

	// 5. Build structure text using helpers
	const FString StructureText = ClaireonAnimHelpers::FormatAnimStructure(Anim, AssetType, bFullDetail, Focus);

	// 6. Gather metadata counts
	const int32 NotifyCount = Anim->Notifies.Num();

	int32 CurveCount = 0;
	int32 NumFrames = 0;
	double FrameRateDecimal = 0.0;
	if (const IAnimationDataModel* DataModel = Anim->GetDataModel())
	{
		CurveCount = DataModel->GetNumberOfFloatCurves();
		NumFrames = DataModel->GetNumberOfFrames();
		FrameRateDecimal = DataModel->GetFrameRate().AsDecimal();
	}

	int32 SectionCount = 0;
	int32 SlotCount = 0;
	if (const UAnimMontage* Montage = Cast<UAnimMontage>(Anim))
	{
		SectionCount = Montage->CompositeSections.Num();
		SlotCount = Montage->SlotAnimTracks.Num();
	}

	int32 SyncMarkerCount = 0;
	bool bHasRootMotion = false;
	bool bIsAdditive = false;
	int32 ModifierCount = 0;
	if (const UAnimSequence* AnimSeq = Cast<UAnimSequence>(Anim))
	{
		SyncMarkerCount = AnimSeq->AuthoredSyncMarkers.Num();
		bHasRootMotion = AnimSeq->bEnableRootMotion;
		bIsAdditive = (AnimSeq->AdditiveAnimType != AAT_None);
		if (const UAnimationModifiersAssetUserData* ModUserData = const_cast<UAnimSequence*>(AnimSeq)->GetAssetUserData<UAnimationModifiersAssetUserData>())
		{
			ModifierCount = ModUserData->GetAnimationModifierInstances().Num();
		}
	}

	const int32 MetadataCount = Anim->GetMetaData().Num();
	const FString SkeletonPath = Anim->GetSkeleton() ? Anim->GetSkeleton()->GetPathName() : TEXT("None");

	// 7. Build JSON response
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetStringField(TEXT("asset_type"), AssetType);
	Data->SetNumberField(TEXT("length_seconds"), Anim->GetPlayLength());
	Data->SetNumberField(TEXT("frame_rate"), FrameRateDecimal);
	Data->SetNumberField(TEXT("num_frames"), NumFrames);
	Data->SetNumberField(TEXT("rate_scale"), Anim->RateScale);
	Data->SetStringField(TEXT("skeleton_path"), SkeletonPath);
	Data->SetBoolField(TEXT("has_root_motion"), bHasRootMotion);
	Data->SetBoolField(TEXT("is_additive"), bIsAdditive);
	Data->SetNumberField(TEXT("notify_count"), NotifyCount);
	Data->SetNumberField(TEXT("curve_count"), CurveCount);
	Data->SetNumberField(TEXT("section_count"), SectionCount);
	Data->SetNumberField(TEXT("slot_count"), SlotCount);
	Data->SetNumberField(TEXT("sync_marker_count"), SyncMarkerCount);
	Data->SetNumberField(TEXT("modifier_count"), ModifierCount);
	Data->SetNumberField(TEXT("metadata_count"), MetadataCount);
	Data->SetStringField(TEXT("structure"), StructureText);

	// 8. Build summary
	FString Summary = FString::Printf(TEXT("%s (%s): %d notifies, %d curves"),
		*FPaths::GetBaseFilename(AssetPath), *AssetType,
		NotifyCount, CurveCount);

	if (SectionCount > 0)
	{
		Summary += FString::Printf(TEXT(", %d sections"), SectionCount);
	}

	return MakeSuccessResult(Data, Summary);
}
