// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AnimEdit.h"
#include "Tools/ClaireonAnimHelpers.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonLog.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Factories/AnimMontageFactory.h"
#include "Factories/AnimCompositeFactory.h"
#include "Curves/RichCurve.h"
#include "AnimationModifier.h"
#include "AnimationModifiersAssetUserData.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "FileHelpers.h"
#include "Editor.h"
#include "UObject/Package.h"
#include "Misc/Paths.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Animation/Skeleton.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "UObject/SavePackage.h"

// Using statements
using FToolResult = IClaireonTool::FToolResult;

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

	/** Validate a target path for new asset creation. Returns canonicalized path and short asset name. */
	bool ValidateNewAssetPath(const FString& InPath, FString& OutCanonPath, FString& OutAssetName, FString& OutError)
	{
		OutCanonPath = FClaireonSessionManager::CanonicalizePath(InPath);
		if (OutCanonPath.IsEmpty())
		{
			OutError = TEXT("Invalid asset path. Must start with /Game/.");
			return false;
		}
		if (StaticFindObject(nullptr, nullptr, *OutCanonPath))
		{
			OutError = FString::Printf(TEXT("Asset already exists at '%s'"), *OutCanonPath);
			return false;
		}
		OutAssetName = FPackageName::GetShortName(OutCanonPath);
		return true;
	}

	/** Save a newly created asset to disk. Uses computed filename since the package has no file yet. */
	bool SaveNewAsset(UObject* Asset, FString& OutError)
	{
		UPackage* Package = Asset->GetOutermost();
		Package->MarkPackageDirty();

		FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		FSavePackageResultStruct Result = UPackage::Save(Package, Asset, *PackageFileName, SaveArgs);
		if (Result.Result != ESavePackageResult::Success)
		{
			OutError = FString::Printf(TEXT("Failed to save package '%s'"), *Package->GetName());
			return false;
		}
		return true;
	}
}

// Static tool data storage
TMap<FString, FAnimEditToolData> ClaireonTool_AnimEdit::ToolData;
bool ClaireonTool_AnimEdit::bDelegateRegistered = false;

// ============================================================================
// Tool Interface Implementation
// ============================================================================

FString ClaireonTool_AnimEdit::GetName() const
{
	return TEXT("claireon.anim_edit");
}

FString ClaireonTool_AnimEdit::GetDescription() const
{
	return TEXT("Session-based editor for animation assets. Supports editing notifies (including skeleton-style notifies), curves, montage sections, data modifiers, metadata, and properties. Use 'open' to start, then operations to modify, 'save' to persist, and 'close' to end.");
}

FString ClaireonTool_AnimEdit::GetFullDescription() const
{
	return TEXT("Interactively edit animation assets (AnimSequence, AnimMontage, AnimComposite) using a session-based model. "
				"Start with 'open' to begin a session, then use operations to modify the animation. "
				"Finish with 'save' to persist changes and 'close' to end the session.\n\n"
				"Asset creation (no session needed): create_montage, create_composite, duplicate_asset\n"
				"Session operations: open, close, get_state, save\n"
				"Notify operations: add_notify, remove_notify, move_notify, duplicate_notify, set_notify_property, get_notify_property, list_notify_properties, add_notify_track, remove_notify_track, rename_notify_track, reorder_notify_track\n"
				"Curve operations: add_curve, remove_curve, add_curve_key, remove_curve_key, set_curve_key_property\n"
				"Montage section operations (montage only): add_section, remove_section, set_section_link, set_section_link_method\n"
				"Montage slot/segment operations (montage only): add_segment, remove_segment, set_segment_property, inspect_segment, retime_segment, add_slot, remove_slot, set_slot_property\n"
				"Montage batch operations: batch_retime_animation\n"
				"Modifier operations (AnimSequence only): list_modifiers, add_modifier, remove_modifier, apply_modifier, revert_modifier\n"
				"Metadata operations: list_metadata, add_metadata, remove_metadata, set_metadata_property\n"
				"Property operations: set_property\n\n"
				"Example workflow:\n"
				"1. open: {\"params\": {\"asset_path\": \"/Game/Animations/MyMontage\"}}\n"
				"2. add_notify: {\"session_id\": \"...\", \"params\": {\"notify_type\": \"skeleton\", \"notify_name\": \"FootStep\", \"time\": 0.5}}\n"
				"3. add_notify: {\"session_id\": \"...\", \"params\": {\"notify_type\": \"AnimNotify_PlaySound\", \"time\": 1.0}, \"suppress_output\": true}\n"
				"4. save: {\"session_id\": \"...\"}\n"
				"5. close: {\"session_id\": \"...\"}\n\n"
				"All mutating operations are wrapped in FScopedTransaction for undo support. "
				"Use 'suppress_output' on intermediate operations to reduce response size.");
}

TSharedPtr<FJsonObject> ClaireonTool_AnimEdit::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// session_id
	TSharedPtr<FJsonObject> SessionIdProp = MakeShared<FJsonObject>();
	SessionIdProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionIdProp->SetStringField(TEXT("description"), TEXT("Session identifier from a previous 'open' operation. Required for all operations except 'open', 'create_montage', 'create_composite', and 'duplicate_asset'."));
	Properties->SetObjectField(TEXT("session_id"), SessionIdProp);

	// operation
	TSharedPtr<FJsonObject> OperationProp = MakeShared<FJsonObject>();
	OperationProp->SetStringField(TEXT("type"), TEXT("string"));
	TArray<TSharedPtr<FJsonValue>> OperationEnum;
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("open")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("create_montage")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("create_composite")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("duplicate_asset")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("close")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("get_state")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("save")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_notify")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_notify")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("move_notify")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("set_notify_property")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("get_notify_property")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("list_notify_properties")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("duplicate_notify")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_notify_track")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_notify_track")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("rename_notify_track")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("reorder_notify_track")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_curve")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_curve")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_curve_key")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_curve_key")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("set_curve_key_property")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_section")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_section")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("set_section_link")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("set_section_link_method")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_segment")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_segment")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("set_segment_property")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_slot")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_slot")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("set_slot_property")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("inspect_segment")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("retime_segment")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("batch_retime_animation")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("list_modifiers")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_modifier")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_modifier")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("apply_modifier")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("revert_modifier")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("list_metadata")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_metadata")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_metadata")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("set_metadata_property")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("set_property")));
	OperationProp->SetArrayField(TEXT("enum"), OperationEnum);
	OperationProp->SetStringField(TEXT("description"), TEXT("The editing operation to perform."));
	Properties->SetObjectField(TEXT("operation"), OperationProp);

	// params
	TSharedPtr<FJsonObject> ParamsProp = MakeShared<FJsonObject>();
	ParamsProp->SetStringField(TEXT("type"), TEXT("object"));
	ParamsProp->SetStringField(TEXT("description"), TEXT("Operation-specific parameters. See operation descriptions for details."));
	Properties->SetObjectField(TEXT("params"), ParamsProp);

	// suppress_output
	TSharedPtr<FJsonObject> SuppressOutputProp = MakeShared<FJsonObject>();
	SuppressOutputProp->SetStringField(TEXT("type"), TEXT("boolean"));
	SuppressOutputProp->SetStringField(TEXT("description"),
		TEXT("When true, returns only a brief status instead of the full animation state. "
			 "Use this for intermediate operations in a batch to reduce response size and speed up execution, "
			 "then omit it on the final operation to get the full state and verify all changes."));
	Properties->SetObjectField(TEXT("suppress_output"), SuppressOutputProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("operation")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

// ============================================================================
// Execute Dispatch
// ============================================================================

FToolResult ClaireonTool_AnimEdit::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Operation;
	if (!Arguments->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: operation"));
	}

	// Params sub-object (optional -- callers may embed fields at top level or under params)
	TSharedPtr<FJsonObject> Params;
	const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
	if (Arguments->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr)
	{
		Params = *ParamsPtr;
	}
	else
	{
		// Treat the entire Arguments object as params for convenience
		Params = Arguments;
	}

	// suppress_output flag
	bool bSuppressOutput = false;
	if (Arguments->HasField(TEXT("suppress_output")))
	{
		bSuppressOutput = Arguments->GetBoolField(TEXT("suppress_output"));
	}

	// Operations that don't need a session
	if (Operation == TEXT("open"))
	{
		return Operation_Open(Params);
	}
	if (Operation == TEXT("create_montage"))
		return Operation_CreateMontage(Params);
	if (Operation == TEXT("create_composite"))
		return Operation_CreateComposite(Params);
	if (Operation == TEXT("duplicate_asset"))
		return Operation_DuplicateAsset(Params);

	// All other operations require a session_id
	FString SessionId;
	if (!Arguments->TryGetStringField(TEXT("session_id"), SessionId) || SessionId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: session_id"));
	}

	FMCPSession* Session = FClaireonSessionManager::Get().FindSession(SessionId);
	if (!Session)
	{
		return MakeErrorResult(FString::Printf(TEXT("Session not found or expired: %s"), *SessionId));
	}
	Session->Touch();

	FAnimEditToolData* Data = ToolData.Find(SessionId);
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session tool data not found or animation asset was unloaded"));
	}

	Data->bSuppressOutput = bSuppressOutput;

	// Dispatch operations
	if (Operation == TEXT("close"))
		return Operation_Close(SessionId, Data, Params);
	if (Operation == TEXT("get_state"))
		return Operation_GetState(SessionId, Data, Params);
	if (Operation == TEXT("save"))
		return Operation_Save(SessionId, Data, Params);

	// Notify ops
	if (Operation == TEXT("add_notify"))
		return Operation_AddNotify(SessionId, Data, Params);
	if (Operation == TEXT("remove_notify"))
		return Operation_RemoveNotify(SessionId, Data, Params);
	if (Operation == TEXT("move_notify"))
		return Operation_MoveNotify(SessionId, Data, Params);
	if (Operation == TEXT("set_notify_property"))
		return Operation_SetNotifyProperty(SessionId, Data, Params);
	if (Operation == TEXT("get_notify_property"))
		return Operation_GetNotifyProperty(SessionId, Data, Params);
	if (Operation == TEXT("list_notify_properties"))
		return Operation_ListNotifyProperties(SessionId, Data, Params);
	if (Operation == TEXT("duplicate_notify"))
		return Operation_DuplicateNotify(SessionId, Data, Params);
	if (Operation == TEXT("add_notify_track"))
		return Operation_AddNotifyTrack(SessionId, Data, Params);
	if (Operation == TEXT("remove_notify_track"))
		return Operation_RemoveNotifyTrack(SessionId, Data, Params);
	if (Operation == TEXT("rename_notify_track"))
		return Operation_RenameNotifyTrack(SessionId, Data, Params);
	if (Operation == TEXT("reorder_notify_track"))
		return Operation_ReorderNotifyTrack(SessionId, Data, Params);

	// Curve ops
	if (Operation == TEXT("add_curve"))
		return Operation_AddCurve(SessionId, Data, Params);
	if (Operation == TEXT("remove_curve"))
		return Operation_RemoveCurve(SessionId, Data, Params);
	if (Operation == TEXT("add_curve_key"))
		return Operation_AddCurveKey(SessionId, Data, Params);
	if (Operation == TEXT("remove_curve_key"))
		return Operation_RemoveCurveKey(SessionId, Data, Params);
	if (Operation == TEXT("set_curve_key_property"))
		return Operation_SetCurveKeyProperty(SessionId, Data, Params);

	// Montage section ops
	if (Operation == TEXT("add_section"))
		return Operation_AddSection(SessionId, Data, Params);
	if (Operation == TEXT("remove_section"))
		return Operation_RemoveSection(SessionId, Data, Params);
	if (Operation == TEXT("set_section_link"))
		return Operation_SetSectionLink(SessionId, Data, Params);
	if (Operation == TEXT("set_section_link_method"))
		return Operation_SetSectionLinkMethod(SessionId, Data, Params);
	if (Operation == TEXT("add_segment"))
		return Operation_AddSegment(SessionId, Data, Params);
	if (Operation == TEXT("remove_segment"))
		return Operation_RemoveSegment(SessionId, Data, Params);
	if (Operation == TEXT("set_segment_property"))
		return Operation_SetSegmentProperty(SessionId, Data, Params);
	if (Operation == TEXT("add_slot"))
		return Operation_AddSlot(SessionId, Data, Params);
	if (Operation == TEXT("remove_slot"))
		return Operation_RemoveSlot(SessionId, Data, Params);
	if (Operation == TEXT("set_slot_property"))
		return Operation_SetSlotProperty(SessionId, Data, Params);
	if (Operation == TEXT("inspect_segment"))
		return Operation_InspectSegment(SessionId, Data, Params);
	if (Operation == TEXT("retime_segment"))
		return Operation_RetimeSegment(SessionId, Data, Params);
	if (Operation == TEXT("batch_retime_animation"))
		return Operation_BatchRetimeAnimation(SessionId, Data, Params);

	// Modifier ops
	if (Operation == TEXT("list_modifiers"))
		return Operation_ListModifiers(SessionId, Data, Params);
	if (Operation == TEXT("add_modifier"))
		return Operation_AddModifier(SessionId, Data, Params);
	if (Operation == TEXT("remove_modifier"))
		return Operation_RemoveModifier(SessionId, Data, Params);
	if (Operation == TEXT("apply_modifier"))
		return Operation_ApplyModifier(SessionId, Data, Params);
	if (Operation == TEXT("revert_modifier"))
		return Operation_RevertModifier(SessionId, Data, Params);

	// Metadata ops
	if (Operation == TEXT("list_metadata"))
		return Operation_ListMetadata(SessionId, Data, Params);
	if (Operation == TEXT("add_metadata"))
		return Operation_AddMetadata(SessionId, Data, Params);
	if (Operation == TEXT("remove_metadata"))
		return Operation_RemoveMetadata(SessionId, Data, Params);
	if (Operation == TEXT("set_metadata_property"))
		return Operation_SetMetadataProperty(SessionId, Data, Params);

	// Property ops
	if (Operation == TEXT("set_property"))
		return Operation_SetProperty(SessionId, Data, Params);

	return MakeErrorResult(FString::Printf(TEXT("Unknown operation: %s"), *Operation));
}

