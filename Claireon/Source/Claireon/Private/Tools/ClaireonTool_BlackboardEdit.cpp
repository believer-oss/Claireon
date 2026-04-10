// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_BlackboardEdit.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonLog.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSafeExec.h"
#include "ClaireonSessionManager.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "FileHelpers.h"
#include "Misc/Guid.h"

using FToolResult = IClaireonTool::FToolResult;

// Static tool data storage
TMap<FString, FBlackboardEditToolData> ClaireonTool_BlackboardEdit::ToolData;
bool ClaireonTool_BlackboardEdit::bDelegateRegistered = false;

namespace
{
	UBlackboardKeyType* CreateKeyTypeForName(const FString& TypeName, UObject* Outer, FString& OutError)
	{
		static const TMap<FString, UClass*> TypeMap = {
			{ TEXT("Bool"), UBlackboardKeyType_Bool::StaticClass() },
			{ TEXT("Int"), UBlackboardKeyType_Int::StaticClass() },
			{ TEXT("Float"), UBlackboardKeyType_Float::StaticClass() },
			{ TEXT("String"), UBlackboardKeyType_String::StaticClass() },
			{ TEXT("Name"), UBlackboardKeyType_Name::StaticClass() },
			{ TEXT("Vector"), UBlackboardKeyType_Vector::StaticClass() },
			{ TEXT("Rotator"), UBlackboardKeyType_Rotator::StaticClass() },
			{ TEXT("Object"), UBlackboardKeyType_Object::StaticClass() },
			{ TEXT("Class"), UBlackboardKeyType_Class::StaticClass() },
			{ TEXT("Enum"), UBlackboardKeyType_Enum::StaticClass() },
		};

		const UClass* const* FoundClass = TypeMap.Find(TypeName);
		if (!FoundClass)
		{
			OutError = FString::Printf(TEXT("Unknown key type: '%s'. Supported types: Bool, Int, Float, String, Name, Vector, Rotator, Object, Class, Enum"), *TypeName);
			return nullptr;
		}

		return NewObject<UBlackboardKeyType>(Outer, *FoundClass);
	}
} // namespace

// ============================================================================
// Session Management
// ============================================================================

void ClaireonTool_BlackboardEdit::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == TEXT("claireon.blackboard_edit"))
	{
		ToolData.Remove(Info.SessionId);
	}
}

// ============================================================================
// Tool Interface
// ============================================================================

FString ClaireonTool_BlackboardEdit::GetName() const
{
	return TEXT("claireon.blackboard_edit");
}

FString ClaireonTool_BlackboardEdit::GetDescription() const
{
	return TEXT("Session-based Blackboard Data editor. Add, remove, rename keys and change types. Set parent blackboard and save.");
}

FString ClaireonTool_BlackboardEdit::GetFullDescription() const
{
	return TEXT("Session-based Blackboard Data editor. Supports adding/removing/renaming keys, "
				"changing key types, setting parent blackboard, and saving.\n\n"
				"Session operations: open, close, status\n"
				"Key operations: add_key, remove_key, rename_key, set_key_type\n"
				"Parent operations: set_parent\n"
				"Build operations: save");
}

