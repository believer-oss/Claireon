// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_NiagaraEdit.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "ClaireonLog.h"
#include "ClaireonSessionManager.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "FileHelpers.h"
#include "Misc/Guid.h"

using FToolResult = IClaireonTool::FToolResult;

// Static tool data storage
TMap<FString, FNiagaraEditToolData> ClaireonTool_NiagaraEdit::ToolData;
bool ClaireonTool_NiagaraEdit::bDelegateRegistered = false;

// ============================================================================
// Session Management
// ============================================================================

void ClaireonTool_NiagaraEdit::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == TEXT("editor.niagara.edit"))
	{
		ToolData.Remove(Info.SessionId);
	}
}

// ============================================================================
// Tool Interface
// ============================================================================

FString ClaireonTool_NiagaraEdit::GetName() const
{
	return TEXT("editor.niagara.edit");
}

FString ClaireonTool_NiagaraEdit::GetCategory() const
{
	return TEXT("niagara");
}

FString ClaireonTool_NiagaraEdit::GetDescription() const
{
	return TEXT("Session-based Niagara System editor. Manage emitters, renderers, and properties. Start with 'open', configure, then 'save'.");
}

FString ClaireonTool_NiagaraEdit::GetFullDescription() const
{
	return TEXT("Session-based Niagara System editor. Supports emitter management, renderer management, "
				"property editing, and saving.\n\n"
				"Session operations: open, close, status, focus_emitter\n"
				"Emitter operations: add_emitter, remove_emitter, rename_emitter, set_emitter_enabled\n"
				"Renderer operations: add_renderer, remove_renderer, set_renderer_property\n"
				"Property operations: set_emitter_property\n"
				"Build operations: save");
}

TSharedPtr<FJsonObject> ClaireonTool_NiagaraEdit::GetInputSchema() const
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
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("focus_emitter")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("add_emitter")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("remove_emitter")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("rename_emitter")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_emitter_enabled")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("add_renderer")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("remove_renderer")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_renderer_property")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_emitter_property")));
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
		TEXT("When true, returns only a brief status instead of the full Niagara state."));
	Properties->SetObjectField(TEXT("suppress_output"), SuppressOutputProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("operation")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonTool_NiagaraEdit::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FNiagaraEditToolData* Data = ToolData.Find(SessionId);
	if (!Data)
	{
		return MakeErrorResult(TEXT("Session tool data not found"));
	}

	Data->bSuppressOutput = bSuppressOutput;

	TSharedPtr<FJsonObject> Params = Arguments->HasField(TEXT("params"))
		? Arguments->GetObjectField(TEXT("params"))
		: MakeShared<FJsonObject>();

	if (Operation == TEXT("close"))
	{
		return Operation_Close(SessionId, Data, Params);
	}
	if (Operation == TEXT("status"))
	{
		return Operation_Status(SessionId, Data, Params);
	}
	if (Operation == TEXT("focus_emitter"))
	{
		return Operation_FocusEmitter(SessionId, Data, Params);
	}
	if (Operation == TEXT("add_emitter"))
	{
		return Operation_AddEmitter(SessionId, Data, Params);
	}
	if (Operation == TEXT("remove_emitter"))
	{
		return Operation_RemoveEmitter(SessionId, Data, Params);
	}
	if (Operation == TEXT("rename_emitter"))
	{
		return Operation_RenameEmitter(SessionId, Data, Params);
	}
	if (Operation == TEXT("set_emitter_enabled"))
	{
		return Operation_SetEmitterEnabled(SessionId, Data, Params);
	}
	if (Operation == TEXT("add_renderer"))
	{
		return Operation_AddRenderer(SessionId, Data, Params);
	}
	if (Operation == TEXT("remove_renderer"))
	{
		return Operation_RemoveRenderer(SessionId, Data, Params);
	}
	if (Operation == TEXT("set_renderer_property"))
	{
		return Operation_SetRendererProperty(SessionId, Data, Params);
	}
	if (Operation == TEXT("set_emitter_property"))
	{
		return Operation_SetEmitterProperty(SessionId, Data, Params);
	}
	if (Operation == TEXT("save"))
	{
		return Operation_Save(SessionId, Data, Params);
	}

	return MakeErrorResult(FString::Printf(TEXT("Unknown operation: %s"), *Operation));
}

// ============================================================================
// Response Building
// ============================================================================

