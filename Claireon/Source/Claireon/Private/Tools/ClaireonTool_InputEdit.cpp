// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_InputEdit.h"
#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "ClaireonLog.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSafeExec.h"
#include "ClaireonSessionManager.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "EnhancedActionKeyMapping.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "FileHelpers.h"
#include "AssetRegistry/AssetRegistryModule.h"

using FToolResult = IClaireonTool::FToolResult;

// Static tool data storage
TMap<FString, FInputEditToolData> ClaireonTool_InputEdit::ToolData;
bool ClaireonTool_InputEdit::bDelegateRegistered = false;

void ClaireonTool_InputEdit::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == TEXT("claireon.input_edit"))
	{
		ToolData.Remove(Info.SessionId);
	}
}

// ============================================================================
// Scope guard helpers
// ============================================================================

namespace
{
	UInputAction* GetInputActionFromSession(FInputEditToolData* Data, FString& OutError)
	{
		if (!Data || !Data->IsValid())
		{
			OutError = TEXT("Session is invalid");
			return nullptr;
		}
		if (Data->AssetType != EInputAssetType::InputAction)
		{
			OutError = TEXT("This operation is only valid for Input Action sessions");
			return nullptr;
		}
		return Data->InputAction.Get();
	}

	UInputMappingContext* GetMappingContextFromSession(FInputEditToolData* Data, FString& OutError)
	{
		if (!Data || !Data->IsValid())
		{
			OutError = TEXT("Session is invalid");
			return nullptr;
		}
		if (Data->AssetType != EInputAssetType::MappingContext)
		{
			OutError = TEXT("This operation is only valid for Input Mapping Context sessions");
			return nullptr;
		}
		return Data->MappingContext.Get();
	}
}

// ============================================================================
// Tool Interface
// ============================================================================

FString ClaireonTool_InputEdit::GetName() const
{
	return TEXT("claireon.input_edit");
}

FString ClaireonTool_InputEdit::GetDescription() const
{
	return TEXT("Session-based Enhanced Input editor. Create and modify Input Actions and Input Mapping Contexts. "
				"Start with 'open' or 'create', configure, then 'save'.");
}

FString ClaireonTool_InputEdit::GetFullDescription() const
{
	return TEXT("Session-based Enhanced Input editor for Input Actions (IA) and Input Mapping Contexts (IMC).\n\n"
				"Session-less operations: open, create\n"
				"Session operations: close, status, save\n"
				"Input Action operations: set_value_type, set_action_property, add_action_trigger, "
				"remove_action_trigger, set_action_trigger_property, add_action_modifier, "
				"remove_action_modifier, set_action_modifier_property\n"
				"IMC operations: add_mapping, remove_mapping, set_mapping_key, set_mapping_action, "
				"add_mapping_trigger, remove_mapping_trigger, add_mapping_modifier, remove_mapping_modifier");
}