TSharedPtr<FJsonObject> ClaireonTool_BlackboardEdit::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// operation
	TSharedPtr<FJsonObject> OpProp = MakeShared<FJsonObject>();
	OpProp->SetStringField(TEXT("type"), TEXT("string"));
	OpProp->SetStringField(TEXT("description"), TEXT("The editing operation to perform."));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("open")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("close")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("status")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("add_key")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("remove_key")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("rename_key")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_key_type")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_parent")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("save")));
		OpProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("operation"), OpProp);

	// session_id
	TSharedPtr<FJsonObject> SessionProp = MakeShared<FJsonObject>();
	SessionProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionProp->SetStringField(TEXT("description"), TEXT("Session identifier from a previous 'open' operation."));
	Properties->SetObjectField(TEXT("session_id"), SessionProp);

	// params
	TSharedPtr<FJsonObject> ParamsProp = MakeShared<FJsonObject>();
	ParamsProp->SetStringField(TEXT("type"), TEXT("object"));
	ParamsProp->SetStringField(TEXT("description"), TEXT("Operation-specific parameters."));
	Properties->SetObjectField(TEXT("params"), ParamsProp);

	// suppress_output
	TSharedPtr<FJsonObject> SuppressOutputProp = MakeShared<FJsonObject>();
	SuppressOutputProp->SetStringField(TEXT("type"), TEXT("boolean"));
	SuppressOutputProp->SetStringField(TEXT("description"),
		TEXT("When true, returns only a brief status instead of the full blackboard state."));
	Properties->SetObjectField(TEXT("suppress_output"), SuppressOutputProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("operation")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonTool_BlackboardEdit::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Operation;
	if (!Arguments->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: operation"));
	}

	// Params sub-object (optional, fall back to Arguments)
	TSharedPtr<FJsonObject> Params;
	const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
	if (Arguments->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr)
	{
		Params = *ParamsPtr;
	}
	else
	{
		Params = Arguments;
	}

	bool bSuppressOutput = false;
	if (Arguments->HasField(TEXT("suppress_output")))
	{
		bSuppressOutput = Arguments->GetBoolField(TEXT("suppress_output"));
	}

	// Operations that don't need a session
	if (Operation == TEXT("open"))
		return Operation_Open(Params);

	// All other operations require session_id
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

	FBlackboardEditToolData* Data = ToolData.Find(SessionId);
	if (!Data)
	{
		return MakeErrorResult(TEXT("Session tool data not found"));
	}

	Data->bSuppressOutput = bSuppressOutput;

	if (Operation == TEXT("close"))
		return Operation_Close(SessionId, Data, Params);
	if (Operation == TEXT("status"))
		return Operation_Status(SessionId, Data, Params);
	if (Operation == TEXT("add_key"))
		return Operation_AddKey(SessionId, Data, Params);
	if (Operation == TEXT("remove_key"))
		return Operation_RemoveKey(SessionId, Data, Params);
	if (Operation == TEXT("rename_key"))
		return Operation_RenameKey(SessionId, Data, Params);
	if (Operation == TEXT("set_key_type"))
		return Operation_SetKeyType(SessionId, Data, Params);
	if (Operation == TEXT("set_parent"))
		return Operation_SetParent(SessionId, Data, Params);
	if (Operation == TEXT("save"))
		return Operation_Save(SessionId, Data, Params);

	return MakeErrorResult(FString::Printf(TEXT("Unknown operation: %s"), *Operation));
}

// ============================================================================
// Response Building
// ============================================================================

FToolResult ClaireonTool_BlackboardEdit::BuildStateResponse(const FString& SessionId, FBlackboardEditToolData* Data)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	if (Data->bSuppressOutput)
	{
		const FString StatusMsg = Data->LastOperationStatus.IsEmpty()
			? TEXT("ok")
			: FString::Printf(TEXT("ok: %s"), *Data->LastOperationStatus);
		TSharedPtr<FJsonObject> SuppressJson = MakeShared<FJsonObject>();
		SuppressJson->SetStringField(TEXT("session_id"), SessionId);
		SuppressJson->SetStringField(TEXT("status"), StatusMsg);
		return MakeSuccessResult(SuppressJson, StatusMsg);
	}

	FString BBView;
	BBView += TEXT("=== Session Status ===\n");
	BBView += FString::Printf(TEXT("Session: %s\n"), *SessionId);
	BBView += FString::Printf(TEXT("Asset: %s\n"), *Data->BlackboardData->GetPathName());
	BBView += FString::Printf(TEXT("Last Operation: %s\n"), *Data->LastOperationStatus);
	BBView += TEXT("\n");
	BBView += ClaireonBehaviorTreeHelpers::FormatBlackboardData(Data->BlackboardData.Get(), false);

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), Data->BlackboardData->GetPathName());
	ResultJson->SetStringField(TEXT("session_id"), SessionId);
	ResultJson->SetStringField(TEXT("last_operation"), Data->LastOperationStatus);
	ResultJson->SetStringField(TEXT("blackboard_view"), BBView);

	const FString Summary = FString::Printf(TEXT("Session %s: %s"),
		*SessionId.Left(8), *Data->LastOperationStatus);

	return MakeSuccessResult(ResultJson, Summary);
}

// ============================================================================
// Session Operations
// ============================================================================