FToolResult ClaireonTool_NiagaraEdit::BuildStateResponse(const FString& SessionId, FNiagaraEditToolData* Data)
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
	Output += FString::Printf(TEXT("Asset: %s\n"), *Data->System->GetPathName());
	Output += FString::Printf(TEXT("Focused Emitter: %s\n"),
		Data->FocusedEmitterIndex < 0 ? TEXT("System Level") : *FString::Printf(TEXT("%d"), Data->FocusedEmitterIndex));
	Output += FString::Printf(TEXT("Last Operation: %s\n"), *Data->LastOperationStatus);
	Output += TEXT("\n");
	Output += ClaireonNiagaraHelpers::FormatNiagaraSystemStructure(Data->System.Get(), false);

	return MakeErrorResult(Output);
}

// ============================================================================
// Session Operations
// ============================================================================

FToolResult ClaireonTool_NiagaraEdit::Operation_Open(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'open' requires params.asset_path"));
	}

	// Canonicalize path early to prevent malformed paths from reaching LoadObject
	AssetPath = FClaireonSessionManager::CanonicalizePath(AssetPath);
	if (AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Invalid asset path. Path must start with /Game/."));
	}

	FString Error;
	UNiagaraSystem* System = ClaireonNiagaraHelpers::LoadNiagaraSystemAsset(AssetPath, Error);
	if (!System)
	{
		return MakeErrorResult(Error);
	}

	// Register delegate if not done yet
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonTool_NiagaraEdit::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	const FString ResolvedAssetPath = System->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, TEXT("editor.niagara.edit"));
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
	FNiagaraEditToolData NewData;
	NewData.System = System;
	NewData.LastOperationStatus = TEXT("Session opened");
	ToolData.Add(SessionId, MoveTemp(NewData));

	FString Output;
	Output += FString::Printf(TEXT("=== Session Opened ===\nSession ID: %s\nAsset: %s\n\n"), *SessionId, *AssetPath);
	Output += ClaireonNiagaraHelpers::FormatNiagaraSystemStructure(System, false);
	return MakeErrorResult(Output);
}