// ============================================================================
// Session Management
// ============================================================================

void ClaireonTool_AnimEdit::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == TEXT("claireon.anim_edit"))
	{
		ToolData.Remove(Info.SessionId);
	}
}

FToolResult ClaireonTool_AnimEdit::BuildStateResponse(const FString& SessionId, FAnimEditToolData* Data)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	// Fast path: skip expensive animation state output when caller doesn't need it
	if (Data->bSuppressOutput)
	{
		const FString StatusMsg = Data->LastOperationStatus.IsEmpty()
			? TEXT("ok")
			: FString::Printf(TEXT("ok: %s"), *Data->LastOperationStatus);
		TSharedPtr<FJsonObject> SuppressData = MakeShared<FJsonObject>();
		SuppressData->SetStringField(TEXT("session_id"), SessionId);
		SuppressData->SetStringField(TEXT("status"), StatusMsg);
		return MakeSuccessResult(SuppressData, StatusMsg);
	}

	// Full response
	FString Output;
	Output += TEXT("=== Session Status ===\n");
	Output += FString::Printf(TEXT("Session: %s\n"), *SessionId);
	Output += FString::Printf(TEXT("Asset: %s (%s)\n"), *Data->Animation->GetPathName(), *Data->AssetType);
	Output += FString::Printf(TEXT("Last Operation: %s\n"), *Data->LastOperationStatus);

	if (Data->FocusedNotifyIndex >= 0)
	{
		Output += FString::Printf(TEXT("Focused Notify: [%d]\n"), Data->FocusedNotifyIndex);
		Output += TEXT("\n");
		Output += ClaireonAnimHelpers::FormatSingleNotify(Data->Animation.Get(), Data->FocusedNotifyIndex);
	}

	Output += TEXT("\n");

	// Show animation structure (compact)
	Output += ClaireonAnimHelpers::FormatAnimStructure(Data->Animation.Get(), Data->AssetType, false);

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("asset_path"), Data->Animation->GetPathName());
	ResponseData->SetStringField(TEXT("asset_type"), Data->AssetType);
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetStringField(TEXT("last_operation"), Data->LastOperationStatus);
	ResponseData->SetStringField(TEXT("state_view"), Output);

	const FString Summary = FString::Printf(TEXT("Session %s: %s"),
		*SessionId.Left(8), *Data->LastOperationStatus);

	return MakeSuccessResult(ResponseData, Summary);
}

// ============================================================================
// Session Operations
// ============================================================================

FToolResult ClaireonTool_AnimEdit::Operation_Open(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	// Resolve path to canonical form
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	FString AssetType, Error;
	UAnimSequenceBase* Anim = ClaireonAnimHelpers::LoadAnimAsset(AssetPath, AssetType, Error);
	if (!Anim)
	{
		return MakeErrorResult(Error);
	}

	// Register delegate if not done yet
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonTool_AnimEdit::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	const FString ResolvedPath = Anim->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedPath, TEXT("claireon.anim_edit"));
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		const FTimespan Elapsed = FDateTime::UtcNow() - Blocker.LastAccessTime;
		return MakeErrorResult(FString::Printf(
			TEXT("Asset is locked by %s session %s (last activity %dm %ds ago). Close that session first."),
			*Blocker.ToolName, *Blocker.SessionId,
			static_cast<int32>(Elapsed.GetTotalMinutes()),
			static_cast<int32>(Elapsed.GetTotalSeconds()) % 60));
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *ResolvedPath));
	}

	const FString SessionId = OpenResult.SessionId;

	// If ReusedExistingSession, still update tool data
	FAnimEditToolData NewData;
	NewData.Animation = Anim;
	NewData.AssetType = AssetType;
	NewData.LastOperationStatus = TEXT("Opened session");
	ToolData.Add(SessionId, MoveTemp(NewData));

	// Return full structure + session ID
	FString StructureText = ClaireonAnimHelpers::FormatAnimStructure(Anim, AssetType, true);

	TSharedPtr<FJsonObject> OpenData = MakeShared<FJsonObject>();
	OpenData->SetStringField(TEXT("session_id"), SessionId);
	OpenData->SetStringField(TEXT("asset_path"), ResolvedPath);
	OpenData->SetStringField(TEXT("asset_type"), AssetType);
	OpenData->SetStringField(TEXT("status"), TEXT("Opened session"));
	OpenData->SetStringField(TEXT("structure"), StructureText);

	return MakeSuccessResult(OpenData, FString::Printf(TEXT("Opened session for %s (%s)"), *FPaths::GetBaseFilename(ResolvedPath), *AssetType));
}

FToolResult ClaireonTool_AnimEdit::Operation_Close(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	bool bSaveFirst = false;
	Params->TryGetBoolField(TEXT("save_first"), bSaveFirst);

	if (bSaveFirst)
	{
		Operation_Save(SessionId, Data, MakeShared<FJsonObject>());
	}

	FClaireonSessionManager::Get().CloseSession(SessionId);
	ToolData.Remove(SessionId);

	TSharedPtr<FJsonObject> CloseData = MakeShared<FJsonObject>();
	CloseData->SetStringField(TEXT("session_id"), SessionId);
	CloseData->SetStringField(TEXT("status"), TEXT("closed"));
	return MakeSuccessResult(CloseData, FString::Printf(TEXT("Session %s closed"), *SessionId));
}

FToolResult ClaireonTool_AnimEdit::Operation_GetState(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	// Optional: focus on a specific section
	FString FocusSection;
	Params->TryGetStringField(TEXT("focus"), FocusSection);

	// Optional: set focused notify index
	double NotifyIndexD = -1.0;
	if (Params->TryGetNumberField(TEXT("notify_index"), NotifyIndexD))
	{
		Data->FocusedNotifyIndex = static_cast<int32>(NotifyIndexD);
	}

	if (!FocusSection.IsEmpty())
	{
		// Return a focused section view instead of full state
		FString SectionOutput = ClaireonAnimHelpers::FormatAnimStructure(Data->Animation.Get(), Data->AssetType, true, FocusSection);

		TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
		ResponseData->SetStringField(TEXT("session_id"), SessionId);
		ResponseData->SetStringField(TEXT("asset_path"), Data->Animation->GetPathName());
		ResponseData->SetStringField(TEXT("asset_type"), Data->AssetType);
		ResponseData->SetStringField(TEXT("focus"), FocusSection);
		ResponseData->SetStringField(TEXT("state_view"), SectionOutput);
		return MakeSuccessResult(ResponseData, FString::Printf(TEXT("Session %s: focused on %s"), *SessionId.Left(8), *FocusSection));
	}

	Data->LastOperationStatus = TEXT("get_state");
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_Save(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	UAnimSequenceBase* Anim = Data->Animation.Get();
	UPackage* Package = Anim->GetOutermost();
	Package->SetDirtyFlag(true);

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_Save", "MCP: Save Animation"));

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false);

	if (bSaved)
	{
		Data->LastOperationStatus = FString::Printf(TEXT("save -> Saved %s"), *Anim->GetPathName());
		return BuildStateResponse(SessionId, Data);
	}
	else
	{
		Data->LastOperationStatus = TEXT("save -> Failed");
		return MakeErrorResult(TEXT("Failed to save animation package"));
	}
}

// ============================================================================
// Notify Operations
// ============================================================================

