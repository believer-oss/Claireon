// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimTools.h"
#include "Tools/ClaireonAnimHelpers.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonLog.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimMetaData.h"
#include "AnimationModifier.h"
#include "AnimationModifiersAssetUserData.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ============================================================================
// File-scope helpers
// ============================================================================

namespace
{
	static const TArray<UAnimationModifier*> EmptyModifiers;

	const TArray<UAnimationModifier*>& GetModifierInstances(const UAnimSequence* AnimSeq)
	{
		if (const UAnimationModifiersAssetUserData* ModUserData = const_cast<UAnimSequence*>(AnimSeq)->GetAssetUserData<UAnimationModifiersAssetUserData>())
		{
			return ModUserData->GetAnimationModifierInstances();
		}
		return EmptyModifiers;
	}

	UClass* ResolveModifierClass(const FString& ClassName, FString& OutError)
	{
		UClass* BaseClass = UAnimationModifier::StaticClass();

		UClass* FoundClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
		if (FoundClass && FoundClass->IsChildOf(BaseClass))
		{
			return FoundClass;
		}

		if (ClassName.StartsWith(TEXT("U")))
		{
			FString WithoutU = ClassName.Mid(1);
			FoundClass = FindFirstObject<UClass>(*WithoutU, EFindFirstObjectOptions::NativeFirst);
			if (FoundClass && FoundClass->IsChildOf(BaseClass))
			{
				return FoundClass;
			}
		}

		IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		FARFilter Filter;
		Filter.ClassPaths.Add(UBlueprintGeneratedClass::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;

		TArray<FAssetData> AssetList;
		AssetRegistry.GetAssets(Filter, AssetList);

		for (const FAssetData& Asset : AssetList)
		{
			if (Asset.AssetName.ToString().Contains(ClassName, ESearchCase::IgnoreCase))
			{
				UClass* BPClass = Cast<UClass>(Asset.GetAsset());
				if (BPClass && BPClass->IsChildOf(BaseClass))
				{
					return BPClass;
				}
			}
		}

		OutError = FString::Printf(TEXT("Could not resolve modifier class: %s"), *ClassName);
		return nullptr;
	}

	TArray<TObjectPtr<UAnimationModifier>>* GetModifierArrayPtr(UAnimationModifiersAssetUserData* AssetUserData)
	{
		FProperty* Prop = AssetUserData->GetClass()->FindPropertyByName(TEXT("AnimationModifierInstances"));
		if (!Prop)
		{
			return nullptr;
		}
		return Prop->ContainerPtrToValuePtr<TArray<TObjectPtr<UAnimationModifier>>>(AssetUserData);
	}

	TMap<FSoftObjectPath, TObjectPtr<UAnimationModifier>>* GetAppliedModifiersMapPtr(UAnimationModifiersAssetUserData* AssetUserData)
	{
		FProperty* Prop = AssetUserData->GetClass()->FindPropertyByName(TEXT("AppliedModifiers"));
		if (!Prop)
		{
			return nullptr;
		}
		return Prop->ContainerPtrToValuePtr<TMap<FSoftObjectPath, TObjectPtr<UAnimationModifier>>>(AssetUserData);
	}
}

// ============================================================================
// claireon.anim_list_modifiers
// ============================================================================

FString ClaireonAnimTool_ListModifiers::GetName() const { return TEXT("claireon.anim_list_modifiers"); }

FString ClaireonAnimTool_ListModifiers::GetDescription() const
{
	return TEXT("List animation modifiers on an AnimSequence.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_ListModifiers::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_ListModifiers::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	UAnimSequence* AnimSeq = RequireAnimSequence(Data, Error);
	if (!AnimSeq)
		return Error;

	FString ModifiersText = ClaireonAnimHelpers::FormatModifiers(AnimSeq, true);

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetStringField(TEXT("asset_path"), Data->Animation->GetPathName());
	ResponseData->SetStringField(TEXT("modifiers_view"), ModifiersText);

	Data->LastOperationStatus = TEXT("list_modifiers");
	return MakeSuccessResult(ResponseData, FString::Printf(TEXT("Session %s: list_modifiers"), *SessionId.Left(8)));
}

// ============================================================================
// claireon.anim_add_modifier
// ============================================================================

FString ClaireonAnimTool_AddModifier::GetName() const { return TEXT("claireon.anim_add_modifier"); }

FString ClaireonAnimTool_AddModifier::GetDescription() const
{
	return TEXT("Add an animation modifier to an AnimSequence.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_AddModifier::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddString(TEXT("class_name"), TEXT("Modifier class name (native or Blueprint)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_AddModifier::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	UAnimSequence* AnimSeq = RequireAnimSequence(Data, Error);
	if (!AnimSeq)
		return Error;

	FString ClassName;
	if (!Arguments->TryGetStringField(TEXT("class_name"), ClassName) || ClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: class_name"));
	}

	FString ResolveError;
	UClass* ModifierClass = ResolveModifierClass(ClassName, ResolveError);
	if (!ModifierClass)
	{
		return MakeErrorResult(ResolveError);
	}

	// Get or create asset user data
	UAnimationModifiersAssetUserData* AssetUserData = AnimSeq->GetAssetUserData<UAnimationModifiersAssetUserData>();
	if (!AssetUserData)
	{
		AssetUserData = NewObject<UAnimationModifiersAssetUserData>(AnimSeq, UAnimationModifiersAssetUserData::StaticClass());
		AssetUserData->SetFlags(RF_Transactional);
		AnimSeq->AddAssetUserData(AssetUserData);
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimAddModifier", "MCP: Add Animation Modifier"));
	AnimSeq->Modify();

	// Create the modifier instance
	UAnimationModifier* NewModifier = NewObject<UAnimationModifier>(AssetUserData, ModifierClass, NAME_None, RF_Transactional);
	if (!NewModifier)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create modifier instance of class %s"), *ClassName));
	}

	// Add to the protected array via reflection
	TArray<TObjectPtr<UAnimationModifier>>* ArrayPtr = GetModifierArrayPtr(AssetUserData);
	if (!ArrayPtr)
	{
		return MakeErrorResult(TEXT("Failed to access AnimationModifierInstances via reflection"));
	}

	ArrayPtr->Add(NewModifier);
	AnimSeq->PostEditChange();
	AnimSeq->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(AnimSeq);

	Data->LastOperationStatus = FString::Printf(TEXT("add_modifier -> Added %s [%d]"), *ModifierClass->GetName(), ArrayPtr->Num() - 1);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// claireon.anim_remove_modifier
// ============================================================================

FString ClaireonAnimTool_RemoveModifier::GetName() const { return TEXT("claireon.anim_remove_modifier"); }

FString ClaireonAnimTool_RemoveModifier::GetDescription() const
{
	return TEXT("Remove an animation modifier by index.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_RemoveModifier::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("modifier_index"), TEXT("Index of the modifier to remove (0-based)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_RemoveModifier::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	UAnimSequence* AnimSeq = RequireAnimSequence(Data, Error);
	if (!AnimSeq)
		return Error;

	double IndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("modifier_index"), IndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: modifier_index"));
	}
	int32 Index = static_cast<int32>(IndexD);

	UAnimationModifiersAssetUserData* AssetUserData = AnimSeq->GetAssetUserData<UAnimationModifiersAssetUserData>();
	if (!AssetUserData)
	{
		return MakeErrorResult(TEXT("No modifier asset user data found on this animation"));
	}

	const TArray<UAnimationModifier*>& Modifiers = AssetUserData->GetAnimationModifierInstances();
	if (Index < 0 || Index >= Modifiers.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Modifier index %d out of range [0, %d)"), Index, Modifiers.Num()));
	}

	UAnimationModifier* ModifierToRemove = Modifiers[Index];
	FString ModifierName = ModifierToRemove ? ModifierToRemove->GetClass()->GetName() : TEXT("null");

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimRemoveModifier", "MCP: Remove Animation Modifier"));
	AnimSeq->Modify();

	// Remove from the protected array via reflection
	TArray<TObjectPtr<UAnimationModifier>>* ArrayPtr = GetModifierArrayPtr(AssetUserData);
	if (!ArrayPtr)
	{
		return MakeErrorResult(TEXT("Failed to access AnimationModifierInstances via reflection"));
	}

	// Also remove from AppliedModifiers map (mirrors RemoveAnimationModifierInstance behavior)
	if (ModifierToRemove)
	{
		TMap<FSoftObjectPath, TObjectPtr<UAnimationModifier>>* MapPtr = GetAppliedModifiersMapPtr(AssetUserData);
		if (MapPtr)
		{
			MapPtr->Remove(ModifierToRemove);
		}
	}

	ArrayPtr->RemoveAt(Index);
	AnimSeq->PostEditChange();
	AnimSeq->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(AnimSeq);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_modifier -> Removed %s [%d]"), *ModifierName, Index);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// claireon.anim_apply_modifier
// ============================================================================

FString ClaireonAnimTool_ApplyModifier::GetName() const { return TEXT("claireon.anim_apply_modifier"); }

FString ClaireonAnimTool_ApplyModifier::GetDescription() const
{
	return TEXT("Apply (execute) an animation modifier.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_ApplyModifier::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("modifier_index"), TEXT("Index of the modifier to apply (0-based)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_ApplyModifier::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	UAnimSequence* AnimSeq = RequireAnimSequence(Data, Error);
	if (!AnimSeq)
		return Error;

	double IndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("modifier_index"), IndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: modifier_index"));
	}
	int32 Index = static_cast<int32>(IndexD);

	const TArray<UAnimationModifier*>& Modifiers = GetModifierInstances(AnimSeq);
	if (Index < 0 || Index >= Modifiers.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Modifier index %d out of range [0, %d)"), Index, Modifiers.Num()));
	}

	UAnimationModifier* Modifier = Modifiers[Index];
	if (!Modifier)
	{
		return MakeErrorResult(FString::Printf(TEXT("Modifier at index %d is null"), Index));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimApplyModifier", "MCP: Apply Animation Modifier"));
	Data->Animation->Modify();

	Modifier->ApplyToAnimationSequence(AnimSeq);

	Data->LastOperationStatus = FString::Printf(TEXT("apply_modifier -> Applied %s [%d]"), *Modifier->GetClass()->GetName(), Index);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// claireon.anim_revert_modifier
// ============================================================================

FString ClaireonAnimTool_RevertModifier::GetName() const { return TEXT("claireon.anim_revert_modifier"); }

FString ClaireonAnimTool_RevertModifier::GetDescription() const
{
	return TEXT("Revert an applied animation modifier.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_RevertModifier::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("modifier_index"), TEXT("Index of the modifier to revert (0-based)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_RevertModifier::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	UAnimSequence* AnimSeq = RequireAnimSequence(Data, Error);
	if (!AnimSeq)
		return Error;

	double IndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("modifier_index"), IndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: modifier_index"));
	}
	int32 Index = static_cast<int32>(IndexD);

	const TArray<UAnimationModifier*>& Modifiers = GetModifierInstances(AnimSeq);
	if (Index < 0 || Index >= Modifiers.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Modifier index %d out of range [0, %d)"), Index, Modifiers.Num()));
	}

	UAnimationModifier* Modifier = Modifiers[Index];
	if (!Modifier)
	{
		return MakeErrorResult(FString::Printf(TEXT("Modifier at index %d is null"), Index));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimRevertModifier", "MCP: Revert Animation Modifier"));
	Data->Animation->Modify();

	Modifier->RevertFromAnimationSequence(AnimSeq);

	Data->LastOperationStatus = FString::Printf(TEXT("revert_modifier -> Reverted %s [%d]"), *Modifier->GetClass()->GetName(), Index);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// claireon.anim_list_metadata
// ============================================================================

FString ClaireonAnimTool_ListMetadata::GetName() const { return TEXT("claireon.anim_list_metadata"); }

FString ClaireonAnimTool_ListMetadata::GetDescription() const
{
	return TEXT("List metadata objects on the animation.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_ListMetadata::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_ListMetadata::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	FString MetadataText = ClaireonAnimHelpers::FormatMetadata(Data->Animation.Get(), true);

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetStringField(TEXT("asset_path"), Data->Animation->GetPathName());
	ResponseData->SetStringField(TEXT("metadata_view"), MetadataText);

	Data->LastOperationStatus = TEXT("list_metadata");
	return MakeSuccessResult(ResponseData, FString::Printf(TEXT("Session %s: list_metadata"), *SessionId.Left(8)));
}

// ============================================================================
// claireon.anim_add_metadata
// ============================================================================

FString ClaireonAnimTool_AddMetadata::GetName() const { return TEXT("claireon.anim_add_metadata"); }

FString ClaireonAnimTool_AddMetadata::GetDescription() const
{
	return TEXT("Add a metadata object to the animation.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_AddMetadata::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddString(TEXT("class_name"), TEXT("Metadata class name (native or Blueprint)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_AddMetadata::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	FString ClassName;
	if (!Arguments->TryGetStringField(TEXT("class_name"), ClassName) || ClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: class_name"));
	}

	FString ResolveError;
	UClass* MetaDataClass = ClaireonAnimHelpers::ResolveMetaDataClass(ClassName, ResolveError);
	if (!MetaDataClass)
	{
		return MakeErrorResult(ResolveError);
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimAddMetadata", "MCP: Add Animation Metadata"));
	Data->Animation->Modify();

	UAnimMetaData* NewMetaData = NewObject<UAnimMetaData>(Data->Animation.Get(), MetaDataClass);
	if (!NewMetaData)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create metadata of class %s"), *ClassName));
	}

	Data->Animation->AddMetaData(NewMetaData);

	Data->LastOperationStatus = FString::Printf(TEXT("add_metadata -> Added %s"), *MetaDataClass->GetName());
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// claireon.anim_remove_metadata
// ============================================================================

FString ClaireonAnimTool_RemoveMetadata::GetName() const { return TEXT("claireon.anim_remove_metadata"); }

FString ClaireonAnimTool_RemoveMetadata::GetDescription() const
{
	return TEXT("Remove a metadata object by index.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_RemoveMetadata::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("metadata_index"), TEXT("Index of the metadata object to remove (0-based)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_RemoveMetadata::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	double IndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("metadata_index"), IndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: metadata_index"));
	}
	int32 Index = static_cast<int32>(IndexD);

	const TArray<UAnimMetaData*>& MetaDataArray = Data->Animation->GetMetaData();
	if (Index < 0 || Index >= MetaDataArray.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Metadata index %d out of range [0, %d)"), Index, MetaDataArray.Num()));
	}

	UAnimMetaData* MetaDataObj = MetaDataArray[Index];
	if (!MetaDataObj)
	{
		return MakeErrorResult(FString::Printf(TEXT("Metadata at index %d is null"), Index));
	}

	FString MetaDataName = MetaDataObj->GetClass()->GetName();

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimRemoveMetadata", "MCP: Remove Animation Metadata"));
	Data->Animation->Modify();

	Data->Animation->RemoveMetaData(MetaDataObj);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_metadata -> Removed %s [%d]"), *MetaDataName, Index);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// claireon.anim_set_metadata_property
// ============================================================================

FString ClaireonAnimTool_SetMetadataProperty::GetName() const { return TEXT("claireon.anim_set_metadata_property"); }

FString ClaireonAnimTool_SetMetadataProperty::GetDescription() const
{
	return TEXT("Set a property on a metadata object.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_SetMetadataProperty::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddInteger(TEXT("metadata_index"), TEXT("Index of the metadata object (0-based)"), true);
	S.AddString(TEXT("property_name"), TEXT("Property name or dot-path"), true);
	S.AddString(TEXT("value"), TEXT("New value for the property"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_SetMetadataProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	double IndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("metadata_index"), IndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: metadata_index"));
	}
	int32 Index = static_cast<int32>(IndexD);

	FString PropertyName;
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_name"));
	}

	FString Value;
	if (!Arguments->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	const TArray<UAnimMetaData*>& MetaDataArray = Data->Animation->GetMetaData();
	if (Index < 0 || Index >= MetaDataArray.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Metadata index %d out of range [0, %d)"), Index, MetaDataArray.Num()));
	}

	UAnimMetaData* MetaDataObj = MetaDataArray[Index];
	if (!MetaDataObj)
	{
		return MakeErrorResult(FString::Printf(TEXT("Metadata at index %d is null"), Index));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimSetMetadataProp", "MCP: Set Metadata Property"));
	Data->Animation->Modify();
	MetaDataObj->Modify();

	FString WriteError;
	if (!ClaireonPropertyUtils::WritePropertyByPath(MetaDataObj, PropertyName, Value, WriteError))
	{
		return MakeErrorResult(WriteError);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("set_metadata_property -> [%d].%s = %s"), Index, *PropertyName, *Value);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// claireon.anim_set_property
// ============================================================================

FString ClaireonAnimTool_SetProperty::GetName() const { return TEXT("claireon.anim_set_property"); }

FString ClaireonAnimTool_SetProperty::GetDescription() const
{
	return TEXT("Set a property on the animation asset itself.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_SetProperty::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddSessionParams();
	S.AddString(TEXT("property_name"), TEXT("Property name (e.g. rate_scale, root_motion_enabled, blend_in_time, or any UPROPERTY path)"), true);
	S.AddString(TEXT("value"), TEXT("New value for the property"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_SetProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimEditToolData* Data;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
		return Error;

	FString PropertyName;
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_name"));
	}

	FString Value;
	if (!Arguments->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimSetProperty", "MCP: Set Animation Property"));
	Data->Animation->Modify();

	UAnimSequenceBase* Anim = Data->Animation.Get();

	// Handle well-known property names with direct accessors
	if (PropertyName == TEXT("rate_scale"))
	{
		Anim->RateScale = FCString::Atof(*Value);
		Data->LastOperationStatus = FString::Printf(TEXT("set_property -> rate_scale = %s"), *Value);
		return BuildStateResponse(SessionId, Data);
	}
	else if (PropertyName == TEXT("root_motion_enabled"))
	{
		UAnimSequence* AnimSeq = Cast<UAnimSequence>(Anim);
		if (!AnimSeq)
		{
			return MakeErrorResult(TEXT("root_motion_enabled only applies to AnimSequence"));
		}
		AnimSeq->bEnableRootMotion = Value.ToBool();
		Data->LastOperationStatus = FString::Printf(TEXT("set_property -> root_motion_enabled = %s"), *Value);
		return BuildStateResponse(SessionId, Data);
	}
	else if (PropertyName == TEXT("force_root_lock"))
	{
		UAnimSequence* AnimSeq = Cast<UAnimSequence>(Anim);
		if (!AnimSeq)
		{
			return MakeErrorResult(TEXT("force_root_lock only applies to AnimSequence"));
		}
		AnimSeq->bForceRootLock = Value.ToBool();
		Data->LastOperationStatus = FString::Printf(TEXT("set_property -> force_root_lock = %s"), *Value);
		return BuildStateResponse(SessionId, Data);
	}
	else if (PropertyName == TEXT("root_motion_root_lock"))
	{
		UAnimSequence* AnimSeq = Cast<UAnimSequence>(Anim);
		if (!AnimSeq)
		{
			return MakeErrorResult(TEXT("root_motion_root_lock only applies to AnimSequence"));
		}
		if (Value == TEXT("RefPose") || Value == TEXT("0"))
		{
			AnimSeq->RootMotionRootLock = ERootMotionRootLock::RefPose;
		}
		else if (Value == TEXT("AnimFirstFrame") || Value == TEXT("1"))
		{
			AnimSeq->RootMotionRootLock = ERootMotionRootLock::AnimFirstFrame;
		}
		else if (Value == TEXT("Zero") || Value == TEXT("2"))
		{
			AnimSeq->RootMotionRootLock = ERootMotionRootLock::Zero;
		}
		else
		{
			return MakeErrorResult(FString::Printf(TEXT("Invalid root_motion_root_lock value: %s (expected RefPose, AnimFirstFrame, or Zero)"), *Value));
		}
		Data->LastOperationStatus = FString::Printf(TEXT("set_property -> root_motion_root_lock = %s"), *Value);
		return BuildStateResponse(SessionId, Data);
	}
	else if (PropertyName == TEXT("blend_in_time"))
	{
		UAnimMontage* Montage = Cast<UAnimMontage>(Anim);
		if (!Montage)
		{
			return MakeErrorResult(TEXT("blend_in_time only applies to AnimMontage"));
		}
		Montage->BlendIn.SetBlendTime(FCString::Atof(*Value));
		Data->LastOperationStatus = FString::Printf(TEXT("set_property -> blend_in_time = %s"), *Value);
		return BuildStateResponse(SessionId, Data);
	}
	else if (PropertyName == TEXT("blend_out_time"))
	{
		UAnimMontage* Montage = Cast<UAnimMontage>(Anim);
		if (!Montage)
		{
			return MakeErrorResult(TEXT("blend_out_time only applies to AnimMontage"));
		}
		Montage->BlendOut.SetBlendTime(FCString::Atof(*Value));
		Data->LastOperationStatus = FString::Printf(TEXT("set_property -> blend_out_time = %s"), *Value);
		return BuildStateResponse(SessionId, Data);
	}
	else
	{
		// Fallback to generic property utils
		FString WriteError;
		if (!ClaireonPropertyUtils::WritePropertyByPath(Anim, PropertyName, Value, WriteError))
		{
			return MakeErrorResult(WriteError);
		}
		Data->LastOperationStatus = FString::Printf(TEXT("set_property -> %s = %s"), *PropertyName, *Value);
		return BuildStateResponse(SessionId, Data);
	}
}