FToolResult ClaireonTool_NiagaraEdit::Operation_Close(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
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

FToolResult ClaireonTool_NiagaraEdit::Operation_Status(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_NiagaraEdit::Operation_FocusEmitter(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	int32 EmitterIndex = -1;
	if (!Params->TryGetNumberField(TEXT("emitter_index"), EmitterIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: emitter_index (-1 for system level)"));
	}

	UNiagaraSystem* System = Data->System.Get();
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	if (EmitterIndex != -1 && (EmitterIndex < 0 || EmitterIndex >= Handles.Num()))
	{
		return MakeErrorResult(FString::Printf(TEXT("Emitter index %d out of range (0-%d, or -1 for system level)"),
			EmitterIndex, Handles.Num() - 1));
	}

	Data->FocusedEmitterIndex = EmitterIndex;

	Data->LastOperationStatus = EmitterIndex < 0
		? TEXT("focus_emitter â Focused on system level")
		: FString::Printf(TEXT("focus_emitter â Focused on emitter %d (%s)"),
			  EmitterIndex, *Handles[EmitterIndex].GetName().ToString());
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Emitter Operations
// ============================================================================

FToolResult ClaireonTool_NiagaraEdit::Operation_AddEmitter(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString EmitterName;
	if (!Params->TryGetStringField(TEXT("emitter_name"), EmitterName) || EmitterName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: emitter_name"));
	}

	UNiagaraSystem* System = Data->System.Get();

	FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Add Niagara Emitter")));
	System->Modify();

	UNiagaraEmitter* NewEmitter = NewObject<UNiagaraEmitter>(System, FName(*EmitterName), RF_Transactional);
	FGuid EmitterVersion = FGuid::NewGuid();
	System->AddEmitterHandle(*NewEmitter, FName(*EmitterName), EmitterVersion);

	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("add_emitter â Added emitter '%s' (index %d)"),
		*EmitterName, System->GetEmitterHandles().Num() - 1);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_NiagaraEdit::Operation_RemoveEmitter(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	int32 EmitterIndex = -1;
	if (!Params->TryGetNumberField(TEXT("emitter_index"), EmitterIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: emitter_index"));
	}

	UNiagaraSystem* System = Data->System.Get();
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	if (EmitterIndex < 0 || EmitterIndex >= Handles.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Emitter index %d out of range (0-%d)"),
			EmitterIndex, Handles.Num() - 1));
	}

	FString RemovedName = Handles[EmitterIndex].GetName().ToString();

	FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Remove Niagara Emitter")));
	System->Modify();

	System->RemoveEmitterHandle(Handles[EmitterIndex]);
	System->MarkPackageDirty();

	// Adjust focused emitter index if needed
	if (Data->FocusedEmitterIndex == EmitterIndex)
	{
		Data->FocusedEmitterIndex = -1;
	}
	else if (Data->FocusedEmitterIndex > EmitterIndex)
	{
		Data->FocusedEmitterIndex--;
	}

	Data->LastOperationStatus = FString::Printf(TEXT("remove_emitter â Removed emitter %d (%s)"),
		EmitterIndex, *RemovedName);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_NiagaraEdit::Operation_RenameEmitter(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	int32 EmitterIndex = -1;
	if (!Params->TryGetNumberField(TEXT("emitter_index"), EmitterIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: emitter_index"));
	}

	FString NewName;
	if (!Params->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: new_name"));
	}

	UNiagaraSystem* System = Data->System.Get();
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	if (EmitterIndex < 0 || EmitterIndex >= Handles.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Emitter index %d out of range (0-%d)"),
			EmitterIndex, Handles.Num() - 1));
	}

	FString OldName = Handles[EmitterIndex].GetName().ToString();

	FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Rename Niagara Emitter")));
	System->Modify();

	// GetEmitterHandles() returns const ref; we need mutable access
	TArray<FNiagaraEmitterHandle>& MutableHandles = System->GetEmitterHandles();
	MutableHandles[EmitterIndex].SetName(FName(*NewName), *System);
	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("rename_emitter â Renamed emitter %d from '%s' to '%s'"),
		EmitterIndex, *OldName, *NewName);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_NiagaraEdit::Operation_SetEmitterEnabled(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	int32 EmitterIndex = -1;
	if (!Params->TryGetNumberField(TEXT("emitter_index"), EmitterIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: emitter_index"));
	}

	bool bEnabled = true;
	if (!Params->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		return MakeErrorResult(TEXT("Missing required parameter: enabled"));
	}

	UNiagaraSystem* System = Data->System.Get();
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	if (EmitterIndex < 0 || EmitterIndex >= Handles.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Emitter index %d out of range (0-%d)"),
			EmitterIndex, Handles.Num() - 1));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Set Niagara Emitter Enabled")));
	System->Modify();

	TArray<FNiagaraEmitterHandle>& MutableHandles = System->GetEmitterHandles();
	MutableHandles[EmitterIndex].SetIsEnabled(bEnabled, *System, true);
	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_emitter_enabled â Set emitter %d (%s) enabled=%s"),
		EmitterIndex, *Handles[EmitterIndex].GetName().ToString(), bEnabled ? TEXT("true") : TEXT("false"));
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Renderer Operations
// ============================================================================

FToolResult ClaireonTool_NiagaraEdit::Operation_AddRenderer(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	int32 EmitterIndex = -1;
	if (!Params->TryGetNumberField(TEXT("emitter_index"), EmitterIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: emitter_index"));
	}

	FString RendererType;
	if (!Params->TryGetStringField(TEXT("renderer_type"), RendererType) || RendererType.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: renderer_type"));
	}

	UNiagaraSystem* System = Data->System.Get();
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	if (EmitterIndex < 0 || EmitterIndex >= Handles.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Emitter index %d out of range (0-%d)"),
			EmitterIndex, Handles.Num() - 1));
	}

	FString Error;
	UClass* RendererClass = ClaireonNiagaraHelpers::ResolveRendererClass(RendererType, Error);
	if (!RendererClass)
	{
		return MakeErrorResult(Error);
	}

	const FNiagaraEmitterHandle& Handle = Handles[EmitterIndex];
	FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
	if (!EmitterData)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not get emitter data for emitter %d"), EmitterIndex));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Add Niagara Renderer")));
	System->Modify();

	UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter;
	UNiagaraRendererProperties* NewRenderer = NewObject<UNiagaraRendererProperties>(Emitter, RendererClass);
	Emitter->AddRenderer(NewRenderer, Handle.GetInstance().Version);
	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("add_renderer â Added %s to emitter %d (%s)"),
		*RendererType, EmitterIndex, *Handle.GetName().ToString());
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_NiagaraEdit::Operation_RemoveRenderer(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	int32 EmitterIndex = -1;
	if (!Params->TryGetNumberField(TEXT("emitter_index"), EmitterIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: emitter_index"));
	}

	int32 RendererIndex = -1;
	if (!Params->TryGetNumberField(TEXT("renderer_index"), RendererIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: renderer_index"));
	}

	UNiagaraSystem* System = Data->System.Get();
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	if (EmitterIndex < 0 || EmitterIndex >= Handles.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Emitter index %d out of range (0-%d)"),
			EmitterIndex, Handles.Num() - 1));
	}

	const FNiagaraEmitterHandle& Handle = Handles[EmitterIndex];
	FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
	if (!EmitterData)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not get emitter data for emitter %d"), EmitterIndex));
	}

	const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
	if (RendererIndex < 0 || RendererIndex >= Renderers.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Renderer index %d out of range (0-%d)"),
			RendererIndex, Renderers.Num() - 1));
	}

	UNiagaraRendererProperties* RendererToRemove = Renderers[RendererIndex];
	FString RendererName = ClaireonNiagaraHelpers::GetRendererTypeName(RendererToRemove);

	FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Remove Niagara Renderer")));
	System->Modify();

	Handle.GetInstance().Emitter->RemoveRenderer(RendererToRemove, Handle.GetInstance().Version);
	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("remove_renderer â Removed %s (index %d) from emitter %d"),
		*RendererName, RendererIndex, EmitterIndex);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_NiagaraEdit::Operation_SetRendererProperty(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	int32 EmitterIndex = -1;
	if (!Params->TryGetNumberField(TEXT("emitter_index"), EmitterIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: emitter_index"));
	}

	int32 RendererIndex = -1;
	if (!Params->TryGetNumberField(TEXT("renderer_index"), RendererIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: renderer_index"));
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

	UNiagaraSystem* System = Data->System.Get();
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	if (EmitterIndex < 0 || EmitterIndex >= Handles.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Emitter index %d out of range (0-%d)"),
			EmitterIndex, Handles.Num() - 1));
	}

	const FNiagaraEmitterHandle& Handle = Handles[EmitterIndex];
	FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
	if (!EmitterData)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not get emitter data for emitter %d"), EmitterIndex));
	}

	const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
	if (RendererIndex < 0 || RendererIndex >= Renderers.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Renderer index %d out of range (0-%d)"),
			RendererIndex, Renderers.Num() - 1));
	}

	UNiagaraRendererProperties* Renderer = Renderers[RendererIndex];

	FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Set Niagara Renderer Property")));
	System->Modify();

	FString Error;
	if (!ClaireonNiagaraHelpers::SetObjectProperty(Renderer, PropertyName, Value, Error))
	{
		return MakeErrorResult(Error);
	}

	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_renderer_property â Set '%s' = '%s' on renderer %d of emitter %d"),
		*PropertyName, *Value, RendererIndex, EmitterIndex);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Property Operations