FToolResult ClaireonTool_BlackboardEdit::Operation_Open(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'open' requires params.asset_path"));
	}

	// Resolve path to canonical form
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	FString Error;
	UBlackboardData* BB = ClaireonBehaviorTreeHelpers::LoadBlackboardAsset(AssetPath, Error);
	if (!BB)
	{
		return MakeErrorResult(Error);
	}

	// Register delegate if not done yet
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonTool_BlackboardEdit::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	const FString ResolvedAssetPath = BB->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, TEXT("claireon.blackboard_edit"));
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		return MakeErrorResult(FString::Printf(TEXT("Asset is locked by %s session %s"), *Blocker.ToolName, *Blocker.SessionId));
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *ResolvedAssetPath));
	}
	const FString SessionId = OpenResult.SessionId;

	// If ReusedExistingSession, still update tool data
	FBlackboardEditToolData NewData;
	NewData.BlackboardData = BB;
	NewData.LastOperationStatus = TEXT("Session opened");
	ToolData.Add(SessionId, MoveTemp(NewData));

	FString StructureText = ClaireonBehaviorTreeHelpers::FormatBlackboardData(BB, false);

	TSharedPtr<FJsonObject> OpenData = MakeShared<FJsonObject>();
	OpenData->SetStringField(TEXT("session_id"), SessionId);
	OpenData->SetStringField(TEXT("asset_path"), AssetPath);
	OpenData->SetStringField(TEXT("status"), TEXT("Session opened"));
	OpenData->SetStringField(TEXT("structure"), StructureText);

	return MakeSuccessResult(OpenData, FString::Printf(TEXT("Opened session for %s"), *FPaths::GetBaseFilename(AssetPath)));
}

FToolResult ClaireonTool_BlackboardEdit::Operation_Close(const FString& SessionId, FBlackboardEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
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
	return MakeSuccessResult(CloseData, FString::Printf(TEXT("Session closed: %s"), *SessionId));
}

