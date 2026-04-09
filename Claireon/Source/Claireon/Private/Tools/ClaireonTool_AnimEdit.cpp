// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AnimEdit.h"
#include "Tools/ClaireonAnimHelpers.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "ClaireonLog.h"
#include "ClaireonSessionManager.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimMetaData.h"
#include "AnimationModifier.h"
#include "AnimationModifiersAssetUserData.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "FileHelpers.h"
#include "Editor.h"
#include "UObject/Package.h"
#include "Misc/Paths.h"

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
				"Session operations: open, close, get_state, save\n"
				"Notify operations: add_notify, remove_notify, move_notify, duplicate_notify, set_notify_property, get_notify_property, list_notify_properties, add_notify_track, remove_notify_track, rename_notify_track, reorder_notify_track\n"
				"Curve operations: add_curve, remove_curve, add_curve_key, remove_curve_key\n"
				"Montage section operations (montage only): add_section, remove_section, set_section_link\n"
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
	SessionIdProp->SetStringField(TEXT("description"), TEXT("Session identifier from a previous 'open' operation. Required for all operations except 'open'."));
	Properties->SetObjectField(TEXT("session_id"), SessionIdProp);

	// operation
	TSharedPtr<FJsonObject> OperationProp = MakeShared<FJsonObject>();
	OperationProp->SetStringField(TEXT("type"), TEXT("string"));
	TArray<TSharedPtr<FJsonValue>> OperationEnum;
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("open")));
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
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_section")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_section")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("set_section_link")));
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

	// Montage section ops
	if (Operation == TEXT("add_section"))
		return Operation_AddSection(SessionId, Data, Params);
	if (Operation == TEXT("remove_section"))
		return Operation_RemoveSection(SessionId, Data, Params);
	if (Operation == TEXT("set_section_link"))
		return Operation_SetSectionLink(SessionId, Data, Params);

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

	// Canonicalize path early to prevent malformed paths from reaching LoadObject
	AssetPath = FClaireonSessionManager::CanonicalizePath(AssetPath);
	if (AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Invalid asset path. Path must start with /Game/."));
	}

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

	double Time = 0.0;
	if (!Params->TryGetNumberField(TEXT("time"), Time))
	{
		return MakeErrorResult(TEXT("Missing required parameter: time"));
	}

	double Value = 0.0;
	if (!Params->TryGetNumberField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AnimEdit_AddCurveKey", "MCP: Add Curve Key"));
	Data->Animation->Modify();

	FString Error;
	if (!ClaireonAnimHelpers::AddCurveKey(Data->Animation.Get(), CurveName, static_cast<float>(Time), static_cast<float>(Value), Error))
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

	double Time = 0.0;
	if (!Params->TryGetNumberField(TEXT("time"), Time))
	{
		return MakeErrorResult(TEXT("Missing required parameter: time"));
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

	Data->LastOperationStatus = FString::Printf(TEXT("set_section_link -> %s -> %s"), *SectionName, *NextSectionName);
	return BuildStateResponse(SessionId, Data);
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

	// UAnimationModifiersAssetUserData::AddAnimationModifier is protected (friend-only).
	return MakeErrorResult(TEXT("add_modifier is not currently supported — UAnimationModifiersAssetUserData::AddAnimationModifier is protected. Use the Animation Data Modifiers panel in the editor instead."));
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

	// UAnimationModifiersAssetUserData::RemoveAnimationModifierInstance is protected (friend-only).
	return MakeErrorResult(TEXT("remove_modifier is not currently supported — UAnimationModifiersAssetUserData::RemoveAnimationModifierInstance is protected. Use the Animation Data Modifiers panel in the editor instead."));
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