TSharedPtr<FJsonObject> ClaireonTool_InputEdit::GetInputSchema() const
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
		// Session management
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("open")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("create")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("close")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("status")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("save")));
		// Input Action operations
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_value_type")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_action_property")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("add_action_trigger")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("remove_action_trigger")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_action_trigger_property")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("add_action_modifier")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("remove_action_modifier")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_action_modifier_property")));
		// IMC operations
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("add_mapping")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("remove_mapping")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_mapping_key")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_mapping_action")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("add_mapping_trigger")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("remove_mapping_trigger")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("add_mapping_modifier")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("remove_mapping_modifier")));
		OpProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("operation"), OpProp);

	// session_id
	TSharedPtr<FJsonObject> SessionProp = MakeShared<FJsonObject>();
	SessionProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionProp->SetStringField(TEXT("description"), TEXT("Session identifier from a previous 'open' or 'create' operation."));
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
		TEXT("When true, returns only a brief status instead of the full asset state."));
	Properties->SetObjectField(TEXT("suppress_output"), SuppressOutputProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("operation")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonTool_InputEdit::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Operation;
	if (!Arguments->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: operation"));
	}

	bool bSuppressOutput = false;
	Arguments->TryGetBoolField(TEXT("suppress_output"), bSuppressOutput);

	// Session-less operations
	if (Operation == TEXT("open"))
	{
		TSharedPtr<FJsonObject> Params = Arguments->HasField(TEXT("params"))
			? Arguments->GetObjectField(TEXT("params"))
			: MakeShared<FJsonObject>();
		return Operation_Open(Params);
	}
	if (Operation == TEXT("create"))
	{
		TSharedPtr<FJsonObject> Params = Arguments->HasField(TEXT("params"))
			? Arguments->GetObjectField(TEXT("params"))
			: MakeShared<FJsonObject>();
		return Operation_Create(Params);
	}

	// Session-required operations
	FString SessionId;
	if (!Arguments->TryGetStringField(TEXT("session_id"), SessionId) || SessionId.IsEmpty())
	{
		return MakeErrorResult(FString::Printf(TEXT("Operation '%s' requires session_id"), *Operation));
	}

	FMCPSession* Session = FClaireonSessionManager::Get().FindSession(SessionId);
	if (!Session)
	{
		return MakeErrorResult(FString::Printf(TEXT("Session not found or expired: %s"), *SessionId));
	}

	FInputEditToolData* Data = ToolData.Find(SessionId);
	if (!Data)
	{
		return MakeErrorResult(TEXT("Session tool data not found"));
	}

	Data->bSuppressOutput = bSuppressOutput;

	TSharedPtr<FJsonObject> Params = Arguments->HasField(TEXT("params"))
		? Arguments->GetObjectField(TEXT("params"))
		: MakeShared<FJsonObject>();

	// Session management operations
	if (Operation == TEXT("close"))    { return Operation_Close(SessionId, Data, Params); }
	if (Operation == TEXT("status"))   { return Operation_Status(SessionId, Data, Params); }
	if (Operation == TEXT("save"))     { return Operation_Save(SessionId, Data, Params); }

	// Input Action operations
	if (Operation == TEXT("set_value_type"))              { return Operation_SetValueType(SessionId, Data, Params); }
	if (Operation == TEXT("set_action_property"))         { return Operation_SetActionProperty(SessionId, Data, Params); }
	if (Operation == TEXT("add_action_trigger"))          { return Operation_AddActionTrigger(SessionId, Data, Params); }
	if (Operation == TEXT("remove_action_trigger"))       { return Operation_RemoveActionTrigger(SessionId, Data, Params); }
	if (Operation == TEXT("set_action_trigger_property")) { return Operation_SetActionTriggerProperty(SessionId, Data, Params); }
	if (Operation == TEXT("add_action_modifier"))         { return Operation_AddActionModifier(SessionId, Data, Params); }
	if (Operation == TEXT("remove_action_modifier"))      { return Operation_RemoveActionModifier(SessionId, Data, Params); }
	if (Operation == TEXT("set_action_modifier_property")) { return Operation_SetActionModifierProperty(SessionId, Data, Params); }

	// IMC operations
	if (Operation == TEXT("add_mapping"))            { return Operation_AddMapping(SessionId, Data, Params); }
	if (Operation == TEXT("remove_mapping"))         { return Operation_RemoveMapping(SessionId, Data, Params); }
	if (Operation == TEXT("set_mapping_key"))        { return Operation_SetMappingKey(SessionId, Data, Params); }
	if (Operation == TEXT("set_mapping_action"))     { return Operation_SetMappingAction(SessionId, Data, Params); }
	if (Operation == TEXT("add_mapping_trigger"))    { return Operation_AddMappingTrigger(SessionId, Data, Params); }
	if (Operation == TEXT("remove_mapping_trigger")) { return Operation_RemoveMappingTrigger(SessionId, Data, Params); }
	if (Operation == TEXT("add_mapping_modifier"))   { return Operation_AddMappingModifier(SessionId, Data, Params); }
	if (Operation == TEXT("remove_mapping_modifier")) { return Operation_RemoveMappingModifier(SessionId, Data, Params); }

	return MakeErrorResult(FString::Printf(TEXT("Unknown operation: %s"), *Operation));
}

// ============================================================================
// Response Building
// ============================================================================