FToolResult ClaireonTool_BlackboardEdit::Operation_Status(const FString& SessionId, FBlackboardEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Key Operations
// ============================================================================

FToolResult ClaireonTool_BlackboardEdit::Operation_AddKey(const FString& SessionId, FBlackboardEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString KeyName;
	if (!Params->TryGetStringField(TEXT("key_name"), KeyName) || KeyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: key_name"));
	}

	FString KeyType;
	if (!Params->TryGetStringField(TEXT("key_type"), KeyType) || KeyType.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: key_type"));
	}

	bool bInstanceSynced = false;
	Params->TryGetBoolField(TEXT("instance_synced"), bInstanceSynced);

	UBlackboardData* BB = Data->BlackboardData.Get();

	// Check for duplicate key name
	for (const FBlackboardEntry& Entry : BB->Keys)
	{
		if (Entry.EntryName == FName(*KeyName))
		{
			return MakeErrorResult(FString::Printf(TEXT("Key '%s' already exists"), *KeyName));
		}
	}
	for (const FBlackboardEntry& Entry : BB->ParentKeys)
	{
		if (Entry.EntryName == FName(*KeyName))
		{
			return MakeErrorResult(FString::Printf(TEXT("Key '%s' already exists in parent blackboard"), *KeyName));
		}
	}

	FString Error;
	UBlackboardKeyType* NewKeyType = CreateKeyTypeForName(KeyType, BB, Error);
	if (!NewKeyType)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Blackboard Key")));
	BB->Modify();

	FBlackboardEntry NewEntry;
	NewEntry.EntryName = FName(*KeyName);
	NewEntry.KeyType = NewKeyType;
	NewEntry.bInstanceSynced = bInstanceSynced;

	BB->Keys.Add(NewEntry);
	BB->UpdateKeyIDs();
	BB->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("add_key â Added '%s' (%s)"), *KeyName, *KeyType);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_BlackboardEdit::Operation_RemoveKey(const FString& SessionId, FBlackboardEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString KeyName;
	if (!Params->TryGetStringField(TEXT("key_name"), KeyName) || KeyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: key_name"));
	}

	UBlackboardData* BB = Data->BlackboardData.Get();

	int32 KeyIndex = INDEX_NONE;
	for (int32 i = 0; i < BB->Keys.Num(); ++i)
	{
		if (BB->Keys[i].EntryName == FName(*KeyName))
		{
			KeyIndex = i;
			break;
		}
	}

	if (KeyIndex == INDEX_NONE)
	{
		return MakeErrorResult(FString::Printf(TEXT("Key '%s' not found in this blackboard's own keys"), *KeyName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Blackboard Key")));
	BB->Modify();

	BB->Keys.RemoveAt(KeyIndex);
	BB->UpdateKeyIDs();
	BB->PropagateKeyChangesToDerivedBlackboardAssets();
	BB->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("remove_key â Removed '%s'"), *KeyName);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_BlackboardEdit::Operation_RenameKey(const FString& SessionId, FBlackboardEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString OldName;
	if (!Params->TryGetStringField(TEXT("old_name"), OldName) || OldName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: old_name"));
	}

	FString NewName;
	if (!Params->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: new_name"));
	}

	UBlackboardData* BB = Data->BlackboardData.Get();

	FBlackboardEntry* FoundEntry = nullptr;
	for (FBlackboardEntry& Entry : BB->Keys)
	{
		if (Entry.EntryName == FName(*OldName))
		{
			FoundEntry = &Entry;
			break;
		}
	}

	if (!FoundEntry)
	{
		return MakeErrorResult(FString::Printf(TEXT("Key '%s' not found"), *OldName));
	}

	// Check new name doesn't conflict
	for (const FBlackboardEntry& Entry : BB->Keys)
	{
		if (Entry.EntryName == FName(*NewName))
		{
			return MakeErrorResult(FString::Printf(TEXT("Key '%s' already exists"), *NewName));
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Rename Blackboard Key")));
	BB->Modify();

	FoundEntry->EntryName = FName(*NewName);
	BB->UpdateKeyIDs();
	BB->PropagateKeyChangesToDerivedBlackboardAssets();
	BB->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("rename_key â '%s' â '%s'"), *OldName, *NewName);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_BlackboardEdit::Operation_SetKeyType(const FString& SessionId, FBlackboardEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString KeyName;
	if (!Params->TryGetStringField(TEXT("key_name"), KeyName) || KeyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: key_name"));
	}

	FString KeyType;
	if (!Params->TryGetStringField(TEXT("key_type"), KeyType) || KeyType.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: key_type"));
	}

	UBlackboardData* BB = Data->BlackboardData.Get();

	FBlackboardEntry* FoundEntry = nullptr;
	for (FBlackboardEntry& Entry : BB->Keys)
	{
		if (Entry.EntryName == FName(*KeyName))
		{
			FoundEntry = &Entry;
			break;
		}
	}

	if (!FoundEntry)
	{
		return MakeErrorResult(FString::Printf(TEXT("Key '%s' not found"), *KeyName));
	}

	FString Error;
	UBlackboardKeyType* NewKeyType = CreateKeyTypeForName(KeyType, BB, Error);
	if (!NewKeyType)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Blackboard Key Type")));
	BB->Modify();

	FoundEntry->KeyType = NewKeyType;
	BB->UpdateKeyIDs();
	BB->PropagateKeyChangesToDerivedBlackboardAssets();
	BB->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_key_type â '%s' â %s"), *KeyName, *KeyType);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Parent Operations
// ============================================================================

FToolResult ClaireonTool_BlackboardEdit::Operation_SetParent(const FString& SessionId, FBlackboardEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString ParentPath;
	Params->TryGetStringField(TEXT("parent_path"), ParentPath);

	UBlackboardData* BB = Data->BlackboardData.Get();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Blackboard Parent")));
	BB->Modify();

	if (ParentPath.IsEmpty())
	{
		BB->Parent = nullptr;
		BB->UpdateKeyIDs();
		BB->MarkPackageDirty();

		Data->LastOperationStatus = TEXT("set_parent â Cleared parent");
		return BuildStateResponse(SessionId, Data);
	}

	FString Error;
	UBlackboardData* ParentBB = ClaireonBehaviorTreeHelpers::LoadBlackboardAsset(ParentPath, Error);
	if (!ParentBB)
	{
		return MakeErrorResult(Error);
	}

	if (ParentBB == BB)
	{
		return MakeErrorResult(TEXT("Cannot set a blackboard as its own parent"));
	}

	BB->Parent = ParentBB;
	BB->UpdateKeyIDs();
	BB->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_parent â Set parent to %s"), *ParentPath);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Build Operations
// ============================================================================

FToolResult ClaireonTool_BlackboardEdit::Operation_Save(const FString& SessionId, FBlackboardEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	UBlackboardData* BB = Data->BlackboardData.Get();
	UPackage* Package = BB->GetPackage();
	Package->SetDirtyFlag(true);

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		return MakeErrorResult(TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor."));
	}
	bool bSuccess = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);

	if (bSuccess)
	{
		Data->LastOperationStatus = FString::Printf(TEXT("save â Saved %s"), *BB->GetPathName());
		return BuildStateResponse(SessionId, Data);
	}
	else
	{
		Data->LastOperationStatus = TEXT("save â Failed");
		return MakeErrorResult(TEXT("Failed to save Blackboard package"));
	}
}