FToolResult ClaireonTool_AnimEdit::Operation_AddNotify(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString NotifyType;
	if (!Params->TryGetStringField(TEXT("notify_type"), NotifyType) || NotifyType.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: notify_type"));
	}

	double Time = 0.0;
	if (!Params->TryGetNumberField(TEXT("time"), Time))
	{
		return MakeErrorResult(TEXT("Missing required parameter: time"));
	}

	double Duration = 0.0;
	Params->TryGetNumberField(TEXT("duration"), Duration);

	// Support end_time as alternative to duration (end_time takes effect only when duration is not provided)
	double EndTimeD = -1.0;
	Params->TryGetNumberField(TEXT("end_time"), EndTimeD);
	if (EndTimeD >= 0.0 && Duration <= 0.0)
	{
		Duration = EndTimeD - Time;
	}

	double TrackIndexD = 0.0;
	Params->TryGetNumberField(TEXT("track_index"), TrackIndexD);
	int32 TrackIndex = static_cast<int32>(TrackIndexD);

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_AddNotify", "MCP: Add Notify"));
	Data->Animation->Modify();

	FString Error;
	int32 NewIndex = -1;

	if (NotifyType == TEXT("skeleton"))
	{
		FString NotifyName;
		if (!Params->TryGetStringField(TEXT("notify_name"), NotifyName) || NotifyName.IsEmpty())
		{
			return MakeErrorResult(TEXT("Missing required parameter: notify_name (required for skeleton notifies)"));
		}

		NewIndex = ClaireonAnimHelpers::AddSkeletonNotify(Data->Animation.Get(), NotifyName, static_cast<float>(Time), TrackIndex, Error);
	}
	else
	{
		// Determine if it's a state notify (duration-based)
		bool bIsState = NotifyType.Contains(TEXT("State")) || NotifyType.StartsWith(TEXT("FSANS_")) || NotifyType.StartsWith(TEXT("ANS_"));
		UClass* NotifyClass = ClaireonAnimHelpers::ResolveNotifyClass(NotifyType, bIsState, Error);
		if (!NotifyClass)
		{
			return MakeErrorResult(Error);
		}

		NewIndex = ClaireonAnimHelpers::AddClassNotify(Data->Animation.Get(), NotifyClass, static_cast<float>(Time), static_cast<float>(Duration), TrackIndex, Error);
	}

	if (NewIndex < 0)
	{
		return MakeErrorResult(Error);
	}

	// Set initial properties if provided
	const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsPtr) && PropsPtr)
	{
		for (const auto& Pair : (*PropsPtr)->Values)
		{
			FString PropValue;
			if (Pair.Value->TryGetString(PropValue))
			{
				FString PropError;
				if (!ClaireonAnimHelpers::SetNotifyProperty(Data->Animation.Get(), NewIndex, Pair.Key, PropValue, PropError))
				{
					UE_LOG(LogClaireon, Warning, TEXT("add_notify: Failed to set property '%s': %s"), *Pair.Key, *PropError);
				}
			}
		}
	}

	Data->FocusedNotifyIndex = NewIndex;
	Data->LastOperationStatus = FString::Printf(TEXT("add_notify -> Added %s at %.2fs [%d]"), *NotifyType, Time, NewIndex);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_RemoveNotify(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	double NotifyIndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("notify_index"), NotifyIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: notify_index"));
	}
	int32 NotifyIndex = static_cast<int32>(NotifyIndexD);

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_RemoveNotify", "MCP: Remove Notify"));
	Data->Animation->Modify();

	FString Error;
	if (!ClaireonAnimHelpers::RemoveNotify(Data->Animation.Get(), NotifyIndex, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->FocusedNotifyIndex = -1;
	Data->LastOperationStatus = FString::Printf(TEXT("remove_notify -> Removed notify [%d]"), NotifyIndex);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_MoveNotify(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	double NotifyIndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("notify_index"), NotifyIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: notify_index"));
	}
	int32 NotifyIndex = static_cast<int32>(NotifyIndexD);

	// All optional -- pass -1.0f for unchanged float values, -1 for unchanged int
	double TimeD = -1.0;
	Params->TryGetNumberField(TEXT("time"), TimeD);
	float NewTime = static_cast<float>(TimeD);

	double DurationD = -1.0;
	Params->TryGetNumberField(TEXT("duration"), DurationD);

	// Support end_time as alternative to duration (end_time takes effect only when duration is not provided)
	double EndTimeD = -1.0;
	Params->TryGetNumberField(TEXT("end_time"), EndTimeD);
	if (EndTimeD >= 0.0 && DurationD < 0.0)
	{
		if (NotifyIndex < 0 || NotifyIndex >= Data->Animation->Notifies.Num())
		{
			return MakeErrorResult(FString::Printf(TEXT("Notify index %d out of range (asset has %d notifies)"), NotifyIndex, Data->Animation->Notifies.Num()));
		}
		float StartTime = (TimeD >= 0.0) ? static_cast<float>(TimeD) : Data->Animation->Notifies[NotifyIndex].GetTime();
		DurationD = EndTimeD - static_cast<double>(StartTime);
	}

	float NewDuration = static_cast<float>(DurationD);

	double TrackIndexD = -1.0;
	Params->TryGetNumberField(TEXT("track_index"), TrackIndexD);
	int32 NewTrackIndex = static_cast<int32>(TrackIndexD);

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_MoveNotify", "MCP: Move Notify"));
	Data->Animation->Modify();

	FString Error;
	if (!ClaireonAnimHelpers::MoveNotify(Data->Animation.Get(), NotifyIndex, NewTime, NewDuration, NewTrackIndex, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->FocusedNotifyIndex = NotifyIndex;
	Data->LastOperationStatus = FString::Printf(TEXT("move_notify -> Moved notify [%d]"), NotifyIndex);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_SetNotifyProperty(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	double NotifyIndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("notify_index"), NotifyIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: notify_index"));
	}
	int32 NotifyIndex = static_cast<int32>(NotifyIndexD);

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_name"));
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_SetNotifyProp", "MCP: Set Notify Property"));
	Data->Animation->Modify();

	FString Error;
	if (!ClaireonAnimHelpers::SetNotifyProperty(Data->Animation.Get(), NotifyIndex, PropertyName, Value, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->FocusedNotifyIndex = NotifyIndex;
	Data->LastOperationStatus = FString::Printf(TEXT("set_notify_property -> [%d].%s = %s"), NotifyIndex, *PropertyName, *Value);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_GetNotifyProperty(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	double NotifyIndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("notify_index"), NotifyIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: notify_index"));
	}
	int32 NotifyIndex = static_cast<int32>(NotifyIndexD);

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_name"));
	}

	FString Error;
	FString Value = ClaireonAnimHelpers::GetNotifyProperty(Data->Animation.Get(), NotifyIndex, PropertyName, Error);
	if (!Error.IsEmpty())
	{
		return MakeErrorResult(Error);
	}

	Data->FocusedNotifyIndex = NotifyIndex;
	Data->LastOperationStatus = FString::Printf(TEXT("get_notify_property -> [%d].%s = %s"), NotifyIndex, *PropertyName, *Value);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("session_id"), SessionId);
	Result->SetStringField(TEXT("property_name"), PropertyName);
	Result->SetStringField(TEXT("value"), Value);
	Result->SetNumberField(TEXT("notify_index"), NotifyIndex);
	Result->SetStringField(TEXT("status"), Data->LastOperationStatus);
	return MakeSuccessResult(Result, Data->LastOperationStatus);
}

FToolResult ClaireonTool_AnimEdit::Operation_ListNotifyProperties(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	double NotifyIndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("notify_index"), NotifyIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: notify_index"));
	}
	int32 NotifyIndex = static_cast<int32>(NotifyIndexD);

	if (NotifyIndex < 0 || NotifyIndex >= Data->Animation->Notifies.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Notify index %d out of range (asset has %d notifies)"), NotifyIndex, Data->Animation->Notifies.Num()));
	}

	const FAnimNotifyEvent& Event = Data->Animation->Notifies[NotifyIndex];
	UObject* SubObject = Event.Notify ? static_cast<UObject*>(Event.Notify) : static_cast<UObject*>(Event.NotifyStateClass);
	if (!SubObject)
	{
		return MakeErrorResult(FString::Printf(TEXT("Notify at index %d is a skeleton notify (no sub-object). Cannot list properties on skeleton notifies."), NotifyIndex));
	}

	FString Filter;
	Params->TryGetStringField(TEXT("filter"), Filter);

	TSharedPtr<FJsonObject> Properties = ClaireonPropertyUtils::GetAllProperties(SubObject, Filter);

	Data->FocusedNotifyIndex = NotifyIndex;
	Data->LastOperationStatus = FString::Printf(TEXT("list_notify_properties -> [%d] %s (%d properties)"), NotifyIndex, *SubObject->GetClass()->GetName(), Properties ? Properties->Values.Num() : 0);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("session_id"), SessionId);
	Result->SetNumberField(TEXT("notify_index"), NotifyIndex);
	Result->SetStringField(TEXT("class"), SubObject->GetClass()->GetName());
	Result->SetObjectField(TEXT("properties"), Properties ? Properties : MakeShared<FJsonObject>());
	Result->SetStringField(TEXT("status"), Data->LastOperationStatus);
	return MakeSuccessResult(Result, Data->LastOperationStatus);
}

FToolResult ClaireonTool_AnimEdit::Operation_DuplicateNotify(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	double NotifyIndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("notify_index"), NotifyIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: notify_index"));
	}
	int32 SourceIndex = static_cast<int32>(NotifyIndexD);

	if (SourceIndex < 0 || SourceIndex >= Data->Animation->Notifies.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Notify index %d out of range (asset has %d notifies)"), SourceIndex, Data->Animation->Notifies.Num()));
	}

	const FAnimNotifyEvent& SourceEvent = Data->Animation->Notifies[SourceIndex];

	// Read optional overrides
	double TimeD = -1.0;
	Params->TryGetNumberField(TEXT("time"), TimeD);
	float NewTime = (TimeD >= 0.0) ? static_cast<float>(TimeD) : SourceEvent.GetTime();

	double TrackIndexD = -1.0;
	Params->TryGetNumberField(TEXT("track_index"), TrackIndexD);
	int32 NewTrackIndex = (TrackIndexD >= 0.0) ? static_cast<int32>(TrackIndexD) : SourceEvent.TrackIndex;

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_DuplicateNotify", "MCP: Duplicate Notify"));
	Data->Animation->Modify();

	// Ensure target track exists
	while (Data->Animation->AnimNotifyTracks.Num() <= NewTrackIndex)
	{
		FAnimNotifyTrack NewTrack;
		NewTrack.TrackName = FName(*FString::Printf(TEXT("%d"), Data->Animation->AnimNotifyTracks.Num()));
		Data->Animation->AnimNotifyTracks.Add(NewTrack);
	}

	// Create a copy of the event
	FAnimNotifyEvent NewEvent;
	NewEvent.TrackIndex = NewTrackIndex;
	NewEvent.Guid = FGuid::NewGuid();
	NewEvent.NotifyName = SourceEvent.NotifyName;
	NewEvent.SetTime(NewTime);
	NewEvent.EndTriggerTimeOffset = SourceEvent.EndTriggerTimeOffset;

	if (SourceEvent.NotifyStateClass)
	{
		// Duplicate state sub-object
		UAnimNotifyState* NewState = DuplicateObject<UAnimNotifyState>(SourceEvent.NotifyStateClass, Data->Animation.Get());
		NewEvent.NotifyStateClass = NewState;
		NewEvent.Notify = nullptr;
		NewEvent.SetDuration(SourceEvent.Duration);
	}
	else if (SourceEvent.Notify)
	{
		// Duplicate instant notify sub-object
		UAnimNotify* NewNotify = DuplicateObject<UAnimNotify>(SourceEvent.Notify, Data->Animation.Get());
		NewEvent.Notify = NewNotify;
		NewEvent.NotifyStateClass = nullptr;
	}
	// else: skeleton notify — no sub-object to duplicate

	int32 NewIndex = Data->Animation->Notifies.Add(NewEvent);
	Data->Animation->RefreshCacheData();
	Data->Animation->MarkPackageDirty();

	Data->FocusedNotifyIndex = NewIndex;
	Data->LastOperationStatus = FString::Printf(TEXT("duplicate_notify -> Duplicated [%d] to [%d] at %.2fs [Track %d]"), SourceIndex, NewIndex, NewTime, NewTrackIndex);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_AddNotifyTrack(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString TrackName;
	if (!Params->TryGetStringField(TEXT("track_name"), TrackName))
	{
		// Auto-name based on current count
		TrackName = FString::Printf(TEXT("%d"), Data->Animation->AnimNotifyTracks.Num());
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_AddNotifyTrack", "MCP: Add Notify Track"));
	Data->Animation->Modify();

	FAnimNotifyTrack NewTrack;
	NewTrack.TrackName = FName(*TrackName);
	Data->Animation->AnimNotifyTracks.Add(NewTrack);

	int32 NewTrackIndex = Data->Animation->AnimNotifyTracks.Num() - 1;
	Data->LastOperationStatus = FString::Printf(TEXT("add_notify_track -> Added track '%s' [%d]"), *TrackName, NewTrackIndex);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_RemoveNotifyTrack(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	double TrackIndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("track_index"), TrackIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: track_index"));
	}
	int32 TrackIndex = static_cast<int32>(TrackIndexD);

	if (TrackIndex < 0 || TrackIndex >= Data->Animation->AnimNotifyTracks.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Track index %d out of range [0, %d)"), TrackIndex, Data->Animation->AnimNotifyTracks.Num()));
	}

	// Don't allow removing the last track
	if (Data->Animation->AnimNotifyTracks.Num() <= 1)
	{
		return MakeErrorResult(TEXT("Cannot remove the last notify track"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_RemoveNotifyTrack", "MCP: Remove Notify Track"));
	Data->Animation->Modify();

	FString RemovedName = Data->Animation->AnimNotifyTracks[TrackIndex].TrackName.ToString();

	// Reassign notifies on this track to track 0
	for (FAnimNotifyEvent& Notify : Data->Animation->Notifies)
	{
		if (Notify.TrackIndex == TrackIndex)
		{
			Notify.TrackIndex = 0;
		}
		else if (Notify.TrackIndex > TrackIndex)
		{
			// Shift down indices above the removed track
			Notify.TrackIndex--;
		}
	}

	Data->Animation->AnimNotifyTracks.RemoveAt(TrackIndex);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_notify_track -> Removed track '%s' [%d], notifies reassigned"), *RemovedName, TrackIndex);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_RenameNotifyTrack(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	double TrackIndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("track_index"), TrackIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: track_index"));
	}
	int32 TrackIndex = static_cast<int32>(TrackIndexD);

	FString NewName;
	if (!Params->TryGetStringField(TEXT("track_name"), NewName))
	{
		return MakeErrorResult(TEXT("Missing required parameter: track_name"));
	}

	if (TrackIndex < 0 || TrackIndex >= Data->Animation->AnimNotifyTracks.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Track index %d out of range [0, %d)"), TrackIndex, Data->Animation->AnimNotifyTracks.Num()));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_RenameNotifyTrack", "MCP: Rename Notify Track"));
	Data->Animation->Modify();

	FString OldName = Data->Animation->AnimNotifyTracks[TrackIndex].TrackName.ToString();
	Data->Animation->AnimNotifyTracks[TrackIndex].TrackName = FName(*NewName);

	Data->LastOperationStatus = FString::Printf(TEXT("rename_notify_track -> Renamed track [%d] '%s' -> '%s'"), TrackIndex, *OldName, *NewName);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_ReorderNotifyTrack(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	double TrackIndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("track_index"), TrackIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: track_index"));
	}
	int32 TrackIndex = static_cast<int32>(TrackIndexD);

	double NewIndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("new_index"), NewIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: new_index"));
	}
	int32 NewIndex = static_cast<int32>(NewIndexD);

	const int32 TrackCount = Data->Animation->AnimNotifyTracks.Num();

	if (TrackIndex < 0 || TrackIndex >= TrackCount)
	{
		return MakeErrorResult(FString::Printf(TEXT("Track index %d out of range [0, %d)"), TrackIndex, TrackCount));
	}
	if (NewIndex < 0 || NewIndex >= TrackCount)
	{
		return MakeErrorResult(FString::Printf(TEXT("New index %d out of range [0, %d)"), NewIndex, TrackCount));
	}
	if (TrackIndex == NewIndex)
	{
		Data->LastOperationStatus = FString::Printf(TEXT("reorder_notify_track -> Track [%d] already at target index"), TrackIndex);
		return BuildStateResponse(SessionId, Data);
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_ReorderNotifyTrack", "MCP: Reorder Notify Track"));
	Data->Animation->Modify();

	FString TrackName = Data->Animation->AnimNotifyTracks[TrackIndex].TrackName.ToString();

	// Remap all notify TrackIndex references
	for (FAnimNotifyEvent& Notify : Data->Animation->Notifies)
	{
		if (Notify.TrackIndex == TrackIndex)
		{
			// The track being moved — assign new index
			Notify.TrackIndex = NewIndex;
		}
		else if (TrackIndex < NewIndex)
		{
			// Moving track forward: tracks in (TrackIndex, NewIndex] shift down by 1
			if (Notify.TrackIndex > TrackIndex && Notify.TrackIndex <= NewIndex)
			{
				Notify.TrackIndex--;
			}
		}
		else
		{
			// Moving track backward: tracks in [NewIndex, TrackIndex) shift up by 1
			if (Notify.TrackIndex >= NewIndex && Notify.TrackIndex < TrackIndex)
			{
				Notify.TrackIndex++;
			}
		}
	}

	// Move the track in the array
	FAnimNotifyTrack MovedTrack = Data->Animation->AnimNotifyTracks[TrackIndex];
	Data->Animation->AnimNotifyTracks.RemoveAt(TrackIndex);
	Data->Animation->AnimNotifyTracks.Insert(MovedTrack, NewIndex);

	Data->LastOperationStatus = FString::Printf(TEXT("reorder_notify_track -> Moved track '%s' from [%d] to [%d]"), *TrackName, TrackIndex, NewIndex);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Curve Operations
// ============================================================================

FToolResult ClaireonTool_AnimEdit::Operation_AddCurve(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString CurveName;
	if (!Params->TryGetStringField(TEXT("curve_name"), CurveName) || CurveName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: curve_name"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_AddCurve", "MCP: Add Curve"));
	Data->Animation->Modify();

	FString Error;
	if (!ClaireonAnimHelpers::AddCurve(Data->Animation.Get(), CurveName, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("add_curve -> Added curve '%s'"), *CurveName);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_RemoveCurve(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString CurveName;
	if (!Params->TryGetStringField(TEXT("curve_name"), CurveName) || CurveName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: curve_name"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_RemoveCurve", "MCP: Remove Curve"));
	Data->Animation->Modify();

	FString Error;
	if (!ClaireonAnimHelpers::RemoveCurve(Data->Animation.Get(), CurveName, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("remove_curve -> Removed curve '%s'"), *CurveName);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_AddCurveKey(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString CurveName;
	if (!Params->TryGetStringField(TEXT("curve_name"), CurveName) || CurveName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: curve_name"));
	}

	double Time = -1.0;
	Params->TryGetNumberField(TEXT("time"), Time);

	// Support frame as alternative to time
	double Frame = -1.0;
	Params->TryGetNumberField(TEXT("frame"), Frame);
	if (Frame >= 0.0 && Time < 0.0)
	{
		const IAnimationDataModel* Model = Data->Animation->GetDataModel();
		double Fps = Model ? Model->GetFrameRate().AsDecimal() : 30.0;
		Time = Frame / Fps;
	}
	if (Time < 0.0)
	{
		return MakeErrorResult(TEXT("Missing required parameter: time or frame"));
	}

	double Value = 0.0;
	if (!Params->TryGetNumberField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	// Build FRichCurveKey with optional tangent params
	FRichCurveKey NewKey(static_cast<float>(Time), static_cast<float>(Value));

	FString InterpModeStr;
	if (Params->TryGetStringField(TEXT("interp_mode"), InterpModeStr))
	{
		FString Lower = InterpModeStr.ToLower();
		if (Lower == TEXT("linear"))        NewKey.InterpMode = RCIM_Linear;
		else if (Lower == TEXT("cubic"))    NewKey.InterpMode = RCIM_Cubic;
		else if (Lower == TEXT("constant")) NewKey.InterpMode = RCIM_Constant;
		else return MakeErrorResult(FString::Printf(TEXT("Invalid interp_mode '%s'. Expected: linear, cubic, constant"), *InterpModeStr));
	}

	FString TangentModeStr;
	if (Params->TryGetStringField(TEXT("tangent_mode"), TangentModeStr))
	{
		FString Lower = TangentModeStr.ToLower();
		if (Lower == TEXT("auto"))       NewKey.TangentMode = RCTM_Auto;
		else if (Lower == TEXT("user"))  NewKey.TangentMode = RCTM_User;
		else if (Lower == TEXT("break")) NewKey.TangentMode = RCTM_Break;
		else return MakeErrorResult(FString::Printf(TEXT("Invalid tangent_mode '%s'. Expected: auto, user, break"), *TangentModeStr));
	}

	double ArriveTangent = 0.0;
	if (Params->TryGetNumberField(TEXT("arrive_tangent"), ArriveTangent))
	{
		NewKey.ArriveTangent = static_cast<float>(ArriveTangent);
	}

	double LeaveTangent = 0.0;
	if (Params->TryGetNumberField(TEXT("leave_tangent"), LeaveTangent))
	{
		NewKey.LeaveTangent = static_cast<float>(LeaveTangent);
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_AddCurveKey", "MCP: Add Curve Key"));
	Data->Animation->Modify();

	FString Error;
	if (!ClaireonAnimHelpers::AddCurveKey(Data->Animation.Get(), CurveName, NewKey, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("add_curve_key -> '%s' key at %.3fs = %.3f"), *CurveName, Time, Value);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_RemoveCurveKey(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString CurveName;
	if (!Params->TryGetStringField(TEXT("curve_name"), CurveName) || CurveName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: curve_name"));
	}

	double Time = -1.0;
	Params->TryGetNumberField(TEXT("time"), Time);

	// Support frame as alternative to time
	double Frame = -1.0;
	Params->TryGetNumberField(TEXT("frame"), Frame);
	if (Frame >= 0.0 && Time < 0.0)
	{
		const IAnimationDataModel* Model = Data->Animation->GetDataModel();
		double Fps = Model ? Model->GetFrameRate().AsDecimal() : 30.0;
		Time = Frame / Fps;
	}
	if (Time < 0.0)
	{
		return MakeErrorResult(TEXT("Missing required parameter: time or frame"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_RemoveCurveKey", "MCP: Remove Curve Key"));
	Data->Animation->Modify();

	FString Error;
	if (!ClaireonAnimHelpers::RemoveCurveKey(Data->Animation.Get(), CurveName, static_cast<float>(Time), Error))
	{
		return MakeErrorResult(Error);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("remove_curve_key -> '%s' key at %.3fs removed"), *CurveName, Time);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_SetCurveKeyProperty(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString CurveName;
	if (!Params->TryGetStringField(TEXT("curve_name"), CurveName) || CurveName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: curve_name"));
	}

	double Time = -1.0;
	Params->TryGetNumberField(TEXT("time"), Time);

	// Support frame as alternative to time
	double Frame = -1.0;
	Params->TryGetNumberField(TEXT("frame"), Frame);
	if (Frame >= 0.0 && Time < 0.0)
	{
		const IAnimationDataModel* Model = Data->Animation->GetDataModel();
		double Fps = Model ? Model->GetFrameRate().AsDecimal() : 30.0;
		Time = Frame / Fps;
	}
	if (Time < 0.0)
	{
		return MakeErrorResult(TEXT("Missing required parameter: time or frame"));
	}

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_name"));
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value) || Value.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_SetCurveKeyProp", "MCP: Set Curve Key Property"));
	Data->Animation->Modify();

	FString Error;
	if (!ClaireonAnimHelpers::SetCurveKeyProperty(Data->Animation.Get(), CurveName, static_cast<float>(Time), PropertyName, Value, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("set_curve_key_property -> '%s' key at %.3fs: %s = %s"), *CurveName, Time, *PropertyName, *Value);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Montage Section Operations
// ============================================================================

FToolResult ClaireonTool_AnimEdit::Operation_AddSection(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (Data->AssetType != TEXT("AnimMontage"))
	{
		return MakeErrorResult(TEXT("add_section is only valid for AnimMontage assets"));
	}

	UAnimMontage* Montage = Cast<UAnimMontage>(Data->Animation.Get());
	if (!Montage)
	{
		return MakeErrorResult(TEXT("Failed to cast to AnimMontage"));
	}

	FString SectionName;
	if (!Params->TryGetStringField(TEXT("section_name"), SectionName) || SectionName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: section_name"));
	}

	double StartTime = 0.0;
	if (!Params->TryGetNumberField(TEXT("start_time"), StartTime))
	{
		return MakeErrorResult(TEXT("Missing required parameter: start_time"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_AddSection", "MCP: Add Montage Section"));
	Data->Animation->Modify();

	FString Error;
	if (!ClaireonAnimHelpers::AddMontageSection(Montage, SectionName, static_cast<float>(StartTime), Error))
	{
		return MakeErrorResult(Error);
	}

	Montage->PostEditChange();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("add_section -> Added section '%s' at %.2fs"), *SectionName, StartTime);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_RemoveSection(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (Data->AssetType != TEXT("AnimMontage"))
	{
		return MakeErrorResult(TEXT("remove_section is only valid for AnimMontage assets"));
	}

	UAnimMontage* Montage = Cast<UAnimMontage>(Data->Animation.Get());
	if (!Montage)
	{
		return MakeErrorResult(TEXT("Failed to cast to AnimMontage"));
	}

	FString SectionName;
	if (!Params->TryGetStringField(TEXT("section_name"), SectionName) || SectionName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: section_name"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_RemoveSection", "MCP: Remove Montage Section"));
	Data->Animation->Modify();

	FString Error;
	if (!ClaireonAnimHelpers::RemoveMontageSection(Montage, SectionName, Error))
	{
		return MakeErrorResult(Error);
	}

	Montage->PostEditChange();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_section -> Removed section '%s'"), *SectionName);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_SetSectionLink(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (Data->AssetType != TEXT("AnimMontage"))
	{
		return MakeErrorResult(TEXT("set_section_link is only valid for AnimMontage assets"));
	}

	UAnimMontage* Montage = Cast<UAnimMontage>(Data->Animation.Get());
	if (!Montage)
	{
		return MakeErrorResult(TEXT("Failed to cast to AnimMontage"));
	}

	FString SectionName;
	if (!Params->TryGetStringField(TEXT("section_name"), SectionName) || SectionName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: section_name"));
	}

	FString NextSectionName;
	if (!Params->TryGetStringField(TEXT("next_section_name"), NextSectionName))
	{
		return MakeErrorResult(TEXT("Missing required parameter: next_section_name"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_SetSectionLink", "MCP: Set Montage Section Link"));
	Data->Animation->Modify();

	FString Error;
	if (!ClaireonAnimHelpers::SetMontageSectionLink(Montage, SectionName, NextSectionName, Error))
	{
		return MakeErrorResult(Error);
	}

	Montage->PostEditChange();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("set_section_link -> %s -> %s"), *SectionName, *NextSectionName);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_SetSectionLinkMethod(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (Data->AssetType != TEXT("AnimMontage"))
	{
		return MakeErrorResult(TEXT("set_section_link_method is only valid for AnimMontage assets"));
	}

	UAnimMontage* Montage = Cast<UAnimMontage>(Data->Animation.Get());
	if (!Montage)
	{
		return MakeErrorResult(TEXT("Failed to cast to AnimMontage"));
	}

	FString SectionName;
	if (!Params->TryGetStringField(TEXT("section_name"), SectionName) || SectionName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: section_name"));
	}

	FString MethodStr;
	if (!Params->TryGetStringField(TEXT("link_method"), MethodStr) || MethodStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: link_method (absolute, relative, proportional)"));
	}

	EAnimLinkMethod::Type DesiredMethod = EAnimLinkMethod::Absolute;
	FString MethodLower = MethodStr.ToLower();
	if (MethodLower == TEXT("proportional")) DesiredMethod = EAnimLinkMethod::Proportional;
	else if (MethodLower == TEXT("relative")) DesiredMethod = EAnimLinkMethod::Relative;
	else if (MethodLower == TEXT("absolute")) DesiredMethod = EAnimLinkMethod::Absolute;
	else return MakeErrorResult(FString::Printf(TEXT("Invalid link_method '%s'. Expected: absolute, relative, proportional"), *MethodStr));

	int32 SectionIndex = Montage->GetSectionIndex(FName(*SectionName));
	if (SectionIndex == INDEX_NONE)
	{
		return MakeErrorResult(FString::Printf(TEXT("Section '%s' not found"), *SectionName));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_SetSectionLinkMethod", "MCP: Set Section Link Method"));
	Montage->Modify();

	Montage->CompositeSections[SectionIndex].ChangeLinkMethod(DesiredMethod);

	Montage->PostEditChange();
	Montage->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("set_section_link_method -> %s = %s"), *SectionName, *MethodStr);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Montage Slot/Segment Operations
// ============================================================================

// Helper: get a montage and validate, returns nullptr on error (error result set)
static UAnimMontage* GetMontageOrError(FAnimEditToolData* Data, const char* OpName, FString& OutError)
{
	if (Data->AssetType != TEXT("AnimMontage"))
	{
		OutError = FString::Printf(TEXT("%s is only valid for AnimMontage assets"), UTF8_TO_TCHAR(OpName));
		return nullptr;
	}
	UAnimMontage* Montage = Cast<UAnimMontage>(Data->Animation.Get());
	if (!Montage)
	{
		OutError = TEXT("Failed to cast to AnimMontage");
	}
	return Montage;
}

FToolResult ClaireonTool_AnimEdit::Operation_AddSegment(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UAnimMontage* Montage = GetMontageOrError(Data, "add_segment", Error);
	if (!Montage) return MakeErrorResult(Error);

	// Which slot to add to (default: 0)
	double SlotIndexD = 0.0;
	Params->TryGetNumberField(TEXT("slot_index"), SlotIndexD);
	int32 SlotIndex = static_cast<int32>(SlotIndexD);

	if (SlotIndex < 0 || SlotIndex >= Montage->SlotAnimTracks.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Slot index %d out of range [0, %d)"), SlotIndex, Montage->SlotAnimTracks.Num()));
	}

	// Animation asset path (required)
	FString AnimPath;
	if (!Params->TryGetStringField(TEXT("anim_path"), AnimPath) || AnimPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: anim_path"));
	}

	UAnimSequenceBase* AnimAsset = LoadObject<UAnimSequenceBase>(nullptr, *AnimPath);
	if (!AnimAsset)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load animation: %s"), *AnimPath));
	}

	FAnimTrack& Track = Montage->SlotAnimTracks[SlotIndex].AnimTrack;

	// Validate
	FText ValidationReason;
	if (!Track.IsValidToAdd(AnimAsset, &ValidationReason))
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot add animation to slot: %s"), *ValidationReason.ToString()));
	}

	// Start position: default to end of current track
	double StartPosD = -1.0;
	Params->TryGetNumberField(TEXT("start_pos"), StartPosD);
	float StartPos = (StartPosD >= 0.0) ? static_cast<float>(StartPosD) : Track.GetLength();

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_AddSegment", "MCP: Add Montage Segment"));
	Montage->Modify();

	FAnimSegment NewSeg;
	NewSeg.SetAnimReference(AnimAsset, true); // true = initialize AnimStartTime/AnimEndTime from asset
	NewSeg.StartPos = StartPos;

	// Optional overrides
	double PlayRate = 1.0;
	if (Params->TryGetNumberField(TEXT("play_rate"), PlayRate))
	{
		NewSeg.AnimPlayRate = static_cast<float>(PlayRate);
	}

	double AnimStartTime = -1.0;
	if (Params->TryGetNumberField(TEXT("anim_start_time"), AnimStartTime))
	{
		NewSeg.AnimStartTime = static_cast<float>(AnimStartTime);
	}

	double AnimEndTime = -1.0;
	if (Params->TryGetNumberField(TEXT("anim_end_time"), AnimEndTime))
	{
		NewSeg.AnimEndTime = static_cast<float>(AnimEndTime);
	}

	Track.AnimSegments.Add(NewSeg);
	Track.CollapseAnimSegments();

	// Find the segment we just added and read back its exact StartPos after collapse
	int32 NewIndex = Track.AnimSegments.Num() - 1;
	float ActualStartPos = Track.AnimSegments[NewIndex].StartPos;

	// Optionally create a section at the exact segment start position
	FString SectionName;
	if (Params->TryGetStringField(TEXT("section_name"), SectionName) && !SectionName.IsEmpty())
	{
		Montage->AddAnimCompositeSection(FName(*SectionName), ActualStartPos);
	}

	// Re-link all sections/notifies to updated segment layout (matches editor's SortAndUpdateMontage flow)
	Montage->UpdateLinkableElements();
	Montage->PostEditChange();
	Montage->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("add_segment -> Added '%s' to slot [%d] at %.6fs [%d]%s"),
		*AnimAsset->GetName(), SlotIndex, ActualStartPos, NewIndex,
		SectionName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (section '%s')"), *SectionName));
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_RemoveSegment(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UAnimMontage* Montage = GetMontageOrError(Data, "remove_segment", Error);
	if (!Montage) return MakeErrorResult(Error);

	double SlotIndexD = 0.0;
	Params->TryGetNumberField(TEXT("slot_index"), SlotIndexD);
	int32 SlotIndex = static_cast<int32>(SlotIndexD);

	if (SlotIndex < 0 || SlotIndex >= Montage->SlotAnimTracks.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Slot index %d out of range [0, %d)"), SlotIndex, Montage->SlotAnimTracks.Num()));
	}

	double SegIndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("segment_index"), SegIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: segment_index"));
	}
	int32 SegIndex = static_cast<int32>(SegIndexD);

	FAnimTrack& Track = Montage->SlotAnimTracks[SlotIndex].AnimTrack;
	if (SegIndex < 0 || SegIndex >= Track.AnimSegments.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Segment index %d out of range [0, %d)"), SegIndex, Track.AnimSegments.Num()));
	}

	FString SegName = Track.AnimSegments[SegIndex].GetAnimReference() ? Track.AnimSegments[SegIndex].GetAnimReference()->GetName() : TEXT("null");

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_RemoveSegment", "MCP: Remove Montage Segment"));
	Montage->Modify();

	Track.AnimSegments.RemoveAt(SegIndex);
	Track.CollapseAnimSegments();
	Montage->UpdateLinkableElements();
	Montage->PostEditChange();
	Montage->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_segment -> Removed '%s' from slot [%d] segment [%d]"), *SegName, SlotIndex, SegIndex);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_SetSegmentProperty(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UAnimMontage* Montage = GetMontageOrError(Data, "set_segment_property", Error);
	if (!Montage) return MakeErrorResult(Error);

	double SlotIndexD = 0.0;
	Params->TryGetNumberField(TEXT("slot_index"), SlotIndexD);
	int32 SlotIndex = static_cast<int32>(SlotIndexD);

	if (SlotIndex < 0 || SlotIndex >= Montage->SlotAnimTracks.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Slot index %d out of range [0, %d)"), SlotIndex, Montage->SlotAnimTracks.Num()));
	}

	double SegIndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("segment_index"), SegIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: segment_index"));
	}
	int32 SegIndex = static_cast<int32>(SegIndexD);

	FAnimTrack& Track = Montage->SlotAnimTracks[SlotIndex].AnimTrack;
	if (SegIndex < 0 || SegIndex >= Track.AnimSegments.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Segment index %d out of range [0, %d)"), SegIndex, Track.AnimSegments.Num()));
	}

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_name"));
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_SetSegmentProp", "MCP: Set Segment Property"));
	Montage->Modify();

	FAnimSegment& Seg = Track.AnimSegments[SegIndex];
	FString PropLower = PropertyName.ToLower();

	if (PropLower == TEXT("anim_path") || PropLower == TEXT("animation"))
	{
		UAnimSequenceBase* NewAnim = LoadObject<UAnimSequenceBase>(nullptr, *Value);
		if (!NewAnim)
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to load animation: %s"), *Value));
		}
		Seg.SetAnimReference(NewAnim);
	}
	else if (PropLower == TEXT("play_rate"))
	{
		Seg.AnimPlayRate = FCString::Atof(*Value);
	}
	else if (PropLower == TEXT("anim_start_time"))
	{
		Seg.AnimStartTime = FCString::Atof(*Value);
	}
	else if (PropLower == TEXT("anim_end_time"))
	{
		Seg.AnimEndTime = FCString::Atof(*Value);
	}
	else if (PropLower == TEXT("looping_count"))
	{
		Seg.LoopingCount = FCString::Atoi(*Value);
	}
	else if (PropLower == TEXT("start_pos"))
	{
		Seg.StartPos = FCString::Atof(*Value);
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Unknown segment property '%s'. Supported: animation, play_rate, anim_start_time, anim_end_time, looping_count, start_pos"), *PropertyName));
	}

	Track.CollapseAnimSegments();
	Montage->UpdateLinkableElements();
	Montage->PostEditChange();
	Montage->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("set_segment_property -> slot [%d] segment [%d]: %s = %s"), SlotIndex, SegIndex, *PropertyName, *Value);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_AddSlot(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UAnimMontage* Montage = GetMontageOrError(Data, "add_slot", Error);
	if (!Montage) return MakeErrorResult(Error);

	FString SlotName;
	if (!Params->TryGetStringField(TEXT("slot_name"), SlotName) || SlotName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: slot_name"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_AddSlot", "MCP: Add Montage Slot"));
	Montage->Modify();

	Montage->AddSlot(FName(*SlotName));
	Montage->PostEditChange();
	Montage->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	int32 NewIndex = Montage->SlotAnimTracks.Num() - 1;
	Data->LastOperationStatus = FString::Printf(TEXT("add_slot -> Added slot '%s' [%d]"), *SlotName, NewIndex);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_RemoveSlot(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UAnimMontage* Montage = GetMontageOrError(Data, "remove_slot", Error);
	if (!Montage) return MakeErrorResult(Error);

	double SlotIndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("slot_index"), SlotIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: slot_index"));
	}
	int32 SlotIndex = static_cast<int32>(SlotIndexD);

	if (SlotIndex < 0 || SlotIndex >= Montage->SlotAnimTracks.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Slot index %d out of range [0, %d)"), SlotIndex, Montage->SlotAnimTracks.Num()));
	}

	if (Montage->SlotAnimTracks.Num() <= 1)
	{
		return MakeErrorResult(TEXT("Cannot remove the last slot track"));
	}

	FString SlotName = Montage->SlotAnimTracks[SlotIndex].SlotName.ToString();

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_RemoveSlot", "MCP: Remove Montage Slot"));
	Montage->Modify();

	Montage->SlotAnimTracks.RemoveAt(SlotIndex);
	Montage->PostEditChange();
	Montage->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_slot -> Removed slot '%s' [%d]"), *SlotName, SlotIndex);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_SetSlotProperty(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UAnimMontage* Montage = GetMontageOrError(Data, "set_slot_property", Error);
	if (!Montage) return MakeErrorResult(Error);

	double SlotIndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("slot_index"), SlotIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: slot_index"));
	}
	int32 SlotIndex = static_cast<int32>(SlotIndexD);

	if (SlotIndex < 0 || SlotIndex >= Montage->SlotAnimTracks.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Slot index %d out of range [0, %d)"), SlotIndex, Montage->SlotAnimTracks.Num()));
	}

	FString SlotName;
	if (!Params->TryGetStringField(TEXT("slot_name"), SlotName) || SlotName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: slot_name"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_SetSlotProp", "MCP: Set Slot Property"));
	Montage->Modify();

	FString OldName = Montage->SlotAnimTracks[SlotIndex].SlotName.ToString();
	Montage->SlotAnimTracks[SlotIndex].SlotName = FName(*SlotName);
	Montage->PostEditChange();
	Montage->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("set_slot_property -> Renamed slot [%d] '%s' -> '%s'"), SlotIndex, *OldName, *SlotName);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_InspectSegment(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UAnimMontage* Montage = GetMontageOrError(Data, "inspect_segment", Error);
	if (!Montage) return MakeErrorResult(Error);

	double SlotIndexD = 0.0;
	Params->TryGetNumberField(TEXT("slot_index"), SlotIndexD);
	int32 SlotIndex = static_cast<int32>(SlotIndexD);

	if (SlotIndex < 0 || SlotIndex >= Montage->SlotAnimTracks.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Slot index %d out of range [0, %d)"), SlotIndex, Montage->SlotAnimTracks.Num()));
	}

	double SegIndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("segment_index"), SegIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: segment_index"));
	}
	int32 SegIndex = static_cast<int32>(SegIndexD);

	const FAnimTrack& Track = Montage->SlotAnimTracks[SlotIndex].AnimTrack;
	if (SegIndex < 0 || SegIndex >= Track.AnimSegments.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Segment index %d out of range [0, %d)"), SegIndex, Track.AnimSegments.Num()));
	}

	const FAnimSegment& Seg = Track.AnimSegments[SegIndex];
	UAnimSequenceBase* AnimRef = Seg.GetAnimReference();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("session_id"), SessionId);
	Result->SetNumberField(TEXT("slot_index"), SlotIndex);
	Result->SetNumberField(TEXT("segment_index"), SegIndex);
	Result->SetStringField(TEXT("animation"), AnimRef ? AnimRef->GetPathName() : TEXT("None"));
	Result->SetStringField(TEXT("animation_name"), AnimRef ? AnimRef->GetName() : TEXT("None"));
	Result->SetNumberField(TEXT("start_pos"), Seg.StartPos);
	Result->SetNumberField(TEXT("anim_start_time"), Seg.AnimStartTime);
	Result->SetNumberField(TEXT("anim_end_time"), Seg.AnimEndTime);
	Result->SetNumberField(TEXT("play_rate"), Seg.AnimPlayRate);
	Result->SetNumberField(TEXT("looping_count"), Seg.LoopingCount);
	Result->SetNumberField(TEXT("duration"), Seg.GetLength());
	Result->SetNumberField(TEXT("end_pos"), Seg.StartPos + Seg.GetLength());
	Result->SetNumberField(TEXT("source_length"), AnimRef ? AnimRef->GetPlayLength() : 0.0f);

	// Find notifies linked to this segment
	TArray<TSharedPtr<FJsonValue>> NotifyArray;
	for (int32 n = 0; n < Montage->Notifies.Num(); ++n)
	{
		const FAnimNotifyEvent& Notify = Montage->Notifies[n];
		if (Notify.GetSlotIndex() == SlotIndex && Notify.GetSegmentIndex() == SegIndex)
		{
			TSharedPtr<FJsonObject> NotifyObj = MakeShared<FJsonObject>();
			NotifyObj->SetNumberField(TEXT("notify_index"), n);
			NotifyObj->SetStringField(TEXT("name"), Notify.NotifyName.ToString());
			NotifyObj->SetNumberField(TEXT("time"), Notify.GetTime());
			if (Notify.NotifyStateClass)
			{
				NotifyObj->SetNumberField(TEXT("duration"), Notify.GetDuration());
				NotifyObj->SetStringField(TEXT("class"), Notify.NotifyStateClass->GetClass()->GetName());
			}
			else if (Notify.Notify)
			{
				NotifyObj->SetStringField(TEXT("class"), Notify.Notify->GetClass()->GetName());
			}
			// Show link method
			switch (Notify.GetLinkMethod())
			{
				case EAnimLinkMethod::Absolute: NotifyObj->SetStringField(TEXT("link_method"), TEXT("absolute")); break;
				case EAnimLinkMethod::Relative: NotifyObj->SetStringField(TEXT("link_method"), TEXT("relative")); break;
				case EAnimLinkMethod::Proportional: NotifyObj->SetStringField(TEXT("link_method"), TEXT("proportional")); break;
			}
			NotifyArray.Add(MakeShared<FJsonValueObject>(NotifyObj));
		}
	}
	Result->SetArrayField(TEXT("notifies"), NotifyArray);

	Data->LastOperationStatus = FString::Printf(TEXT("inspect_segment -> slot [%d] segment [%d]: %s (%.3fs-%.3fs)"),
		SlotIndex, SegIndex, AnimRef ? *AnimRef->GetName() : TEXT("None"), Seg.StartPos, Seg.StartPos + Seg.GetLength());
	Result->SetStringField(TEXT("status"), Data->LastOperationStatus);
	return MakeSuccessResult(Result, Data->LastOperationStatus);
}

FToolResult ClaireonTool_AnimEdit::Operation_RetimeSegment(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UAnimMontage* Montage = GetMontageOrError(Data, "retime_segment", Error);
	if (!Montage) return MakeErrorResult(Error);

	double SlotIndexD = 0.0;
	Params->TryGetNumberField(TEXT("slot_index"), SlotIndexD);
	int32 SlotIndex = static_cast<int32>(SlotIndexD);

	if (SlotIndex < 0 || SlotIndex >= Montage->SlotAnimTracks.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Slot index %d out of range [0, %d)"), SlotIndex, Montage->SlotAnimTracks.Num()));
	}

	double SegIndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("segment_index"), SegIndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: segment_index"));
	}
	int32 SegIndex = static_cast<int32>(SegIndexD);

	FAnimTrack& Track = Montage->SlotAnimTracks[SlotIndex].AnimTrack;
	if (SegIndex < 0 || SegIndex >= Track.AnimSegments.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Segment index %d out of range [0, %d)"), SegIndex, Track.AnimSegments.Num()));
	}

	FAnimSegment& Seg = Track.AnimSegments[SegIndex];

	// Capture old timing
	float OldStartPos = Seg.StartPos;
	float OldLength = Seg.GetLength();

	// Apply the requested change
	double NewEndTime = -1.0, NewDuration = -1.0, NewPlayRate = -1.0;
	Params->TryGetNumberField(TEXT("new_end_time"), NewEndTime);
	Params->TryGetNumberField(TEXT("new_duration"), NewDuration);
	Params->TryGetNumberField(TEXT("new_play_rate"), NewPlayRate);

	if (NewEndTime < 0.0 && NewDuration < 0.0 && NewPlayRate < 0.0)
	{
		return MakeErrorResult(TEXT("At least one of new_end_time, new_duration, or new_play_rate is required"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_RetimeSegment", "MCP: Retime Montage Segment"));
	Montage->Modify();

	if (NewEndTime >= 0.0)
	{
		Seg.AnimEndTime = static_cast<float>(NewEndTime);
	}
	else if (NewPlayRate > 0.0)
	{
		Seg.AnimPlayRate = static_cast<float>(NewPlayRate);
	}
	else if (NewDuration > 0.0)
	{
		// Adjust play rate to achieve desired duration
		float AnimRange = Seg.AnimEndTime - Seg.AnimStartTime;
		if (AnimRange > 0.0f)
		{
			Seg.AnimPlayRate = (AnimRange * Seg.LoopingCount) / static_cast<float>(NewDuration);
		}
	}

	float NewLength = Seg.GetLength();
	float LengthRatio = (OldLength > 0.0f) ? (NewLength / OldLength) : 1.0f;

	// Notify retiming mode:
	//   "manual" (default) — proportionally scale contained notifies ourselves
	//   "proportional" — change link method to Proportional, let engine auto-scale
	//   "relative" — change link method to Relative (move with segment start, don't scale)
	//   "absolute" — change link method to Absolute (don't move at all)
	//   "none" / false — don't touch notifies
	FString NotifyMode = TEXT("manual");
	{
		bool bRetimeNotifies = true;
		if (Params->TryGetBoolField(TEXT("retime_notifies"), bRetimeNotifies) && !bRetimeNotifies)
		{
			NotifyMode = TEXT("none");
		}
		FString ModeStr;
		if (Params->TryGetStringField(TEXT("notify_link_method"), ModeStr))
		{
			NotifyMode = ModeStr.ToLower();
		}
	}

	if (NotifyMode != TEXT("none") && FMath::Abs(LengthRatio - 1.0f) > KINDA_SMALL_NUMBER)
	{
		float OldSegEnd = OldStartPos + OldLength;

		if (NotifyMode == TEXT("proportional") || NotifyMode == TEXT("relative") || NotifyMode == TEXT("absolute"))
		{
			// Change link method only on notifies fully contained within this segment
			EAnimLinkMethod::Type DesiredMethod = EAnimLinkMethod::Absolute;
			if (NotifyMode == TEXT("proportional")) DesiredMethod = EAnimLinkMethod::Proportional;
			else if (NotifyMode == TEXT("relative")) DesiredMethod = EAnimLinkMethod::Relative;

			for (FAnimNotifyEvent& Notify : Montage->Notifies)
			{
				float NotifyStart = Notify.GetTime();
				float NotifyEnd = Notify.NotifyStateClass ? (NotifyStart + Notify.GetDuration()) : NotifyStart;
				if (NotifyStart < OldStartPos - KINDA_SMALL_NUMBER || NotifyEnd > OldSegEnd + KINDA_SMALL_NUMBER)
				{
					continue;
				}
				if (Notify.GetLinkMethod() != DesiredMethod)
				{
					Notify.ChangeLinkMethod(DesiredMethod);
				}
			}
		}
		else // "manual" — proportionally scale contained notifies ourselves
		{
			for (FAnimNotifyEvent& Notify : Montage->Notifies)
			{
				float NotifyStart = Notify.GetTime();
				float NotifyEnd = Notify.NotifyStateClass ? (NotifyStart + Notify.GetDuration()) : NotifyStart;

				// Only retime notifies fully contained within this segment's old time range
				if (NotifyStart < OldStartPos - KINDA_SMALL_NUMBER || NotifyEnd > OldSegEnd + KINDA_SMALL_NUMBER)
				{
					continue;
				}

				float RelativePos = (OldLength > 0.0f) ? (NotifyStart - OldStartPos) / OldLength : 0.0f;
				Notify.SetTime(OldStartPos + RelativePos * NewLength);

				if (Notify.NotifyStateClass && Notify.GetDuration() > 0.0f)
				{
					Notify.SetDuration(Notify.GetDuration() * LengthRatio);
				}
			}
		}
	}

	Track.CollapseAnimSegments();
	Montage->UpdateLinkableElements();
	Montage->PostEditChange();
	Montage->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);

	Data->LastOperationStatus = FString::Printf(TEXT("retime_segment -> slot [%d] segment [%d]: %.3fs -> %.3fs (ratio: %.3f, notify_mode: %s)"),
		SlotIndex, SegIndex, OldLength, NewLength, LengthRatio, *NotifyMode);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_BatchRetimeAnimation(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString AnimPath;
	if (!Params->TryGetStringField(TEXT("anim_path"), AnimPath) || AnimPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: anim_path"));
	}

	UAnimSequenceBase* TargetAnim = LoadObject<UAnimSequenceBase>(nullptr, *AnimPath);
	if (!TargetAnim)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load animation: %s"), *AnimPath));
	}

	float CurrentAnimLength = TargetAnim->GetPlayLength();

	double NewLengthD = -1.0;
	Params->TryGetNumberField(TEXT("new_length"), NewLengthD);
	float NewLength = (NewLengthD > 0.0) ? static_cast<float>(NewLengthD) : CurrentAnimLength;

	double OldLengthD = -1.0;
	Params->TryGetNumberField(TEXT("old_length"), OldLengthD);
	float OldSourceLength = (OldLengthD > 0.0) ? static_cast<float>(OldLengthD) : 0.0f; // 0 = auto-detect from segment

	// Notify mode: same options as retime_segment
	FString NotifyMode = TEXT("manual");
	{
		bool bRetimeNotifies = true;
		if (Params->TryGetBoolField(TEXT("retime_notifies"), bRetimeNotifies) && !bRetimeNotifies)
		{
			NotifyMode = TEXT("none");
		}
		FString ModeStr;
		if (Params->TryGetStringField(TEXT("notify_link_method"), ModeStr))
		{
			NotifyMode = ModeStr.ToLower();
		}
	}

	// Use GetReferencers to find only montages that actually reference this animation
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FName AnimPackageName = TargetAnim->GetOutermost()->GetFName();

	TArray<FAssetIdentifier> Referencers;
	AssetRegistry.GetReferencers(FAssetIdentifier(AnimPackageName), Referencers, UE::AssetRegistry::EDependencyCategory::Package);

	int32 MontagesUpdated = 0;
	int32 SegmentsUpdated = 0;
	TArray<TSharedPtr<FJsonValue>> UpdatedMontages;

	for (const FAssetIdentifier& Ref : Referencers)
	{
		// Only load if it's a montage — check asset data first
		TArray<FAssetData> AssetsInPackage;
		AssetRegistry.GetAssetsByPackageName(Ref.PackageName, AssetsInPackage);

		for (const FAssetData& AssetData : AssetsInPackage)
		{
			if (!AssetData.AssetClassPath.GetAssetName().ToString().Contains(TEXT("AnimMontage")))
			{
				continue;
			}

			UAnimMontage* Montage = Cast<UAnimMontage>(AssetData.GetAsset());
			if (!Montage) continue;

			bool bMontageModified = false;

			FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_BatchRetime", "MCP: Batch Retime Animation"));
			Montage->Modify();

			for (int32 SlotIdx = 0; SlotIdx < Montage->SlotAnimTracks.Num(); ++SlotIdx)
			{
				FAnimTrack& Track = Montage->SlotAnimTracks[SlotIdx].AnimTrack;
				for (int32 SegIdx = 0; SegIdx < Track.AnimSegments.Num(); ++SegIdx)
				{
					FAnimSegment& Seg = Track.AnimSegments[SegIdx];
					if (Seg.GetAnimReference() != TargetAnim) continue;

					float OldStartPos = Seg.StartPos;
					float OldLength = Seg.GetLength();

					// Scale AnimEndTime proportionally rather than replacing it
					// This preserves trims: if segment was trimmed to 80% of old source, it stays at 80% of new source
					float EffectiveOldSource = (OldSourceLength > 0.0f) ? OldSourceLength : Seg.AnimEndTime;
					float ScaleRatio = (EffectiveOldSource > 0.0f) ? (NewLength / EffectiveOldSource) : 1.0f;
					Seg.AnimEndTime *= ScaleRatio;
					Seg.AnimStartTime *= ScaleRatio;

					float NewSegLength = Seg.GetLength();
					float LengthRatio = (OldLength > 0.0f) ? (NewSegLength / OldLength) : 1.0f;

					// Retime contained notifies
					if (NotifyMode != TEXT("none") && FMath::Abs(LengthRatio - 1.0f) > KINDA_SMALL_NUMBER)
					{
						float OldSegEnd = OldStartPos + OldLength;

						if (NotifyMode == TEXT("proportional") || NotifyMode == TEXT("relative") || NotifyMode == TEXT("absolute"))
						{
							EAnimLinkMethod::Type DesiredMethod = EAnimLinkMethod::Absolute;
							if (NotifyMode == TEXT("proportional")) DesiredMethod = EAnimLinkMethod::Proportional;
							else if (NotifyMode == TEXT("relative")) DesiredMethod = EAnimLinkMethod::Relative;

							for (FAnimNotifyEvent& Notify : Montage->Notifies)
							{
								float NotifyStart = Notify.GetTime();
								float NotifyEnd = Notify.NotifyStateClass ? (NotifyStart + Notify.GetDuration()) : NotifyStart;
								if (NotifyStart < OldStartPos - KINDA_SMALL_NUMBER || NotifyEnd > OldSegEnd + KINDA_SMALL_NUMBER)
									continue;
								if (Notify.GetLinkMethod() != DesiredMethod)
									Notify.ChangeLinkMethod(DesiredMethod);
							}
						}
						else // manual
						{
							for (FAnimNotifyEvent& Notify : Montage->Notifies)
							{
								float NotifyStart = Notify.GetTime();
								float NotifyEnd = Notify.NotifyStateClass ? (NotifyStart + Notify.GetDuration()) : NotifyStart;
								if (NotifyStart < OldStartPos - KINDA_SMALL_NUMBER || NotifyEnd > OldSegEnd + KINDA_SMALL_NUMBER)
									continue;

								float RelativePos = (OldLength > 0.0f) ? (NotifyStart - OldStartPos) / OldLength : 0.0f;
								Notify.SetTime(OldStartPos + RelativePos * NewSegLength);
								if (Notify.NotifyStateClass && Notify.GetDuration() > 0.0f)
									Notify.SetDuration(Notify.GetDuration() * LengthRatio);
							}
						}
					}

					bMontageModified = true;
					SegmentsUpdated++;
				}

				if (bMontageModified)
				{
					Track.CollapseAnimSegments();
				}
			}

			if (bMontageModified)
			{
				Montage->UpdateLinkableElements();
				Montage->PostEditChange();
				Montage->MarkPackageDirty();
				ClaireonAssetUtils::RefreshAssetEditorIfOpen(Montage);
				MontagesUpdated++;

				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("montage_path"), Montage->GetPathName());
				Entry->SetStringField(TEXT("montage_name"), Montage->GetName());
				UpdatedMontages.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("session_id"), SessionId);
	Result->SetStringField(TEXT("anim_path"), AnimPath);
	Result->SetNumberField(TEXT("new_length"), NewLength);
	Result->SetNumberField(TEXT("montages_updated"), MontagesUpdated);
	Result->SetNumberField(TEXT("segments_updated"), SegmentsUpdated);
	Result->SetArrayField(TEXT("updated_montages"), UpdatedMontages);

	Data->LastOperationStatus = FString::Printf(TEXT("batch_retime_animation -> Updated %d segment(s) in %d montage(s) for '%s' (new length: %.3fs)"),
		SegmentsUpdated, MontagesUpdated, *TargetAnim->GetName(), NewLength);
	Result->SetStringField(TEXT("status"), Data->LastOperationStatus);
	return MakeSuccessResult(Result, Data->LastOperationStatus);
}

// ============================================================================
// Modifier Operations (AnimSequence only)
// ============================================================================

FToolResult ClaireonTool_AnimEdit::Operation_ListModifiers(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (Data->AssetType != TEXT("AnimSequence"))
	{
		return MakeErrorResult(TEXT("Modifier operations are only valid for AnimSequence assets"));
	}

	UAnimSequence* AnimSeq = Cast<UAnimSequence>(Data->Animation.Get());
	if (!AnimSeq)
	{
		return MakeErrorResult(TEXT("Failed to cast to AnimSequence"));
	}

	FString ModifiersText = ClaireonAnimHelpers::FormatModifiers(AnimSeq, true);

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetStringField(TEXT("asset_path"), Data->Animation->GetPathName());
	ResponseData->SetStringField(TEXT("modifiers_view"), ModifiersText);

	Data->LastOperationStatus = TEXT("list_modifiers");
	return MakeSuccessResult(ResponseData, FString::Printf(TEXT("Session %s: list_modifiers"), *SessionId.Left(8)));
}

// Helper: resolve a modifier class name to UClass* (native or Blueprint)
static UClass* ResolveModifierClass(const FString& ClassName, FString& OutError)
{
	UClass* BaseClass = UAnimationModifier::StaticClass();

	// Try direct class name lookup
	UClass* FoundClass = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	if (FoundClass && FoundClass->IsChildOf(BaseClass))
	{
		return FoundClass;
	}

	// Try with U prefix stripped
	if (ClassName.StartsWith(TEXT("U")))
	{
		FString WithoutU = ClassName.Mid(1);
		FoundClass = FindFirstObject<UClass>(*WithoutU, EFindFirstObjectOptions::NativeFirst);
		if (FoundClass && FoundClass->IsChildOf(BaseClass))
		{
			return FoundClass;
		}
	}

	// Search asset registry for Blueprint modifier classes
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

	OutError = FString::Printf(TEXT("Could not resolve modifier class: %s. Use the full class name or a short name matching a loaded class."), *ClassName);
	return nullptr;
}

// Helper: access the protected AnimationModifierInstances array via UE reflection
static TArray<TObjectPtr<UAnimationModifier>>* GetModifierArrayPtr(UAnimationModifiersAssetUserData* AssetUserData)
{
	FProperty* Prop = AssetUserData->GetClass()->FindPropertyByName(TEXT("AnimationModifierInstances"));
	if (!Prop) return nullptr;
	return Prop->ContainerPtrToValuePtr<TArray<TObjectPtr<UAnimationModifier>>>(AssetUserData);
}

// Helper: access the protected AppliedModifiers map via UE reflection
static TMap<FSoftObjectPath, TObjectPtr<UAnimationModifier>>* GetAppliedModifiersMapPtr(UAnimationModifiersAssetUserData* AssetUserData)
{
	FProperty* Prop = AssetUserData->GetClass()->FindPropertyByName(TEXT("AppliedModifiers"));
	if (!Prop) return nullptr;
	return Prop->ContainerPtrToValuePtr<TMap<FSoftObjectPath, TObjectPtr<UAnimationModifier>>>(AssetUserData);
}

FToolResult ClaireonTool_AnimEdit::Operation_AddModifier(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (Data->AssetType != TEXT("AnimSequence"))
	{
		return MakeErrorResult(TEXT("Modifier operations are only valid for AnimSequence assets"));
	}

	UAnimSequence* AnimSeq = Cast<UAnimSequence>(Data->Animation.Get());
	if (!AnimSeq)
	{
		return MakeErrorResult(TEXT("Failed to cast to AnimSequence"));
	}

	FString ClassName;
	if (!Params->TryGetStringField(TEXT("class_name"), ClassName) || ClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: class_name"));
	}

	FString Error;
	UClass* ModifierClass = ResolveModifierClass(ClassName, Error);
	if (!ModifierClass)
	{
		return MakeErrorResult(Error);
	}

	// Get or create asset user data
	UAnimationModifiersAssetUserData* AssetUserData = AnimSeq->GetAssetUserData<UAnimationModifiersAssetUserData>();
	if (!AssetUserData)
	{
		AssetUserData = NewObject<UAnimationModifiersAssetUserData>(AnimSeq, UAnimationModifiersAssetUserData::StaticClass());
		AssetUserData->SetFlags(RF_Transactional);
		AnimSeq->AddAssetUserData(AssetUserData);
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_AddModifier", "MCP: Add Animation Modifier"));
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

FToolResult ClaireonTool_AnimEdit::Operation_RemoveModifier(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (Data->AssetType != TEXT("AnimSequence"))
	{
		return MakeErrorResult(TEXT("Modifier operations are only valid for AnimSequence assets"));
	}

	UAnimSequence* AnimSeq = Cast<UAnimSequence>(Data->Animation.Get());
	if (!AnimSeq)
	{
		return MakeErrorResult(TEXT("Failed to cast to AnimSequence"));
	}

	double IndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("modifier_index"), IndexD))
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

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_RemoveModifier", "MCP: Remove Animation Modifier"));
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

FToolResult ClaireonTool_AnimEdit::Operation_ApplyModifier(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (Data->AssetType != TEXT("AnimSequence"))
	{
		return MakeErrorResult(TEXT("Modifier operations are only valid for AnimSequence assets"));
	}

	UAnimSequence* AnimSeq = Cast<UAnimSequence>(Data->Animation.Get());
	if (!AnimSeq)
	{
		return MakeErrorResult(TEXT("Failed to cast to AnimSequence"));
	}

	double IndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("modifier_index"), IndexD))
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

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_ApplyModifier", "MCP: Apply Animation Modifier"));
	Data->Animation->Modify();

	Modifier->ApplyToAnimationSequence(AnimSeq);

	Data->LastOperationStatus = FString::Printf(TEXT("apply_modifier -> Applied %s [%d]"), *Modifier->GetClass()->GetName(), Index);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_RevertModifier(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (Data->AssetType != TEXT("AnimSequence"))
	{
		return MakeErrorResult(TEXT("Modifier operations are only valid for AnimSequence assets"));
	}

	UAnimSequence* AnimSeq = Cast<UAnimSequence>(Data->Animation.Get());
	if (!AnimSeq)
	{
		return MakeErrorResult(TEXT("Failed to cast to AnimSequence"));
	}

	double IndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("modifier_index"), IndexD))
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

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_RevertModifier", "MCP: Revert Animation Modifier"));
	Data->Animation->Modify();

	Modifier->RevertFromAnimationSequence(AnimSeq);

	Data->LastOperationStatus = FString::Printf(TEXT("revert_modifier -> Reverted %s [%d]"), *Modifier->GetClass()->GetName(), Index);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Metadata Operations
// ============================================================================

FToolResult ClaireonTool_AnimEdit::Operation_ListMetadata(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString MetadataText = ClaireonAnimHelpers::FormatMetadata(Data->Animation.Get(), true);

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetStringField(TEXT("asset_path"), Data->Animation->GetPathName());
	ResponseData->SetStringField(TEXT("metadata_view"), MetadataText);

	Data->LastOperationStatus = TEXT("list_metadata");
	return MakeSuccessResult(ResponseData, FString::Printf(TEXT("Session %s: list_metadata"), *SessionId.Left(8)));
}

FToolResult ClaireonTool_AnimEdit::Operation_AddMetadata(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString ClassName;
	if (!Params->TryGetStringField(TEXT("class_name"), ClassName) || ClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: class_name"));
	}

	FString Error;
	UClass* MetaDataClass = ClaireonAnimHelpers::ResolveMetaDataClass(ClassName, Error);
	if (!MetaDataClass)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_AddMetadata", "MCP: Add Animation Metadata"));
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

FToolResult ClaireonTool_AnimEdit::Operation_RemoveMetadata(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	double IndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("metadata_index"), IndexD))
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

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_RemoveMetadata", "MCP: Remove Animation Metadata"));
	Data->Animation->Modify();

	Data->Animation->RemoveMetaData(MetaDataObj);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_metadata -> Removed %s [%d]"), *MetaDataName, Index);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_AnimEdit::Operation_SetMetadataProperty(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	double IndexD = -1.0;
	if (!Params->TryGetNumberField(TEXT("metadata_index"), IndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: metadata_index"));
	}
	int32 Index = static_cast<int32>(IndexD);

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_name"));
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
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

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_SetMetadataProp", "MCP: Set Metadata Property"));
	Data->Animation->Modify();
	MetaDataObj->Modify();

	FString Error;
	if (!ClaireonPropertyUtils::WritePropertyByPath(MetaDataObj, PropertyName, Value, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("set_metadata_property -> [%d].%s = %s"), Index, *PropertyName, *Value);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Property Operations
// ============================================================================

FToolResult ClaireonTool_AnimEdit::Operation_SetProperty(const FString& SessionId, FAnimEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_name"));
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_SetProperty", "MCP: Set Animation Property"));
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
		FString Error;
		if (!ClaireonPropertyUtils::WritePropertyByPath(Anim, PropertyName, Value, Error))
		{
			return MakeErrorResult(Error);
		}
		Data->LastOperationStatus = FString::Printf(TEXT("set_property -> %s = %s"), *PropertyName, *Value);
		return BuildStateResponse(SessionId, Data);
	}
}

// ============================================================================
// Asset Creation Operations (session-agnostic)
// ============================================================================

FToolResult ClaireonTool_AnimEdit::Operation_CreateMontage(const TSharedPtr<FJsonObject>& Params)
{
	FString Path;
	if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: path"));
	}
	FString SkeletonPath;
	if (!Params->TryGetStringField(TEXT("skeleton"), SkeletonPath) || SkeletonPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: skeleton"));
	}

	// Validate target path
	FString CanonPath, AssetName, Error;
	if (!ValidateNewAssetPath(Path, CanonPath, AssetName, Error))
	{
		return MakeErrorResult(Error);
	}

	// Load skeleton
	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
	if (!Skeleton)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load skeleton at '%s'"), *SkeletonPath));
	}

	// Optionally load source animation
	FString AnimPath;
	UAnimSequence* SourceAnim = nullptr;
	if (Params->TryGetStringField(TEXT("animation"), AnimPath) && !AnimPath.IsEmpty())
	{
		SourceAnim = LoadObject<UAnimSequence>(nullptr, *AnimPath);
		if (!SourceAnim)
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to load animation at '%s'"), *AnimPath));
		}
		// Validate skeleton compatibility
		if (SourceAnim->GetSkeleton() != Skeleton)
		{
			return MakeErrorResult(TEXT("Source animation skeleton does not match the provided skeleton"));
		}
	}

	// Optional slot name
	FString SlotName = TEXT("DefaultSlot");
	Params->TryGetStringField(TEXT("slot_name"), SlotName);

	// Create montage via engine factory (handles frame rate, default section, etc.)
	UPackage* Package = CreatePackage(*CanonPath);

	UAnimMontageFactory* Factory = NewObject<UAnimMontageFactory>();
	Factory->TargetSkeleton = Skeleton;
	if (SourceAnim)
	{
		Factory->SourceAnimation = SourceAnim;
	}

	UAnimMontage* Montage = Cast<UAnimMontage>(
		Factory->FactoryCreateNew(UAnimMontage::StaticClass(), Package,
			FName(*AssetName), RF_Public | RF_Standalone, nullptr, GWarn));
	if (!Montage)
	{
		return MakeErrorResult(TEXT("Factory failed to create montage"));
	}

	// Set custom slot name if specified
	if (SlotName != TEXT("DefaultSlot") && Montage->SlotAnimTracks.Num() > 0)
	{
		Montage->SlotAnimTracks[0].SlotName = FName(*SlotName);
	}

	// Register and save
	FAssetRegistryModule::AssetCreated(Montage);

	FString SaveError;
	if (!SaveNewAsset(Montage, SaveError))
	{
		return MakeErrorResult(SaveError);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), Montage->GetPathName());
	Result->SetStringField(TEXT("asset_type"), TEXT("AnimMontage"));
	Result->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	Result->SetStringField(TEXT("slot_name"), SlotName);
	if (SourceAnim)
	{
		Result->SetStringField(TEXT("animation"), SourceAnim->GetPathName());
		Result->SetNumberField(TEXT("length"), Montage->GetPlayLength());
	}

	return MakeSuccessResult(Result, FString::Printf(TEXT("Created montage '%s'"), *AssetName));
}

FToolResult ClaireonTool_AnimEdit::Operation_CreateComposite(const TSharedPtr<FJsonObject>& Params)
{
	FString Path;
	if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: path"));
	}
	FString SkeletonPath;
	if (!Params->TryGetStringField(TEXT("skeleton"), SkeletonPath) || SkeletonPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: skeleton"));
	}

	// Validate target path
	FString CanonPath, AssetName, Error;
	if (!ValidateNewAssetPath(Path, CanonPath, AssetName, Error))
	{
		return MakeErrorResult(Error);
	}

	// Load skeleton
	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
	if (!Skeleton)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load skeleton at '%s'"), *SkeletonPath));
	}

	// Optionally load source animation
	FString AnimPath;
	UAnimSequence* SourceAnim = nullptr;
	if (Params->TryGetStringField(TEXT("animation"), AnimPath) && !AnimPath.IsEmpty())
	{
		SourceAnim = LoadObject<UAnimSequence>(nullptr, *AnimPath);
		if (!SourceAnim)
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to load animation at '%s'"), *AnimPath));
		}
		if (SourceAnim->GetSkeleton() != Skeleton)
		{
			return MakeErrorResult(TEXT("Source animation skeleton does not match the provided skeleton"));
		}
	}

	// Create composite via engine factory (handles frame rate, etc.)
	UPackage* Package = CreatePackage(*CanonPath);

	UAnimCompositeFactory* CompositeFactory = NewObject<UAnimCompositeFactory>();
	CompositeFactory->TargetSkeleton = Skeleton;
	if (SourceAnim)
	{
		CompositeFactory->SourceAnimation = SourceAnim;
	}

	UAnimComposite* Composite = Cast<UAnimComposite>(
		CompositeFactory->FactoryCreateNew(UAnimComposite::StaticClass(), Package,
			FName(*AssetName), RF_Public | RF_Standalone, nullptr, GWarn));
	if (!Composite)
	{
		return MakeErrorResult(TEXT("Factory failed to create composite"));
	}

	// Composite factory doesn't call UpdateCommonTargetFrameRate (unlike montage factory).
	// Set CommonTargetFrameRate via reflection since it's a protected UPROPERTY.
	if (SourceAnim)
	{
		FProperty* FrameRateProp = UAnimCompositeBase::StaticClass()->FindPropertyByName(TEXT("CommonTargetFrameRate"));
		if (FrameRateProp)
		{
			FFrameRate* TargetRate = FrameRateProp->ContainerPtrToValuePtr<FFrameRate>(Composite);
			*TargetRate = SourceAnim->GetSamplingFrameRate();
		}
	}

	// Register and save
	FAssetRegistryModule::AssetCreated(Composite);

	FString SaveError;
	if (!SaveNewAsset(Composite, SaveError))
	{
		return MakeErrorResult(SaveError);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), Composite->GetPathName());
	Result->SetStringField(TEXT("asset_type"), TEXT("AnimComposite"));
	Result->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	if (SourceAnim)
	{
		Result->SetStringField(TEXT("animation"), SourceAnim->GetPathName());
		Result->SetNumberField(TEXT("length"), Composite->GetPlayLength());
	}

	return MakeSuccessResult(Result, FString::Printf(TEXT("Created composite '%s'"), *AssetName));
}

FToolResult ClaireonTool_AnimEdit::Operation_DuplicateAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString SourcePath;
	if (!Params->TryGetStringField(TEXT("source_path"), SourcePath) || SourcePath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: source_path"));
	}
	FString DestPath;
	if (!Params->TryGetStringField(TEXT("dest_path"), DestPath) || DestPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: dest_path"));
	}

	// Canonicalize paths
	SourcePath = FClaireonSessionManager::CanonicalizePath(SourcePath);
	if (SourcePath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Invalid source_path. Must start with /Game/."));
	}
	DestPath = FClaireonSessionManager::CanonicalizePath(DestPath);
	if (DestPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Invalid dest_path. Must start with /Game/."));
	}

	// Load source asset
	UObject* SourceObj = FSoftObjectPath(SourcePath).TryLoad();
	if (!SourceObj)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load source asset at '%s'"), *SourcePath));
	}

	// Verify it's an animation asset
	UAnimSequenceBase* SourceAnim = Cast<UAnimSequenceBase>(SourceObj);
	if (!SourceAnim)
	{
		return MakeErrorResult(FString::Printf(TEXT("Source asset '%s' is not an animation asset (AnimSequence, AnimMontage, or AnimComposite)"), *SourcePath));
	}

	// Check dest doesn't already exist
	if (StaticFindObject(nullptr, nullptr, *DestPath))
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset already exists at '%s'"), *DestPath));
	}

	// Parse dest into folder + name
	FString DestFolder = FPackageName::GetLongPackagePath(DestPath);
	FString DestName = FPackageName::GetShortName(DestPath);

	// Duplicate via engine AssetTools
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* NewAsset = AssetTools.DuplicateAsset(DestName, DestFolder, SourceObj);
	if (!NewAsset)
	{
		return MakeErrorResult(FString::Printf(TEXT("Engine failed to duplicate asset to '%s/%s'"), *DestFolder, *DestName));
	}

	// Save the new package
	UPackage* NewPackage = NewAsset->GetOutermost();
	NewPackage->MarkPackageDirty();

	FString PackageFileName = FPackageName::LongPackageNameToFilename(NewPackage->GetName(), FPackageName::GetAssetPackageExtension());

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::Save(NewPackage, NewAsset, *PackageFileName, SaveArgs);

	// Detect type
	FString AssetType = TEXT("AnimSequence");
	if (Cast<UAnimMontage>(NewAsset))
		AssetType = TEXT("AnimMontage");
	else if (Cast<UAnimComposite>(NewAsset))
		AssetType = TEXT("AnimComposite");

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source_path"), SourceAnim->GetPathName());
	Result->SetStringField(TEXT("dest_path"), NewAsset->GetPathName());
	Result->SetStringField(TEXT("asset_type"), AssetType);

	return MakeSuccessResult(Result, FString::Printf(TEXT("Duplicated %s to '%s'"), *AssetType, *DestName));
}