FToolResult ClaireonTool_InputEdit::BuildStateResponse(const FString& SessionId, FInputEditToolData* Data)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	if (Data->bSuppressOutput)
	{
		return MakeErrorResult(Data->LastOperationStatus.IsEmpty()
			? TEXT("ok")
			: FString::Printf(TEXT("ok: %s"), *Data->LastOperationStatus));
	}

	FString Output;
	Output += TEXT("=== Session Status ===\n");
	Output += FString::Printf(TEXT("Session: %s\n"), *SessionId);
	Output += FString::Printf(TEXT("Last Operation: %s\n"), *Data->LastOperationStatus);

	if (Data->AssetType == EInputAssetType::InputAction)
	{
		Output += FString::Printf(TEXT("Asset Type: Input Action\n"));
		Output += FString::Printf(TEXT("Asset: %s\n\n"), *Data->InputAction->GetPathName());
		Output += ClaireonEnhancedInputHelpers::FormatInputAction(Data->InputAction.Get(), false);
	}
	else
	{
		Output += FString::Printf(TEXT("Asset Type: Input Mapping Context\n"));
		Output += FString::Printf(TEXT("Asset: %s\n\n"), *Data->MappingContext->GetPathName());
		Output += ClaireonEnhancedInputHelpers::FormatMappingContext(Data->MappingContext.Get(), false);
	}

	return MakeErrorResult(Output);
}

// ============================================================================
// Session Operations
// ============================================================================

FToolResult ClaireonTool_InputEdit::Operation_Open(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'open' requires params.asset_path"));
	}

	FString LoadError;
	UObject* Asset = ClaireonEnhancedInputHelpers::LoadInputAsset(AssetPath, LoadError);
	if (!Asset)
	{
		return MakeErrorResult(LoadError);
	}

	// Register delegate if not done yet
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonTool_InputEdit::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	const FString ResolvedAssetPath = Asset->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, TEXT("claireon.input_edit"));
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

	FInputEditToolData NewData;
	NewData.LastOperationStatus = TEXT("Session opened");

	if (UInputAction* IA = Cast<UInputAction>(Asset))
	{
		NewData.AssetType = EInputAssetType::InputAction;
		NewData.InputAction = IA;
	}
	else if (UInputMappingContext* IMC = Cast<UInputMappingContext>(Asset))
	{
		NewData.AssetType = EInputAssetType::MappingContext;
		NewData.MappingContext = IMC;
	}
	ToolData.Add(SessionId, MoveTemp(NewData));

	FInputEditToolData* Data = ToolData.Find(SessionId);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_InputEdit::Operation_Create(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'create' requires params.asset_path"));
	}

	FString AssetTypeStr;
	if (!Params->TryGetStringField(TEXT("asset_type"), AssetTypeStr) || AssetTypeStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("'create' requires params.asset_type ('input_action' or 'mapping_context')"));
	}

	bool bIsIA = (AssetTypeStr == TEXT("input_action") || AssetTypeStr == TEXT("ia"));
	bool bIsIMC = (AssetTypeStr == TEXT("mapping_context") || AssetTypeStr == TEXT("imc"));
	if (!bIsIA && !bIsIMC)
	{
		return MakeErrorResult(FString::Printf(TEXT("Unknown asset_type: %s (expected: 'input_action' or 'mapping_context')"), *AssetTypeStr));
	}

	// Resolve path
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	// Check asset doesn't already exist
	FSoftObjectPath SoftPath(AssetPath);
	if (SoftPath.TryLoad())
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset already exists at path: %s. Use 'open' instead."), *AssetPath));
	}

	// Create package
	FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create package: %s"), *PackageName));
	}

	UObject* NewAsset = nullptr;
	EInputAssetType NewAssetType;
	FString ShortName = FPackageName::GetShortName(AssetPath);

	if (bIsIA)
	{
		UInputAction* IA = NewObject<UInputAction>(Package, *ShortName, RF_Public | RF_Standalone);
		if (!IA)
		{
			return MakeErrorResult(TEXT("Failed to create Input Action"));
		}
		NewAsset = IA;
		NewAssetType = EInputAssetType::InputAction;
	}
	else
	{
		UInputMappingContext* IMC = NewObject<UInputMappingContext>(Package, *ShortName, RF_Public | RF_Standalone);
		if (!IMC)
		{
			return MakeErrorResult(TEXT("Failed to create Input Mapping Context"));
		}
		NewAsset = IMC;
		NewAssetType = EInputAssetType::MappingContext;
	}

	// Save package
	Package->SetDirtyFlag(true);
	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	if (!ClaireonSafeExec::DidLastExecutionCrash())
	{
		UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);
	}

	// Register with asset registry
	FAssetRegistryModule::AssetCreated(NewAsset);

	// Open edit session
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonTool_InputEdit::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	const FString ResolvedAssetPath = NewAsset->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, TEXT("claireon.input_edit"));
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		return MakeErrorResult(FString::Printf(TEXT("Asset is locked by %s session %s"), *Blocker.ToolName, *Blocker.SessionId));
	}
	const FString SessionId = OpenResult.SessionId;

	FInputEditToolData NewData;
	NewData.AssetType = NewAssetType;
	NewData.LastOperationStatus = TEXT("Asset created and session opened");
	if (NewAssetType == EInputAssetType::InputAction)
	{
		NewData.InputAction = Cast<UInputAction>(NewAsset);
	}
	else
	{
		NewData.MappingContext = Cast<UInputMappingContext>(NewAsset);
	}
	ToolData.Add(SessionId, MoveTemp(NewData));

	FInputEditToolData* Data = ToolData.Find(SessionId);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_InputEdit::Operation_Close(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	bool bSaveFirst = false;
	Params->TryGetBoolField(TEXT("save_first"), bSaveFirst);

	if (bSaveFirst)
	{
		Operation_Save(SessionId, Data, MakeShared<FJsonObject>());
	}

	FClaireonSessionManager::Get().CloseSession(SessionId);
	ToolData.Remove(SessionId);

	return MakeErrorResult(FString::Printf(TEXT("Session closed: %s"), *SessionId));
}