// ============================================================================

FToolResult ClaireonTool_NiagaraEdit::Operation_SetEmitterProperty(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	int32 EmitterIndex = -1;
	if (!Params->TryGetNumberField(TEXT("emitter_index"), EmitterIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: emitter_index"));
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

	UNiagaraSystem* System = Data->System.Get();
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	if (EmitterIndex < 0 || EmitterIndex >= Handles.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Emitter index %d out of range (0-%d)"),
			EmitterIndex, Handles.Num() - 1));
	}

	const FNiagaraEmitterHandle& Handle = Handles[EmitterIndex];
	UNiagaraEmitter* Emitter = Handle.GetInstance().Emitter;
	if (!Emitter)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not get emitter instance for emitter %d"), EmitterIndex));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("MCP: Set Niagara Emitter Property")));
	System->Modify();

	FString Error;
	if (!ClaireonNiagaraHelpers::SetObjectProperty(Emitter, PropertyName, Value, Error))
	{
		return MakeErrorResult(Error);
	}

	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_emitter_property â Set '%s' = '%s' on emitter %d (%s)"),
		*PropertyName, *Value, EmitterIndex, *Handle.GetName().ToString());
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Build Operations
// ============================================================================

FToolResult ClaireonTool_NiagaraEdit::Operation_Save(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	UNiagaraSystem* System = Data->System.Get();
	UPackage* Package = System->GetPackage();
	Package->SetDirtyFlag(true);

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	bool bSuccess = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);

	if (bSuccess)
	{
		Data->LastOperationStatus = FString::Printf(TEXT("save â Saved %s"), *System->GetPathName());
		return MakeErrorResult(FString::Printf(TEXT("Saved: %s"), *System->GetPathName()));
	}
	else
	{
		Data->LastOperationStatus = TEXT("save â Failed");
		return MakeErrorResult(TEXT("Failed to save Niagara System package"));
	}
}