FToolResult ClaireonTool_InputEdit::Operation_Status(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_InputEdit::Operation_Save(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	UObject* Asset = (Data->AssetType == EInputAssetType::InputAction)
		? static_cast<UObject*>(Data->InputAction.Get())
		: static_cast<UObject*>(Data->MappingContext.Get());

	UPackage* Package = Asset->GetPackage();
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
		Data->LastOperationStatus = FString::Printf(TEXT("save -- Saved %s"), *Asset->GetPathName());
		return MakeErrorResult(FString::Printf(TEXT("Saved: %s"), *Asset->GetPathName()));
	}
	else
	{
		Data->LastOperationStatus = TEXT("save -- Failed");
		return MakeErrorResult(TEXT("Failed to save package"));
	}
}

// ============================================================================
// Input Action Operations
// ============================================================================

FToolResult ClaireonTool_InputEdit::Operation_SetValueType(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UInputAction* IA = GetInputActionFromSession(Data, Error);
	if (!IA)
	{
		return MakeErrorResult(Error);
	}

	FString ValueTypeStr;
	if (!Params->TryGetStringField(TEXT("value_type"), ValueTypeStr) || ValueTypeStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("'set_value_type' requires params.value_type (bool, float, 2d, 3d)"));
	}

	EInputActionValueType NewType;
	if (!ClaireonEnhancedInputHelpers::ParseValueType(ValueTypeStr, NewType, Error))
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Input Action Value Type")));
	IA->Modify();
	IA->ValueType = NewType;

	Data->LastOperationStatus = FString::Printf(TEXT("set_value_type -- %s"), *ClaireonEnhancedInputHelpers::ValueTypeToString(NewType));
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_InputEdit::Operation_SetActionProperty(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UInputAction* IA = GetInputActionFromSession(Data, Error);
	if (!IA)
	{
		return MakeErrorResult(Error);
	}

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'set_action_property' requires params.property_name"));
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("'set_action_property' requires params.value"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Input Action Property")));
	IA->Modify();

	if (!ClaireonEnhancedInputHelpers::SetObjectProperty(IA, PropertyName, Value, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("set_action_property -- %s = %s"), *PropertyName, *Value);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_InputEdit::Operation_AddActionTrigger(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UInputAction* IA = GetInputActionFromSession(Data, Error);
	if (!IA)
	{
		return MakeErrorResult(Error);
	}

	FString TriggerClassName;
	if (!Params->TryGetStringField(TEXT("trigger_class"), TriggerClassName) || TriggerClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'add_action_trigger' requires params.trigger_class"));
	}

	UClass* TriggerClass = ClaireonEnhancedInputHelpers::ResolveTriggerClass(TriggerClassName, Error);
	if (!TriggerClass)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Input Action Trigger")));
	IA->Modify();

	UInputTrigger* NewTrigger = ClaireonEnhancedInputHelpers::CreateTrigger(IA, TriggerClass);
	if (!NewTrigger)
	{
		return MakeErrorResult(TEXT("Failed to create trigger instance"));
	}

	int32 NewIndex = IA->Triggers.Add(NewTrigger);

	Data->LastOperationStatus = FString::Printf(TEXT("add_action_trigger -- Added %s at index %d"), *TriggerClass->GetName(), NewIndex);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_InputEdit::Operation_RemoveActionTrigger(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UInputAction* IA = GetInputActionFromSession(Data, Error);
	if (!IA)
	{
		return MakeErrorResult(Error);
	}

	int32 Index = -1;
	if (!Params->TryGetNumberField(TEXT("index"), Index))
	{
		return MakeErrorResult(TEXT("'remove_action_trigger' requires params.index"));
	}

	if (Index < 0 || Index >= IA->Triggers.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Trigger index %d out of range (0-%d)"), Index, IA->Triggers.Num() - 1));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Input Action Trigger")));
	IA->Modify();

	FString RemovedName = IA->Triggers[Index] ? IA->Triggers[Index]->GetClass()->GetName() : TEXT("(null)");
	IA->Triggers.RemoveAt(Index);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_action_trigger -- Removed %s from index %d"), *RemovedName, Index);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_InputEdit::Operation_SetActionTriggerProperty(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UInputAction* IA = GetInputActionFromSession(Data, Error);
	if (!IA)
	{
		return MakeErrorResult(Error);
	}

	int32 Index = -1;
	if (!Params->TryGetNumberField(TEXT("index"), Index))
	{
		return MakeErrorResult(TEXT("'set_action_trigger_property' requires params.index"));
	}

	if (Index < 0 || Index >= IA->Triggers.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Trigger index %d out of range (0-%d)"), Index, IA->Triggers.Num() - 1));
	}

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'set_action_trigger_property' requires params.property_name"));
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("'set_action_trigger_property' requires params.value"));
	}

	UInputTrigger* Trigger = IA->Triggers[Index];
	if (!Trigger)
	{
		return MakeErrorResult(FString::Printf(TEXT("Trigger at index %d is null"), Index));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Input Action Trigger Property")));
	IA->Modify();
	Trigger->Modify();

	if (!ClaireonEnhancedInputHelpers::SetObjectProperty(Trigger, PropertyName, Value, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("set_action_trigger_property -- [%d].%s = %s"), Index, *PropertyName, *Value);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_InputEdit::Operation_AddActionModifier(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UInputAction* IA = GetInputActionFromSession(Data, Error);
	if (!IA)
	{
		return MakeErrorResult(Error);
	}

	FString ModifierClassName;
	if (!Params->TryGetStringField(TEXT("modifier_class"), ModifierClassName) || ModifierClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'add_action_modifier' requires params.modifier_class"));
	}

	UClass* ModifierClass = ClaireonEnhancedInputHelpers::ResolveModifierClass(ModifierClassName, Error);
	if (!ModifierClass)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Input Action Modifier")));
	IA->Modify();

	UInputModifier* NewModifier = ClaireonEnhancedInputHelpers::CreateModifier(IA, ModifierClass);
	if (!NewModifier)
	{
		return MakeErrorResult(TEXT("Failed to create modifier instance"));
	}

	int32 NewIndex = IA->Modifiers.Add(NewModifier);

	Data->LastOperationStatus = FString::Printf(TEXT("add_action_modifier -- Added %s at index %d"), *ModifierClass->GetName(), NewIndex);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_InputEdit::Operation_RemoveActionModifier(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UInputAction* IA = GetInputActionFromSession(Data, Error);
	if (!IA)
	{
		return MakeErrorResult(Error);
	}

	int32 Index = -1;
	if (!Params->TryGetNumberField(TEXT("index"), Index))
	{
		return MakeErrorResult(TEXT("'remove_action_modifier' requires params.index"));
	}

	if (Index < 0 || Index >= IA->Modifiers.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Modifier index %d out of range (0-%d)"), Index, IA->Modifiers.Num() - 1));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Input Action Modifier")));
	IA->Modify();

	FString RemovedName = IA->Modifiers[Index] ? IA->Modifiers[Index]->GetClass()->GetName() : TEXT("(null)");
	IA->Modifiers.RemoveAt(Index);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_action_modifier -- Removed %s from index %d"), *RemovedName, Index);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_InputEdit::Operation_SetActionModifierProperty(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UInputAction* IA = GetInputActionFromSession(Data, Error);
	if (!IA)
	{
		return MakeErrorResult(Error);
	}

	int32 Index = -1;
	if (!Params->TryGetNumberField(TEXT("index"), Index))
	{
		return MakeErrorResult(TEXT("'set_action_modifier_property' requires params.index"));
	}

	if (Index < 0 || Index >= IA->Modifiers.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Modifier index %d out of range (0-%d)"), Index, IA->Modifiers.Num() - 1));
	}

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'set_action_modifier_property' requires params.property_name"));
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("'set_action_modifier_property' requires params.value"));
	}

	UInputModifier* Modifier = IA->Modifiers[Index];
	if (!Modifier)
	{
		return MakeErrorResult(FString::Printf(TEXT("Modifier at index %d is null"), Index));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Input Action Modifier Property")));
	IA->Modify();
	Modifier->Modify();

	if (!ClaireonEnhancedInputHelpers::SetObjectProperty(Modifier, PropertyName, Value, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("set_action_modifier_property -- [%d].%s = %s"), Index, *PropertyName, *Value);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Input Mapping Context Operations
// ============================================================================

FToolResult ClaireonTool_InputEdit::Operation_AddMapping(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UInputMappingContext* IMC = GetMappingContextFromSession(Data, Error);
	if (!IMC)
	{
		return MakeErrorResult(Error);
	}

	FString ActionPath;
	if (!Params->TryGetStringField(TEXT("action_path"), ActionPath) || ActionPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'add_mapping' requires params.action_path"));
	}

	FString KeyName;
	if (!Params->TryGetStringField(TEXT("key"), KeyName) || KeyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'add_mapping' requires params.key"));
	}

	// Load the input action
	auto ResolveResult = ClaireonPathResolver::Resolve(ActionPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	UInputAction* Action = LoadObject<UInputAction>(nullptr, *ResolveResult.ResolvedPath.Path);
	if (!Action)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Input Action at: %s"), *ActionPath));
	}

	// Resolve key
	FKey Key;
	if (!ClaireonEnhancedInputHelpers::ResolveKey(KeyName, Key, Error))
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Input Mapping")));
	IMC->Modify();

	IMC->MapKey(Action, Key);
	int32 NewIndex = IMC->GetMappings().Num() - 1;

	ClaireonEnhancedInputHelpers::NotifyMappingContextModified(IMC);

	Data->LastOperationStatus = FString::Printf(TEXT("add_mapping -- Added %s -> %s at index %d"), *Action->GetName(), *KeyName, NewIndex);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_InputEdit::Operation_RemoveMapping(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UInputMappingContext* IMC = GetMappingContextFromSession(Data, Error);
	if (!IMC)
	{
		return MakeErrorResult(Error);
	}

	int32 Index = -1;
	if (!Params->TryGetNumberField(TEXT("index"), Index))
	{
		return MakeErrorResult(TEXT("'remove_mapping' requires params.index"));
	}

	TArray<FEnhancedActionKeyMapping>& Mappings = ClaireonEnhancedInputHelpers::GetMappingsMutable(IMC);
	if (Index < 0 || Index >= Mappings.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Mapping index %d out of range (0-%d)"), Index, Mappings.Num() - 1));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Input Mapping")));
	IMC->Modify();

	Mappings.RemoveAt(Index);
	ClaireonEnhancedInputHelpers::NotifyMappingContextModified(IMC);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_mapping -- Removed mapping at index %d"), Index);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_InputEdit::Operation_SetMappingKey(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UInputMappingContext* IMC = GetMappingContextFromSession(Data, Error);
	if (!IMC)
	{
		return MakeErrorResult(Error);
	}

	int32 Index = -1;
	if (!Params->TryGetNumberField(TEXT("index"), Index))
	{
		return MakeErrorResult(TEXT("'set_mapping_key' requires params.index"));
	}

	const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();
	if (Index < 0 || Index >= Mappings.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Mapping index %d out of range (0-%d)"), Index, Mappings.Num() - 1));
	}

	FString KeyName;
	if (!Params->TryGetStringField(TEXT("key"), KeyName) || KeyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'set_mapping_key' requires params.key"));
	}

	FKey NewKey;
	if (!ClaireonEnhancedInputHelpers::ResolveKey(KeyName, NewKey, Error))
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Mapping Key")));
	IMC->Modify();

	IMC->GetMapping(Index).Key = NewKey;
	ClaireonEnhancedInputHelpers::NotifyMappingContextModified(IMC);

	Data->LastOperationStatus = FString::Printf(TEXT("set_mapping_key -- [%d].Key = %s"), Index, *KeyName);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_InputEdit::Operation_SetMappingAction(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UInputMappingContext* IMC = GetMappingContextFromSession(Data, Error);
	if (!IMC)
	{
		return MakeErrorResult(Error);
	}

	int32 Index = -1;
	if (!Params->TryGetNumberField(TEXT("index"), Index))
	{
		return MakeErrorResult(TEXT("'set_mapping_action' requires params.index"));
	}

	const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();
	if (Index < 0 || Index >= Mappings.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Mapping index %d out of range (0-%d)"), Index, Mappings.Num() - 1));
	}

	FString ActionPath;
	if (!Params->TryGetStringField(TEXT("action_path"), ActionPath) || ActionPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'set_mapping_action' requires params.action_path"));
	}

	auto ResolveResult = ClaireonPathResolver::Resolve(ActionPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	UInputAction* Action = LoadObject<UInputAction>(nullptr, *ResolveResult.ResolvedPath.Path);
	if (!Action)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Input Action at: %s"), *ActionPath));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Mapping Action")));
	IMC->Modify();

	IMC->GetMapping(Index).Action = Action;
	ClaireonEnhancedInputHelpers::NotifyMappingContextModified(IMC);

	Data->LastOperationStatus = FString::Printf(TEXT("set_mapping_action -- [%d].Action = %s"), Index, *Action->GetName());
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_InputEdit::Operation_AddMappingTrigger(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UInputMappingContext* IMC = GetMappingContextFromSession(Data, Error);
	if (!IMC)
	{
		return MakeErrorResult(Error);
	}

	int32 MappingIndex = -1;
	if (!Params->TryGetNumberField(TEXT("mapping_index"), MappingIndex))
	{
		return MakeErrorResult(TEXT("'add_mapping_trigger' requires params.mapping_index"));
	}

	const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();
	if (MappingIndex < 0 || MappingIndex >= Mappings.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Mapping index %d out of range (0-%d)"), MappingIndex, Mappings.Num() - 1));
	}

	FString TriggerClassName;
	if (!Params->TryGetStringField(TEXT("trigger_class"), TriggerClassName) || TriggerClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'add_mapping_trigger' requires params.trigger_class"));
	}

	UClass* TriggerClass = ClaireonEnhancedInputHelpers::ResolveTriggerClass(TriggerClassName, Error);
	if (!TriggerClass)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Mapping Trigger")));
	IMC->Modify();

	UInputTrigger* NewTrigger = ClaireonEnhancedInputHelpers::CreateTrigger(IMC, TriggerClass);
	if (!NewTrigger)
	{
		return MakeErrorResult(TEXT("Failed to create trigger instance"));
	}

	int32 NewIndex = IMC->GetMapping(MappingIndex).Triggers.Add(NewTrigger);
	ClaireonEnhancedInputHelpers::NotifyMappingContextModified(IMC);

	Data->LastOperationStatus = FString::Printf(TEXT("add_mapping_trigger -- [%d].Triggers[%d] = %s"), MappingIndex, NewIndex, *TriggerClass->GetName());
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_InputEdit::Operation_RemoveMappingTrigger(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UInputMappingContext* IMC = GetMappingContextFromSession(Data, Error);
	if (!IMC)
	{
		return MakeErrorResult(Error);
	}

	int32 MappingIndex = -1;
	if (!Params->TryGetNumberField(TEXT("mapping_index"), MappingIndex))
	{
		return MakeErrorResult(TEXT("'remove_mapping_trigger' requires params.mapping_index"));
	}

	const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();
	if (MappingIndex < 0 || MappingIndex >= Mappings.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Mapping index %d out of range (0-%d)"), MappingIndex, Mappings.Num() - 1));
	}

	int32 TriggerIndex = -1;
	if (!Params->TryGetNumberField(TEXT("trigger_index"), TriggerIndex))
	{
		return MakeErrorResult(TEXT("'remove_mapping_trigger' requires params.trigger_index"));
	}

	FEnhancedActionKeyMapping& Mapping = IMC->GetMapping(MappingIndex);
	if (TriggerIndex < 0 || TriggerIndex >= Mapping.Triggers.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Trigger index %d out of range (0-%d) on mapping %d"),
			TriggerIndex, Mapping.Triggers.Num() - 1, MappingIndex));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Mapping Trigger")));
	IMC->Modify();

	FString RemovedName = Mapping.Triggers[TriggerIndex] ? Mapping.Triggers[TriggerIndex]->GetClass()->GetName() : TEXT("(null)");
	Mapping.Triggers.RemoveAt(TriggerIndex);
	ClaireonEnhancedInputHelpers::NotifyMappingContextModified(IMC);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_mapping_trigger -- Removed %s from [%d].Triggers[%d]"), *RemovedName, MappingIndex, TriggerIndex);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_InputEdit::Operation_AddMappingModifier(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UInputMappingContext* IMC = GetMappingContextFromSession(Data, Error);
	if (!IMC)
	{
		return MakeErrorResult(Error);
	}

	int32 MappingIndex = -1;
	if (!Params->TryGetNumberField(TEXT("mapping_index"), MappingIndex))
	{
		return MakeErrorResult(TEXT("'add_mapping_modifier' requires params.mapping_index"));
	}

	const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();
	if (MappingIndex < 0 || MappingIndex >= Mappings.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Mapping index %d out of range (0-%d)"), MappingIndex, Mappings.Num() - 1));
	}

	FString ModifierClassName;
	if (!Params->TryGetStringField(TEXT("modifier_class"), ModifierClassName) || ModifierClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'add_mapping_modifier' requires params.modifier_class"));
	}

	UClass* ModifierClass = ClaireonEnhancedInputHelpers::ResolveModifierClass(ModifierClassName, Error);
	if (!ModifierClass)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Mapping Modifier")));
	IMC->Modify();

	UInputModifier* NewModifier = ClaireonEnhancedInputHelpers::CreateModifier(IMC, ModifierClass);
	if (!NewModifier)
	{
		return MakeErrorResult(TEXT("Failed to create modifier instance"));
	}

	int32 NewIndex = IMC->GetMapping(MappingIndex).Modifiers.Add(NewModifier);
	ClaireonEnhancedInputHelpers::NotifyMappingContextModified(IMC);

	Data->LastOperationStatus = FString::Printf(TEXT("add_mapping_modifier -- [%d].Modifiers[%d] = %s"), MappingIndex, NewIndex, *ModifierClass->GetName());
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_InputEdit::Operation_RemoveMappingModifier(const FString& SessionId, FInputEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UInputMappingContext* IMC = GetMappingContextFromSession(Data, Error);
	if (!IMC)
	{
		return MakeErrorResult(Error);
	}

	int32 MappingIndex = -1;
	if (!Params->TryGetNumberField(TEXT("mapping_index"), MappingIndex))
	{
		return MakeErrorResult(TEXT("'remove_mapping_modifier' requires params.mapping_index"));
	}

	const TArray<FEnhancedActionKeyMapping>& Mappings = IMC->GetMappings();
	if (MappingIndex < 0 || MappingIndex >= Mappings.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Mapping index %d out of range (0-%d)"), MappingIndex, Mappings.Num() - 1));
	}

	int32 ModifierIndex = -1;
	if (!Params->TryGetNumberField(TEXT("modifier_index"), ModifierIndex))
	{
		return MakeErrorResult(TEXT("'remove_mapping_modifier' requires params.modifier_index"));
	}

	FEnhancedActionKeyMapping& Mapping = IMC->GetMapping(MappingIndex);
	if (ModifierIndex < 0 || ModifierIndex >= Mapping.Modifiers.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Modifier index %d out of range (0-%d) on mapping %d"),
			ModifierIndex, Mapping.Modifiers.Num() - 1, MappingIndex));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Mapping Modifier")));
	IMC->Modify();

	FString RemovedName = Mapping.Modifiers[ModifierIndex] ? Mapping.Modifiers[ModifierIndex]->GetClass()->GetName() : TEXT("(null)");
	Mapping.Modifiers.RemoveAt(ModifierIndex);
	ClaireonEnhancedInputHelpers::NotifyMappingContextModified(IMC);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_mapping_modifier -- Removed %s from [%d].Modifiers[%d]"), *RemovedName, MappingIndex, ModifierIndex);
	return BuildStateResponse(SessionId, Data);
}
