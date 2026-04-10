// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_NiagaraEdit.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "ClaireonLog.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSafeExec.h"
#include "ClaireonSessionManager.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraSpriteRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraNode.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraScriptSource.h"
#include "NiagaraGraph.h"
#include "NiagaraParameterMapHistory.h"
#include "EdGraphSchema_Niagara.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "NiagaraDataInterfaceCurve.h"
#include "NiagaraDataInterfaceColorCurve.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraEditorModule.h"
#include "NiagaraEditorSettings.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraNodeInput.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "FileHelpers.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ============================================================================
// File-local helpers for graph traversal (replacing non-exported engine functions)
// ============================================================================

/**
 * Local reimplementation of FNiagaraStackGraphUtilities::GetStackFunctionInputs
 * which is not exported from NiagaraEditor in UE 5.5. Enumerates module-level input
 * variables (Module.* pins) on a function call node using only exported graph APIs.
 */
static void GetModuleInputVariables(UNiagaraNodeFunctionCall& ModuleNode, TArray<FNiagaraVariable>& OutInputVars)
{
	FPinCollectorArray InputPins;
	ModuleNode.GetInputPins(InputPins);
	for (UEdGraphPin* Pin : InputPins)
	{
		if (!Pin)
			continue;
		const FString PinName = Pin->PinName.ToString();
		if (!PinName.StartsWith(TEXT("Module.")))
			continue;
		FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
		if (!PinType.IsValid())
			continue;
		OutInputVars.Emplace(PinType, *PinName);
	}
}

/** Find the ParameterMap input pin on a Niagara node by checking pin types. */
static UEdGraphPin* FindParameterMapInputPin(UNiagaraNode& Node)
{
	FPinCollectorArray InputPins;
	Node.GetInputPins(InputPins);
	for (UEdGraphPin* Pin : InputPins)
	{
		FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
		if (PinType == FNiagaraTypeDefinition::GetParameterMapDef())
		{
			return Pin;
		}
	}
	return nullptr;
}

/** Find the ParameterMap output pin on a Niagara node by checking pin types. */
static UEdGraphPin* FindParameterMapOutputPin(UNiagaraNode& Node)
{
	FPinCollectorArray OutputPins;
	Node.GetOutputPins(OutputPins);
	for (UEdGraphPin* Pin : OutputPins)
	{
		FNiagaraTypeDefinition PinType = UEdGraphSchema_Niagara::PinToTypeDefinition(Pin);
		if (PinType == FNiagaraTypeDefinition::GetParameterMapDef())
		{
			return Pin;
		}
	}
	return nullptr;
}

/**
 * Local reimplementation of FNiagaraStackGraphUtilities::GetStackFunctionInputOverridePin.
 * Finds an existing override pin for a module input (read-only lookup).
 */
static UEdGraphPin* FindStackFunctionInputOverridePin(UNiagaraNodeFunctionCall& StackFunctionCall, FNiagaraParameterHandle AliasedInputParameterHandle)
{
	// Find the ParameterMapSet node connected to the function call's parameter map input
	UEdGraphPin* FuncInputPin = FindParameterMapInputPin(StackFunctionCall);
	if (FuncInputPin && FuncInputPin->LinkedTo.Num() == 1)
	{
		UEdGraphNode* OverrideNode = FuncInputPin->LinkedTo[0]->GetOwningNode();
		if (OverrideNode)
		{
			// Search the override node's input pins for one matching the aliased handle name
			for (UEdGraphPin* Pin : OverrideNode->Pins)
			{
				if (Pin->Direction == EGPD_Input && Pin->PinName == AliasedInputParameterHandle.GetParameterHandleString())
				{
					return Pin;
				}
			}
		}
	}
	return nullptr;
}

// Find a static switch input pin on a module node by name.
// Reimplements UNiagaraNodeFunctionCall::FindStaticSwitchInputPin (not exported).
static UEdGraphPin* FindStaticSwitchPin(UNiagaraNodeFunctionCall* ModuleNode, const FName& VariableName)
{
	UNiagaraGraph* CalledGraph = ModuleNode->GetCalledGraph();
	if (!CalledGraph)
	{
		return nullptr;
	}

	FPinCollectorArray InputPins;
	ModuleNode->GetInputPins(InputPins);
	for (const FNiagaraVariable& SwitchVar : CalledGraph->FindStaticSwitchInputs())
	{
		if (SwitchVar.GetName().IsEqual(VariableName))
		{
			for (UEdGraphPin* Pin : InputPins)
			{
				if (VariableName.IsEqual(Pin->GetFName()))
				{
					return Pin;
				}
			}
		}
	}
	return nullptr;
}

// Ensure an emitter's script graph has a valid Input→Output chain for the given usage.
// This reimplements FNiagaraStackGraphUtilities::ResetGraphForOutput (which is not exported).
// Without this, emitters added via AddEmitterToSystem may have broken graph chains that crash
// the Niagara editor UI and produce no compiled bytecode (no particles).
static void EnsureGraphInputOutputChain(UNiagaraGraph* Graph, ENiagaraScriptUsage Usage, FGuid UsageId)
{
	if (!Graph)
	{
		return;
	}

	// Find the output node for this usage
	UNiagaraNodeOutput* OutputNode = Graph->FindEquivalentOutputNode(Usage, UsageId);
	if (!OutputNode)
	{
		return; // No output node at all — can't fix
	}

	// Check if the output node already has a valid input chain
	UEdGraphPin* OutputInputPin = FindParameterMapInputPin(*OutputNode);
	if (!OutputInputPin)
	{
		return; // Output node has no ParameterMap input pin — structural problem
	}

	// Walk backward to see if we can reach an input node
	UNiagaraNode* CurrentNode = OutputNode;
	bool bFoundInputNode = false;
	while (CurrentNode)
	{
		UEdGraphPin* InputPin = FindParameterMapInputPin(*CurrentNode);
		if (InputPin && InputPin->LinkedTo.Num() == 1)
		{
			UNiagaraNode* PrevNode = Cast<UNiagaraNode>(InputPin->LinkedTo[0]->GetOwningNode());
			if (Cast<UNiagaraNodeInput>(PrevNode))
			{
				bFoundInputNode = true;
				break;
			}
			CurrentNode = PrevNode;
		}
		else
		{
			break;
		}
	}

	if (bFoundInputNode)
	{
		return; // Chain is valid, nothing to fix
	}

	// Chain is broken — create a new input node and link it to the output node's input
	Graph->Modify();

	FGraphNodeCreator<UNiagaraNodeInput> InputNodeCreator(*Graph);
	UNiagaraNodeInput* InputNode = InputNodeCreator.CreateNode();
	InputNode->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
	InputNode->Usage = ENiagaraInputNodeUsage::Parameter;
	InputNodeCreator.Finalize();

	UEdGraphPin* InputNodeOutputPin = FindParameterMapOutputPin(*InputNode);
	if (InputNodeOutputPin && OutputInputPin)
	{
		// Break any existing (broken) connections on the output node's input pin
		OutputInputPin->BreakAllPinLinks();
		// Link: InputNode output → OutputNode input
		OutputInputPin->MakeLinkTo(InputNodeOutputPin);
	}
}

// Validate all emitter stack graphs for a newly added emitter
static void EnsureEmitterGraphChains(UNiagaraSystem* System, int32 EmitterIndex)
{
	if (!System)
	{
		return;
	}

	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	if (EmitterIndex < 0 || EmitterIndex >= Handles.Num())
	{
		return;
	}

	FVersionedNiagaraEmitterData* EmitterData = Handles[EmitterIndex].GetEmitterData();
	if (!EmitterData)
	{
		return;
	}

	UNiagaraScriptSource* ScriptSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
	if (!ScriptSource || !ScriptSource->NodeGraph)
	{
		return;
	}

	UNiagaraGraph* Graph = ScriptSource->NodeGraph;

	// Ensure chains for all 4 standard stacks
	EnsureGraphInputOutputChain(Graph, ENiagaraScriptUsage::EmitterSpawnScript, EmitterData->EmitterSpawnScriptProps.Script->GetUsageId());
	EnsureGraphInputOutputChain(Graph, ENiagaraScriptUsage::EmitterUpdateScript, EmitterData->EmitterUpdateScriptProps.Script->GetUsageId());
	EnsureGraphInputOutputChain(Graph, ENiagaraScriptUsage::ParticleSpawnScript, EmitterData->SpawnScriptProps.Script->GetUsageId());
	EnsureGraphInputOutputChain(Graph, ENiagaraScriptUsage::ParticleUpdateScript, EmitterData->UpdateScriptProps.Script->GetUsageId());
}

using FToolResult = IClaireonTool::FToolResult;

// Static tool data storage
TMap<FString, FNiagaraEditToolData> ClaireonTool_NiagaraEdit::ToolData;
bool ClaireonTool_NiagaraEdit::bDelegateRegistered = false;

// ============================================================================
// Session Management
// ============================================================================

void ClaireonTool_NiagaraEdit::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == TEXT("claireon.niagara_edit"))
	{
		ToolData.Remove(Info.SessionId);
	}
}

// ============================================================================
// Tool Interface
// ============================================================================

FString ClaireonTool_NiagaraEdit::GetName() const
{
	return TEXT("claireon.niagara_edit");
}

FString ClaireonTool_NiagaraEdit::GetDescription() const
{
	return TEXT("Session-based Niagara System editor. Manage emitters, renderers, and properties. Start with 'open', configure, then 'save'.");
}

FString ClaireonTool_NiagaraEdit::GetFullDescription() const
{
	return TEXT("Session-based Niagara System editor. Supports emitter management, renderer management, "
				"module management, module input editing (including curves), property editing, "
				"system creation, parameter management, compilation, and saving.\n\n"
				"Session-less operations: open, create, list_modules\n"
				"Session operations: close, status, focus_emitter\n"
				"Emitter operations: add_emitter, remove_emitter, rename_emitter, set_emitter_enabled\n"
				"Renderer operations: add_renderer, remove_renderer, set_renderer_property\n"
				"Module operations: add_module, remove_module\n"
				"Module input operations: get_module_inputs, set_module_input\n"
				"Property operations: set_emitter_property, set_system_property\n"
				"Parameter operations: add_parameter, remove_parameter, set_parameter_value\n"
				"Build operations: compile, save");
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
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("list_modules")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("add_module")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("remove_module")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("get_module_inputs")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_module_input")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("create")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_system_property")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("add_parameter")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("remove_parameter")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_parameter_value")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("compile")));
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
	if (Operation == TEXT("create"))
	{
		TSharedPtr<FJsonObject> Params = Arguments->HasField(TEXT("params"))
			? Arguments->GetObjectField(TEXT("params"))
			: MakeShared<FJsonObject>();
		return Operation_Create(Params);
	}
	if (Operation == TEXT("list_modules"))
	{
		TSharedPtr<FJsonObject> Params = Arguments->HasField(TEXT("params"))
			? Arguments->GetObjectField(TEXT("params"))
			: MakeShared<FJsonObject>();
		return Operation_ListModules(Params);
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
	if (Operation == TEXT("add_module"))
	{
		return Operation_AddModule(SessionId, Data, Params);
	}
	if (Operation == TEXT("remove_module"))
	{
		return Operation_RemoveModule(SessionId, Data, Params);
	}
	if (Operation == TEXT("get_module_inputs"))
	{
		return Operation_GetModuleInputs(SessionId, Data, Params);
	}
	if (Operation == TEXT("set_module_input"))
	{
		return Operation_SetModuleInput(SessionId, Data, Params);
	}
	if (Operation == TEXT("set_system_property"))
	{
		return Operation_SetSystemProperty(SessionId, Data, Params);
	}
	if (Operation == TEXT("add_parameter"))
	{
		return Operation_AddParameter(SessionId, Data, Params);
	}
	if (Operation == TEXT("remove_parameter"))
	{
		return Operation_RemoveParameter(SessionId, Data, Params);
	}
	if (Operation == TEXT("set_parameter_value"))
	{
		return Operation_SetParameterValue(SessionId, Data, Params);
	}
	if (Operation == TEXT("compile"))
	{
		return Operation_Compile(SessionId, Data, Params);
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

	// Resolve path to canonical form
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

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
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, TEXT("claireon.niagara_edit"));
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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Niagara Emitter")));
	System->Modify();

	// Load the default empty emitter template from editor settings
	UNiagaraEmitter* TemplateEmitter = nullptr;
	FSoftObjectPath DefaultEmptyPath = GetDefault<UNiagaraEditorSettings>()->DefaultEmptyEmitter;
	if (DefaultEmptyPath.IsValid())
	{
		TemplateEmitter = Cast<UNiagaraEmitter>(DefaultEmptyPath.TryLoad());
	}
	if (!TemplateEmitter)
	{
		return MakeErrorResult(TEXT("Could not load default empty emitter template. Check NiagaraEditorSettings.DefaultEmptyEmitter"));
	}

	// Use the engine's official AddEmitterToSystem which properly rebuilds emitter nodes
	// and synchronizes the overview graph. Calling AddEmitterHandle directly leaves the
	// graph in a state that crashes the Niagara editor UI's stack view traversal.
	FGuid NewHandleId = FNiagaraEditorUtilities::AddEmitterToSystem(*System, *TemplateEmitter, TemplateEmitter->GetExposedVersion().VersionGuid);

	// Ensure the duplicated emitter's script graphs have valid Input→Output chains.
	// CreateWithParentAndOwner (called internally) may produce graphs with broken chains.
	int32 NewEmitterIndex = System->GetEmitterHandles().Num() - 1;
	EnsureEmitterGraphChains(System, NewEmitterIndex);

	// Rename the newly added emitter to the requested name
	if (NewEmitterIndex >= 0)
	{
		System->GetEmitterHandles()[NewEmitterIndex].SetName(FName(*EmitterName), *System);
	}

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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Niagara Emitter")));
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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Rename Niagara Emitter")));
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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Niagara Emitter Enabled")));
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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Niagara Renderer")));
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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Niagara Renderer")));
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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Niagara Renderer Property")));
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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Niagara Emitter Property")));
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
// Module Operations
// ============================================================================

FToolResult ClaireonTool_NiagaraEdit::Operation_ListModules(const TSharedPtr<FJsonObject>& Params)
{
	FString Query;
	Params->TryGetStringField(TEXT("query"), Query);

	FString Stack;
	Params->TryGetStringField(TEXT("stack"), Stack);

	int32 MaxResults = 20;
	Params->TryGetNumberField(TEXT("max_results"), MaxResults);
	if (MaxResults <= 0)
	{
		MaxResults = 20;
	}

	// Validate stack filter if provided (reserved for future filtering by compatible stack)
	if (!Stack.IsEmpty())
	{
		ENiagaraScriptUsage StackUsage;
		FString StackError;
		if (!ClaireonNiagaraHelpers::ResolveStackName(Stack, StackUsage, StackError))
		{
			return MakeErrorResult(StackError);
		}
	}

	// Query asset registry for all UNiagaraScript assets
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UNiagaraScript::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	// Build result list
	TArray<TSharedPtr<FJsonValue>> ResultArray;

	for (const FAssetData& Asset : AssetList)
	{
		if (ResultArray.Num() >= MaxResults)
		{
			break;
		}

		// Filter by module usage via asset registry tag
		// UNiagaraScript::Usage is AssetRegistrySearchable
		FString UsageStr;
		bool bIsModule = false;
		if (Asset.GetTagValue(GET_MEMBER_NAME_CHECKED(UNiagaraScript, Usage), UsageStr))
		{
			// Tag value is the enum name (e.g. "Module") or integer
			bIsModule = UsageStr == TEXT("Module") || UsageStr == TEXT("ENiagaraScriptUsage::Module")
				|| UsageStr == FString::FromInt(static_cast<int32>(ENiagaraScriptUsage::Module));
		}

		if (!bIsModule)
		{
			continue;
		}

		const FString AssetName = Asset.AssetName.ToString();

		// Apply query filter
		if (!Query.IsEmpty() && !AssetName.Contains(Query, ESearchCase::IgnoreCase))
		{
			continue;
		}

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), AssetName);
		Entry->SetStringField(TEXT("asset_path"), Asset.GetObjectPathString());
		ResultArray.Add(MakeShared<FJsonValueObject>(Entry));
	}

	// Format as text output
	FString Output;
	Output += FString::Printf(TEXT("=== Available Modules (%d results) ===\n"), ResultArray.Num());
	for (int32 i = 0; i < ResultArray.Num(); ++i)
	{
		const TSharedPtr<FJsonObject>& Entry = ResultArray[i]->AsObject();
		Output += FString::Printf(TEXT("  [%d] %s\n       %s\n"),
			i,
			*Entry->GetStringField(TEXT("name")),
			*Entry->GetStringField(TEXT("asset_path")));
	}

	if (ResultArray.Num() == 0)
	{
		Output += TEXT("  (no modules found matching query)\n");
	}

	return MakeErrorResult(Output);
}

FToolResult ClaireonTool_NiagaraEdit::Operation_AddModule(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	int32 EmitterIndex = -1;
	if (!Params->TryGetNumberField(TEXT("emitter_index"), EmitterIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: emitter_index"));
	}

	FString StackName;
	if (!Params->TryGetStringField(TEXT("stack"), StackName) || StackName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: stack"));
	}

	FString ModuleName;
	if (!Params->TryGetStringField(TEXT("module"), ModuleName) || ModuleName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: module"));
	}

	int32 Index = INDEX_NONE;
	Params->TryGetNumberField(TEXT("index"), Index);

	UNiagaraSystem* System = Data->System.Get();

	// Resolve stack name
	ENiagaraScriptUsage Usage;
	FString Error;
	if (!ClaireonNiagaraHelpers::ResolveStackName(StackName, Usage, Error))
	{
		return MakeErrorResult(Error);
	}

	// Resolve module script
	UNiagaraScript* Script = ClaireonNiagaraHelpers::ResolveModuleScript(ModuleName, Error);
	if (!Script)
	{
		return MakeErrorResult(Error);
	}

	// Get the stack output node
	UNiagaraNodeOutput* OutputNode = ClaireonNiagaraHelpers::GetStackOutputNode(System, EmitterIndex, Usage, Error);
	if (!OutputNode)
	{
		return MakeErrorResult(Error);
	}

	// Add the module
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Niagara Module")));
	System->Modify();

	UNiagaraNodeFunctionCall* NewModuleNode = FNiagaraStackGraphUtilities::AddScriptModuleToStack(Script, *OutputNode, Index);

	System->MarkPackageDirty();

	if (!NewModuleNode)
	{
		return MakeErrorResult(TEXT("AddScriptModuleToStack returned null — module could not be added"));
	}

	// Count position by re-querying ordered modules
	TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
	ClaireonNiagaraHelpers::GetOrderedModuleNodes(System, EmitterIndex, Usage, ModuleNodes, Error);

	int32 NewIndex = ModuleNodes.IndexOfByKey(NewModuleNode);

	Data->LastOperationStatus = FString::Printf(TEXT("add_module → Added '%s' to %s stack at index %d"),
		*NewModuleNode->GetFunctionName(), *StackName, NewIndex);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_NiagaraEdit::Operation_RemoveModule(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	int32 EmitterIndex = -1;
	if (!Params->TryGetNumberField(TEXT("emitter_index"), EmitterIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: emitter_index"));
	}

	FString StackName;
	if (!Params->TryGetStringField(TEXT("stack"), StackName) || StackName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: stack"));
	}

	int32 ModuleIndex = -1;
	if (!Params->TryGetNumberField(TEXT("module_index"), ModuleIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: module_index"));
	}

	UNiagaraSystem* System = Data->System.Get();

	// Resolve stack name
	ENiagaraScriptUsage Usage;
	FString Error;
	if (!ClaireonNiagaraHelpers::ResolveStackName(StackName, Usage, Error))
	{
		return MakeErrorResult(Error);
	}

	// Get ordered module nodes
	TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
	if (!ClaireonNiagaraHelpers::GetOrderedModuleNodes(System, EmitterIndex, Usage, ModuleNodes, Error))
	{
		return MakeErrorResult(Error);
	}

	if (ModuleIndex < 0 || ModuleIndex >= ModuleNodes.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Module index %d out of range. Stack '%s' has %d modules (0-%d)"),
			ModuleIndex, *StackName, ModuleNodes.Num(), ModuleNodes.Num() - 1));
	}

	FString RemovedName = ModuleNodes[ModuleIndex]->GetFunctionName();

	// Get emitter GUID
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	if (EmitterIndex < 0 || EmitterIndex >= Handles.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Emitter index %d out of range (0-%d)"),
			EmitterIndex, Handles.Num() - 1));
	}
	FGuid EmitterId = Handles[EmitterIndex].GetId();

	// Remove the module by reconnecting the ParameterMap chain and destroying the node.
	// This reimplements FNiagaraStackGraphUtilities::RemoveModuleFromStack which is not exported.
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Niagara Module")));
	System->Modify();

	UNiagaraNodeFunctionCall* ModuleToRemove = ModuleNodes[ModuleIndex];
	UEdGraphPin* ModuleInputPin = FindParameterMapInputPin(*ModuleToRemove);
	UEdGraphPin* ModuleOutputPin = FindParameterMapOutputPin(*ModuleToRemove);

	// Find the upstream and downstream connections
	UEdGraphPin* UpstreamOutputPin = nullptr;
	UEdGraphNode* OverrideNode = nullptr;
	if (ModuleInputPin && ModuleInputPin->LinkedTo.Num() == 1)
	{
		UEdGraphPin* LinkedPin = ModuleInputPin->LinkedTo[0];
		UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
		// The linked node is likely a ParameterMapSet (override) node
		// Check if it has its own upstream connection
		UNiagaraNode* LinkedNiagaraNode = Cast<UNiagaraNode>(LinkedNode);
		if (LinkedNiagaraNode)
		{
			UEdGraphPin* OverrideInputPin = FindParameterMapInputPin(*LinkedNiagaraNode);
			if (OverrideInputPin && OverrideInputPin->LinkedTo.Num() == 1)
			{
				// This override node sits between the previous module and this one
				UpstreamOutputPin = OverrideInputPin->LinkedTo[0];
				OverrideNode = LinkedNode;
			}
			else
			{
				// No further upstream — the linked node is the direct predecessor
				UpstreamOutputPin = LinkedPin;
			}
		}
	}

	UEdGraphPin* DownstreamInputPin = nullptr;
	if (ModuleOutputPin && ModuleOutputPin->LinkedTo.Num() == 1)
	{
		DownstreamInputPin = ModuleOutputPin->LinkedTo[0];
	}

	// Break all connections on the module node
	ModuleToRemove->BreakAllNodeLinks();

	// Break all connections on the override node if present
	if (OverrideNode)
	{
		OverrideNode->BreakAllNodeLinks();
	}

	// Reconnect upstream to downstream
	if (UpstreamOutputPin && DownstreamInputPin)
	{
		UpstreamOutputPin->MakeLinkTo(DownstreamInputPin);
	}

	// Destroy the nodes
	if (OverrideNode)
	{
		OverrideNode->DestroyNode();
	}
	ModuleToRemove->DestroyNode();

	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("remove_module → Removed '%s' (index %d) from %s stack"),
		*RemovedName, ModuleIndex, *StackName);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Module Input Operations
// ============================================================================

FToolResult ClaireonTool_NiagaraEdit::Operation_GetModuleInputs(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	int32 EmitterIndex = -1;
	if (!Params->TryGetNumberField(TEXT("emitter_index"), EmitterIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: emitter_index"));
	}

	FString StackName;
	if (!Params->TryGetStringField(TEXT("stack"), StackName) || StackName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: stack"));
	}

	int32 ModuleIndex = -1;
	if (!Params->TryGetNumberField(TEXT("module_index"), ModuleIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: module_index"));
	}

	UNiagaraSystem* System = Data->System.Get();

	// Resolve stack name
	ENiagaraScriptUsage Usage;
	FString Error;
	if (!ClaireonNiagaraHelpers::ResolveStackName(StackName, Usage, Error))
	{
		return MakeErrorResult(Error);
	}

	// Get ordered module nodes
	TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
	if (!ClaireonNiagaraHelpers::GetOrderedModuleNodes(System, EmitterIndex, Usage, ModuleNodes, Error))
	{
		return MakeErrorResult(Error);
	}

	if (ModuleIndex < 0 || ModuleIndex >= ModuleNodes.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Module index %d out of range. Stack '%s' has %d modules (0-%d)"),
			ModuleIndex, *StackName, ModuleNodes.Num(), ModuleNodes.Num() - 1));
	}

	UNiagaraNodeFunctionCall* ModuleNode = ModuleNodes[ModuleIndex];

	// Get all module inputs
	TArray<FNiagaraVariable> InputVars;
	GetModuleInputVariables(*ModuleNode, InputVars);

	// Build output
	FString Output;
	Output += FString::Printf(TEXT("=== Module Inputs: %s ===\n"), *ModuleNode->GetFunctionName());
	Output += FString::Printf(TEXT("Stack: %s, Module Index: %d\n\n"), *StackName, ModuleIndex);

	if (InputVars.Num() == 0)
	{
		Output += TEXT("  (no inputs)\n");
	}

	for (int32 i = 0; i < InputVars.Num(); ++i)
	{
		const FNiagaraVariable& Var = InputVars[i];
		const FString VarName = Var.GetName().ToString();
		const FString TypeName = Var.GetType().GetName();

		// Build the aliased parameter handle for this input.
		// Strip "Module." prefix to avoid double-prefixing (same fix as set_module_input).
		FString RawName = Var.GetName().ToString();
		if (RawName.StartsWith(TEXT("Module.")))
		{
			RawName = RawName.Mid(7);
		}
		FNiagaraParameterHandle ModuleHandle = FNiagaraParameterHandle::CreateModuleParameterHandle(FName(*RawName));
		FNiagaraParameterHandle AliasedHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleHandle, ModuleNode);

		UEdGraphPin* OverridePin = FindStackFunctionInputOverridePin(*ModuleNode, AliasedHandle);

		bool bIsOverridden = (OverridePin != nullptr);
		FString ValueStr = TEXT("(default)");

		if (bIsOverridden && OverridePin)
		{
			ValueStr = OverridePin->DefaultValue;
			if (ValueStr.IsEmpty())
			{
				ValueStr = TEXT("(empty)");
			}
		}

		Output += FString::Printf(TEXT("  [%d] %s : %s = %s%s\n"),
			i, *VarName, *TypeName, *ValueStr,
			bIsOverridden ? TEXT(" (overridden)") : TEXT(""));
	}

	// Also list static switch inputs
	UNiagaraGraph* CalledGraph = ModuleNode->GetCalledGraph();
	if (CalledGraph)
	{
		TArray<FNiagaraVariable> SwitchVars = CalledGraph->FindStaticSwitchInputs();
		if (SwitchVars.Num() > 0)
		{
			Output += TEXT("\n  --- Static Switches ---\n");
			for (int32 i = 0; i < SwitchVars.Num(); ++i)
			{
				const FNiagaraVariable& SwitchVar = SwitchVars[i];
				UEdGraphPin* SwitchPin = FindStaticSwitchPin(ModuleNode, SwitchVar.GetName());
				FString SwitchValue = SwitchPin ? SwitchPin->DefaultValue : TEXT("(not set)");

				// Resolve enum display name for the current value
				FString DisplayValue = SwitchValue;
				UEnum* SwitchEnum = SwitchVar.GetType().GetEnum();
				if (SwitchEnum)
				{
					// Find the display name for the current value
					for (int32 e = 0; e < SwitchEnum->NumEnums() - 1; ++e) // -1 to skip _MAX
					{
						if (SwitchEnum->GetNameStringByIndex(e) == SwitchValue)
						{
							DisplayValue = SwitchEnum->GetDisplayNameTextByIndex(e).ToString();
							break;
						}
					}
				}

				// List valid values for enum switches
				FString ValidValues;
				if (SwitchEnum)
				{
					for (int32 e = 0; e < SwitchEnum->NumEnums() - 1; ++e)
					{
						if (!ValidValues.IsEmpty())
							ValidValues += TEXT(", ");
						ValidValues += FString::Printf(TEXT("%s (%s)"),
							*SwitchEnum->GetDisplayNameTextByIndex(e).ToString(),
							*SwitchEnum->GetNameStringByIndex(e));
					}
				}

				Output += FString::Printf(TEXT("  [S%d] %s = %s (static switch)%s\n"),
					i, *SwitchVar.GetName().ToString(), *DisplayValue,
					ValidValues.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("\n        Valid: %s"), *ValidValues));
			}
		}
	}

	Data->LastOperationStatus = FString::Printf(TEXT("get_module_inputs → Listed %d inputs for '%s'"),
		InputVars.Num(), *ModuleNode->GetFunctionName());
	return MakeErrorResult(Output);
}

FToolResult ClaireonTool_NiagaraEdit::Operation_SetModuleInput(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	int32 EmitterIndex = -1;
	if (!Params->TryGetNumberField(TEXT("emitter_index"), EmitterIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: emitter_index"));
	}

	FString StackName;
	if (!Params->TryGetStringField(TEXT("stack"), StackName) || StackName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: stack"));
	}

	int32 ModuleIndex = -1;
	if (!Params->TryGetNumberField(TEXT("module_index"), ModuleIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: module_index"));
	}

	FString InputName;
	if (!Params->TryGetStringField(TEXT("input_name"), InputName) || InputName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: input_name"));
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	UNiagaraSystem* System = Data->System.Get();

	// Resolve stack name
	ENiagaraScriptUsage Usage;
	FString Error;
	if (!ClaireonNiagaraHelpers::ResolveStackName(StackName, Usage, Error))
	{
		return MakeErrorResult(Error);
	}

	// Get ordered module nodes
	TArray<UNiagaraNodeFunctionCall*> ModuleNodes;
	if (!ClaireonNiagaraHelpers::GetOrderedModuleNodes(System, EmitterIndex, Usage, ModuleNodes, Error))
	{
		return MakeErrorResult(Error);
	}

	if (ModuleIndex < 0 || ModuleIndex >= ModuleNodes.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Module index %d out of range. Stack '%s' has %d modules (0-%d)"),
			ModuleIndex, *StackName, ModuleNodes.Num(), ModuleNodes.Num() - 1));
	}

	UNiagaraNodeFunctionCall* ModuleNode = ModuleNodes[ModuleIndex];

	// Get all module inputs
	TArray<FNiagaraVariable> InputVars;
	GetModuleInputVariables(*ModuleNode, InputVars);

	// Find matching input by name (case-insensitive)
	const FNiagaraVariable* MatchedVar = nullptr;
	for (const FNiagaraVariable& Var : InputVars)
	{
		if (Var.GetName().ToString().Equals(InputName, ESearchCase::IgnoreCase))
		{
			MatchedVar = &Var;
			break;
		}
	}

	if (!MatchedVar)
	{
		// Not a regular input — check if it's a static switch (e.g., "Color Mode", "Lifetime Mode").
		// Static switches are stored as pins directly on the module function call node.
		// Try the name as-is, then with "Module." stripped.
		FString SwitchName = InputName;
		if (SwitchName.StartsWith(TEXT("Module.")))
		{
			SwitchName = SwitchName.Mid(7);
		}
		UEdGraphPin* SwitchPin = FindStaticSwitchPin(ModuleNode, FName(*SwitchName));
		if (SwitchPin)
		{
			// Resolve the value: accept display names ("DirectSet") or raw ("NewEnumerator0").
			// Look up the static switch variable to get its enum type.
			FString ResolvedValue = Value;
			UNiagaraGraph* SwitchGraph = ModuleNode->GetCalledGraph();
			if (SwitchGraph)
			{
				for (const FNiagaraVariable& SwitchVar : SwitchGraph->FindStaticSwitchInputs())
				{
					if (SwitchVar.GetName().IsEqual(FName(*SwitchName)))
					{
						UEnum* SwitchEnum = SwitchVar.GetType().GetEnum();
						if (SwitchEnum)
						{
							// Try matching by display name (case-insensitive)
							for (int32 e = 0; e < SwitchEnum->NumEnums() - 1; ++e)
							{
								FString DisplayName = SwitchEnum->GetDisplayNameTextByIndex(e).ToString();
								if (DisplayName.Equals(Value, ESearchCase::IgnoreCase))
								{
									ResolvedValue = SwitchEnum->GetNameStringByIndex(e);
									break;
								}
							}
						}
						break;
					}
				}
			}

			FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Niagara Static Switch")));
			System->Modify();
			SwitchPin->Modify();
			SwitchPin->DefaultValue = ResolvedValue;

			// After changing a static switch, the module's pins must be rebuilt
			// and downstream parameter map nodes synchronized. Without this,
			// the compiler sees stale parameter maps and produces errors like
			// "Default found for X, but not found in ParameterMap traversal".
			// RefreshFromExternalChanges internally calls ReallocatePins (protected)
			// and SynchronizeReferencingMapPinsWithFunctionCall.
			ModuleNode->RefreshFromExternalChanges();
			// Also call synchronize directly in case RefreshFromExternalChanges
			// didn't detect a change (cached ID match)
			FNiagaraStackGraphUtilities::SynchronizeReferencingMapPinsWithFunctionCall(*ModuleNode);
			ModuleNode->MarkNodeRequiresSynchronization(TEXT("MCP set static switch"), true);
			System->MarkPackageDirty();

			Data->LastOperationStatus = FString::Printf(TEXT("set_module_input → Set static switch '%s' = '%s' on module '%s'"),
				*SwitchName, *Value, *ModuleNode->GetFunctionName());
			return BuildStateResponse(SessionId, Data);
		}

		// List available inputs including static switches
		FString AvailableInputs;
		for (const FNiagaraVariable& Var : InputVars)
		{
			if (!AvailableInputs.IsEmpty())
			{
				AvailableInputs += TEXT(", ");
			}
			AvailableInputs += Var.GetName().ToString();
		}
		// Also list static switches
		UNiagaraGraph* CalledGraph = ModuleNode->GetCalledGraph();
		if (CalledGraph)
		{
			for (const FNiagaraVariable& SwitchVar : CalledGraph->FindStaticSwitchInputs())
			{
				if (!AvailableInputs.IsEmpty())
				{
					AvailableInputs += TEXT(", ");
				}
				AvailableInputs += SwitchVar.GetName().ToString() + TEXT(" (static switch)");
			}
		}
		return MakeErrorResult(FString::Printf(TEXT("Input '%s' not found on module '%s'. Available inputs: %s"),
			*InputName, *ModuleNode->GetFunctionName(), *AvailableInputs));
	}

	FNiagaraTypeDefinition TypeDef = MatchedVar->GetType();

	// Build the aliased parameter handle for this input.
	// GetStackFunctionInputs returns variables with "Module." prefix (e.g., "Module.Velocity Strength").
	// CreateModuleParameterHandle adds its own "Module." prefix, so we must strip it first to avoid
	// double-prefixing ("Module.Module.Velocity Strength") which creates an override pin the compiler ignores.
	FString RawInputName = MatchedVar->GetName().ToString();
	if (RawInputName.StartsWith(TEXT("Module.")))
	{
		RawInputName = RawInputName.Mid(7); // Strip "Module."
	}
	FNiagaraParameterHandle ModuleHandle = FNiagaraParameterHandle::CreateModuleParameterHandle(FName(*RawInputName));
	FNiagaraParameterHandle AliasedHandle = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(ModuleHandle, ModuleNode);

	// Resolve the ScriptVarId from the module script's graph metadata
	FGuid ScriptVarId;
	UNiagaraScriptSource* FunctionScriptSource = ModuleNode->GetFunctionScriptSource();
	if (FunctionScriptSource && FunctionScriptSource->NodeGraph)
	{
		// Look up the variable in the module script's graph to get its metadata GUID.
		// The graph stores variables with "Module." prefix — MatchedVar already has it.
		FNiagaraVariable LookupVar = *MatchedVar;

		TOptional<FNiagaraVariableMetaData> MetaData = FunctionScriptSource->NodeGraph->GetMetaData(LookupVar);
		if (MetaData.IsSet())
		{
			ScriptVarId = MetaData->GetVariableGuid();
		}
	}

	// Wrap in transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Niagara Module Input")));
	System->Modify();

	// Handle data interface types (curves)
	if (TypeDef.IsDataInterface())
	{
		UEdGraphPin& OverridePin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(*ModuleNode, AliasedHandle, TypeDef, ScriptVarId, FGuid());

		// Check if there's already a DI node linked to this pin.
		// If so, modify its data directly instead of creating a new node.
		// Creating a new node after BreakAllPinLinks leaves the old node orphaned
		// in the graph, which confuses the compiler's parameter map traversal for
		// the Interpolated Spawn Script and silently breaks the emitter in game.
		UNiagaraDataInterface* ExistingDI = nullptr;
		if (OverridePin.LinkedTo.Num() == 1)
		{
			UEdGraphNode* LinkedNode = OverridePin.LinkedTo[0]->GetOwningNode();
			if (UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(LinkedNode))
			{
				// GetDataInterface() is not exported, so access the DataInterface property via reflection
				FProperty* DIProp = FindFProperty<FProperty>(UNiagaraNodeInput::StaticClass(), TEXT("DataInterface"));
				if (DIProp)
				{
					UObject* const* DIPtr = DIProp->ContainerPtrToValuePtr<UObject*>(InputNode);
					if (DIPtr)
					{
						ExistingDI = Cast<UNiagaraDataInterface>(*DIPtr);
					}
				}
			}
		}

		if (Value.StartsWith(TEXT("[")))
		{
			// Float curve: JSON array of {"time": X, "value": Y}
			TArray<TSharedPtr<FJsonValue>> KeyArray;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Value);
			if (!FJsonSerializer::Deserialize(Reader, KeyArray))
			{
				return MakeErrorResult(TEXT("Failed to parse float curve JSON array. Expected: [{\"time\": 0.0, \"value\": 1.0}, ...]"));
			}

			// Build the curve data
			FRichCurve NewCurve;
			for (const TSharedPtr<FJsonValue>& KeyVal : KeyArray)
			{
				const TSharedPtr<FJsonObject>& KeyObj = KeyVal->AsObject();
				if (KeyObj.IsValid())
				{
					double Time = KeyObj->GetNumberField(TEXT("time"));
					double Val = KeyObj->GetNumberField(TEXT("value"));
					NewCurve.AddKey(static_cast<float>(Time), static_cast<float>(Val));
				}
			}

			// If there's already a curve DI linked, modify it directly
			if (UNiagaraDataInterfaceCurve* ExistingCurve = Cast<UNiagaraDataInterfaceCurve>(ExistingDI))
			{
				ExistingCurve->Modify();
				ExistingCurve->Curve = NewCurve;
			}
			else
			{
				// No existing DI — break any links and create a new one
				if (OverridePin.LinkedTo.Num() > 0)
				{
					OverridePin.BreakAllPinLinks();
				}
				UNiagaraDataInterface* OutDI = nullptr;
				FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(OverridePin, UNiagaraDataInterfaceCurve::StaticClass(), InputName, OutDI, ScriptVarId);
				if (UNiagaraDataInterfaceCurve* TargetCurve = Cast<UNiagaraDataInterfaceCurve>(OutDI))
				{
					TargetCurve->Curve = NewCurve;
				}
			}
		}
		else if (Value.StartsWith(TEXT("{")) && Value.Contains(TEXT("\"r\"")))
		{
			// Color curve: JSON object with r/g/b/a channel arrays
			TSharedPtr<FJsonObject> ColorObj;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Value);
			if (!FJsonSerializer::Deserialize(Reader, ColorObj) || !ColorObj.IsValid())
			{
				return MakeErrorResult(TEXT("Failed to parse color curve JSON. Expected: {\"r\": [{\"time\": 0, \"value\": 1}, ...], \"g\": [...], \"b\": [...], \"a\": [...]}"));
			}

			auto PopulateChannel = [&](const FString& ChannelName, FRichCurve& OutCurve)
			{
				OutCurve.Reset();
				const TArray<TSharedPtr<FJsonValue>>* ChannelArray = nullptr;
				if (ColorObj->TryGetArrayField(ChannelName, ChannelArray))
				{
					for (const TSharedPtr<FJsonValue>& KeyVal : *ChannelArray)
					{
						const TSharedPtr<FJsonObject>& KeyObj = KeyVal->AsObject();
						if (KeyObj.IsValid())
						{
							double Time = KeyObj->GetNumberField(TEXT("time"));
							double Val = KeyObj->GetNumberField(TEXT("value"));
							OutCurve.AddKey(static_cast<float>(Time), static_cast<float>(Val));
						}
					}
				}
			};

			// If there's already a color curve DI linked, modify it directly
			if (UNiagaraDataInterfaceColorCurve* ExistingColorCurve = Cast<UNiagaraDataInterfaceColorCurve>(ExistingDI))
			{
				ExistingColorCurve->Modify();
				PopulateChannel(TEXT("r"), ExistingColorCurve->RedCurve);
				PopulateChannel(TEXT("g"), ExistingColorCurve->GreenCurve);
				PopulateChannel(TEXT("b"), ExistingColorCurve->BlueCurve);
				PopulateChannel(TEXT("a"), ExistingColorCurve->AlphaCurve);
			}
			else
			{
				// No existing DI — break any links and create a new one
				if (OverridePin.LinkedTo.Num() > 0)
				{
					OverridePin.BreakAllPinLinks();
				}
				UNiagaraDataInterface* OutDI = nullptr;
				FNiagaraStackGraphUtilities::SetDataInterfaceValueForFunctionInput(OverridePin, UNiagaraDataInterfaceColorCurve::StaticClass(), InputName, OutDI, ScriptVarId);
				if (UNiagaraDataInterfaceColorCurve* TargetColorCurve = Cast<UNiagaraDataInterfaceColorCurve>(OutDI))
				{
					PopulateChannel(TEXT("r"), TargetColorCurve->RedCurve);
					PopulateChannel(TEXT("g"), TargetColorCurve->GreenCurve);
					PopulateChannel(TEXT("b"), TargetColorCurve->BlueCurve);
					PopulateChannel(TEXT("a"), TargetColorCurve->AlphaCurve);
				}
			}
		}
		else
		{
			return MakeErrorResult(FString::Printf(TEXT("Data interface type '%s' is not supported. For float curves, use JSON array: [{\"time\": 0, \"value\": 1}, ...]. For color curves, use JSON object with r/g/b/a arrays."),
				*TypeDef.GetName()));
		}

		System->MarkPackageDirty();

		Data->LastOperationStatus = FString::Printf(TEXT("set_module_input -> Set curve '%s' on module '%s'"),
			*InputName, *ModuleNode->GetFunctionName());
		return BuildStateResponse(SessionId, Data);
	}

	// Standard (non-DI) input: set pin default value
	UEdGraphPin& Pin = FNiagaraStackGraphUtilities::GetOrCreateStackFunctionInputOverridePin(*ModuleNode, AliasedHandle, TypeDef, ScriptVarId, FGuid());
	Pin.Modify();
	Pin.DefaultValue = Value;

	// Critical: notify the owning node that the graph needs recompilation.
	// Without this, the Niagara VM continues using stale compiled bytecode.
	if (UNiagaraNode* OwningNode = Cast<UNiagaraNode>(Pin.GetOwningNode()))
	{
		OwningNode->MarkNodeRequiresSynchronization(TEXT("MCP set_module_input"), true);
	}

	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_module_input → Set '%s' = '%s' on module '%s'"),
		*InputName, *Value, *ModuleNode->GetFunctionName());
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// System Creation (Stage 006)
// ============================================================================

FToolResult ClaireonTool_NiagaraEdit::Operation_Create(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'create' requires params.asset_path"));
	}

	// Resolve path to canonical form
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

	// UNiagaraSystemFactoryNew is not exported (no NIAGARAEDITOR_API on class) in UE 5.5.
	// Create the system directly and initialize via the exported static InitializeSystem.
	UNiagaraSystem* System = NewObject<UNiagaraSystem>(Package, *FPackageName::GetShortName(AssetPath), RF_Public | RF_Standalone);
	if (!System)
	{
		return MakeErrorResult(TEXT("Failed to create Niagara System"));
	}

	// Initialize with default nodes (SystemState etc.) — required for emitters to resolve dependencies.
	// InitializeSystem is NIAGARAEDITOR_API and available in all supported engine versions.
	UNiagaraSystemFactoryNew::InitializeSystem(System, true);

	// Save package
	Package->SetDirtyFlag(true);
	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	if (!ClaireonSafeExec::DidLastExecutionCrash())
	{
		UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);
	}

	// Register with asset registry
	FAssetRegistryModule::AssetCreated(System);

	// Open edit session using same logic as Operation_Open
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonTool_NiagaraEdit::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	const FString ResolvedAssetPath = System->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, TEXT("claireon.niagara_edit"));
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		return MakeErrorResult(FString::Printf(TEXT("Asset is locked by %s session %s"), *Blocker.ToolName, *Blocker.SessionId));
	}
	const FString SessionId = OpenResult.SessionId;

	FNiagaraEditToolData NewData;
	NewData.System = System;
	NewData.LastOperationStatus = TEXT("System created and session opened");
	ToolData.Add(SessionId, MoveTemp(NewData));

	FString Output;
	Output += FString::Printf(TEXT("=== System Created ===\nSession ID: %s\nAsset: %s\n\n"), *SessionId, *AssetPath);
	Output += ClaireonNiagaraHelpers::FormatNiagaraSystemStructure(System, false);
	return MakeErrorResult(Output);
}

// ============================================================================
// System Properties & Parameters (Stage 007)
// ============================================================================

FToolResult ClaireonTool_NiagaraEdit::Operation_SetSystemProperty(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
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

	UNiagaraSystem* System = Data->System.Get();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Niagara System Property")));
	System->Modify();

	FString Error;
	if (!ClaireonNiagaraHelpers::SetObjectProperty(System, PropertyName, Value, Error))
	{
		return MakeErrorResult(Error);
	}

	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_system_property -> Set '%s' = '%s'"),
		*PropertyName, *Value);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_NiagaraEdit::Operation_AddParameter(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: name"));
	}

	FString TypeStr;
	if (!Params->TryGetStringField(TEXT("type"), TypeStr) || TypeStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: type"));
	}

	// Auto-prefix with "User." if not already
	FString FullName = Name;
	if (!FullName.StartsWith(TEXT("User.")))
	{
		FullName = TEXT("User.") + FullName;
	}

	// Map type string to FNiagaraTypeDefinition
	FNiagaraTypeDefinition TypeDef;
	if (TypeStr.Equals(TEXT("Float"), ESearchCase::IgnoreCase))
	{
		TypeDef = FNiagaraTypeDefinition::GetFloatDef();
	}
	else if (TypeStr.Equals(TEXT("Vector"), ESearchCase::IgnoreCase))
	{
		TypeDef = FNiagaraTypeDefinition::GetVec3Def();
	}
	else if (TypeStr.Equals(TEXT("Color"), ESearchCase::IgnoreCase) || TypeStr.Equals(TEXT("LinearColor"), ESearchCase::IgnoreCase))
	{
		TypeDef = FNiagaraTypeDefinition::GetColorDef();
	}
	else if (TypeStr.Equals(TEXT("Bool"), ESearchCase::IgnoreCase))
	{
		TypeDef = FNiagaraTypeDefinition::GetBoolDef();
	}
	else if (TypeStr.Equals(TEXT("Int"), ESearchCase::IgnoreCase))
	{
		TypeDef = FNiagaraTypeDefinition::GetIntDef();
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Unsupported parameter type: '%s'. Valid types: Float, Vector, Color, LinearColor, Bool, Int"), *TypeStr));
	}

	UNiagaraSystem* System = Data->System.Get();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Niagara Parameter")));
	System->Modify();

	FNiagaraVariable Variable(TypeDef, FName(*FullName));
	System->GetExposedParameters().AddParameter(Variable, true);

	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("add_parameter -> Added '%s' (%s)"),
		*FullName, *TypeStr);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_NiagaraEdit::Operation_RemoveParameter(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: name"));
	}

	// Auto-prefix with "User." if not already
	FString FullName = Name;
	if (!FullName.StartsWith(TEXT("User.")))
	{
		FullName = TEXT("User.") + FullName;
	}

	UNiagaraSystem* System = Data->System.Get();

	// Find the parameter
	TArray<FNiagaraVariable> UserParams;
	System->GetExposedParameters().GetUserParameters(UserParams);

	const FNiagaraVariable* FoundVar = nullptr;
	for (const FNiagaraVariable& Param : UserParams)
	{
		if (Param.GetName().ToString().Equals(FullName, ESearchCase::IgnoreCase))
		{
			FoundVar = &Param;
			break;
		}
	}

	if (!FoundVar)
	{
		return MakeErrorResult(FString::Printf(TEXT("Parameter '%s' not found in exposed parameters"), *FullName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Niagara Parameter")));
	System->Modify();

	System->GetExposedParameters().RemoveParameter(*FoundVar);

	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("remove_parameter -> Removed '%s'"), *FullName);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_NiagaraEdit::Operation_SetParameterValue(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Name;
	if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: name"));
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	// Auto-prefix with "User." if not already
	FString FullName = Name;
	if (!FullName.StartsWith(TEXT("User.")))
	{
		FullName = TEXT("User.") + FullName;
	}

	UNiagaraSystem* System = Data->System.Get();

	// Find the parameter
	TArray<FNiagaraVariable> UserParams;
	System->GetExposedParameters().GetUserParameters(UserParams);

	const FNiagaraVariable* FoundVar = nullptr;
	for (const FNiagaraVariable& Param : UserParams)
	{
		if (Param.GetName().ToString().Equals(FullName, ESearchCase::IgnoreCase))
		{
			FoundVar = &Param;
			break;
		}
	}

	if (!FoundVar)
	{
		return MakeErrorResult(FString::Printf(TEXT("Parameter '%s' not found in exposed parameters"), *FullName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Niagara Parameter Value")));
	System->Modify();

	FNiagaraTypeDefinition TypeDef = FoundVar->GetType();
	FNiagaraVariable MutableVar = *FoundVar;

	// Parse value based on type
	if (TypeDef == FNiagaraTypeDefinition::GetFloatDef())
	{
		float FloatVal = FCString::Atof(*Value);
		MutableVar.SetValue(FloatVal);
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetIntDef())
	{
		int32 IntVal = FCString::Atoi(*Value);
		MutableVar.SetValue(IntVal);
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetBoolDef())
	{
		FNiagaraBool BoolVal;
		BoolVal.SetValue(Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value == TEXT("1"));
		MutableVar.SetValue(BoolVal);
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetVec3Def())
	{
		FVector3f Vec;
		// Parse "(X, Y, Z)" or "X, Y, Z" format
		FString CleanValue = Value;
		CleanValue.ReplaceInline(TEXT("("), TEXT(""));
		CleanValue.ReplaceInline(TEXT(")"), TEXT(""));
		TArray<FString> Components;
		CleanValue.ParseIntoArray(Components, TEXT(","));
		if (Components.Num() >= 3)
		{
			Vec.X = FCString::Atof(*Components[0].TrimStartAndEnd());
			Vec.Y = FCString::Atof(*Components[1].TrimStartAndEnd());
			Vec.Z = FCString::Atof(*Components[2].TrimStartAndEnd());
		}
		MutableVar.SetValue(Vec);
	}
	else if (TypeDef == FNiagaraTypeDefinition::GetColorDef())
	{
		FLinearColor Color;
		// Parse "(R, G, B, A)" or "R, G, B, A" format
		FString CleanValue = Value;
		CleanValue.ReplaceInline(TEXT("("), TEXT(""));
		CleanValue.ReplaceInline(TEXT(")"), TEXT(""));
		CleanValue.ReplaceInline(TEXT("R="), TEXT(""));
		CleanValue.ReplaceInline(TEXT("G="), TEXT(""));
		CleanValue.ReplaceInline(TEXT("B="), TEXT(""));
		CleanValue.ReplaceInline(TEXT("A="), TEXT(""));
		TArray<FString> Components;
		CleanValue.ParseIntoArray(Components, TEXT(","));
		if (Components.Num() >= 3)
		{
			Color.R = FCString::Atof(*Components[0].TrimStartAndEnd());
			Color.G = FCString::Atof(*Components[1].TrimStartAndEnd());
			Color.B = FCString::Atof(*Components[2].TrimStartAndEnd());
			Color.A = Components.Num() >= 4 ? FCString::Atof(*Components[3].TrimStartAndEnd()) : 1.0f;
		}
		MutableVar.SetValue(Color);
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot set value for parameter type: %s"), *TypeDef.GetName()));
	}

	System->GetExposedParameters().SetParameterData(MutableVar.GetData(), MutableVar, true);

	System->MarkPackageDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("set_parameter_value -> Set '%s' = '%s'"),
		*FullName, *Value);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Compile (Stage 008)
// ============================================================================

FToolResult ClaireonTool_NiagaraEdit::Operation_Compile(const FString& SessionId, FNiagaraEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	UNiagaraSystem* System = Data->System.Get();

	// Use UNiagaraSystem::RequestCompile for a simpler synchronous-ish compile
	System->RequestCompile(true);

	// Wait for compilation to finish (poll with timeout)
	const double StartTime = FPlatformTime::Seconds();
	const double TimeoutSeconds = 5.0;
	while (System->HasOutstandingCompilationRequests())
	{
		if (FPlatformTime::Seconds() - StartTime > TimeoutSeconds)
		{
			Data->LastOperationStatus = TEXT("compile -> Timed out after 5s (compilation may still be in progress)");
			return MakeErrorResult(TEXT("Compilation timed out after 5 seconds. The system may still be compiling in the background."));
		}
		FPlatformProcess::Sleep(0.01f);
	}

	// Collect results
	FString Output;
	Output += TEXT("=== Compilation Results ===\n");

	bool bAllSuccess = true;
	TArray<FString> Errors;
	TArray<FString> Warnings;

	// Check emitter compile statuses
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
	for (int32 i = 0; i < Handles.Num(); ++i)
	{
		const FNiagaraEmitterHandle& Handle = Handles[i];
		FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
		if (!EmitterData)
		{
			continue;
		}

		// Check each script's compile status on the emitter
		auto CheckScript = [&](UNiagaraScript* Script, const FString& ScriptLabel)
		{
			if (!Script)
			{
				return;
			}

			ENiagaraScriptCompileStatus Status = Script->GetLastCompileStatus();
			if (Status == ENiagaraScriptCompileStatus::NCS_Dirty)
			{
				Warnings.Add(FString::Printf(TEXT("Emitter %d (%s) %s: needs compile"), i, *Handle.GetName().ToString(), *ScriptLabel));
			}
			else if (Status == ENiagaraScriptCompileStatus::NCS_Error)
			{
				bAllSuccess = false;
				Errors.Add(FString::Printf(TEXT("Emitter %d (%s) %s: compile error"), i, *Handle.GetName().ToString(), *ScriptLabel));
			}
		};

		CheckScript(EmitterData->GetGPUComputeScript(), TEXT("GPU Compute"));
	}

	Output += FString::Printf(TEXT("Success: %s\n"), bAllSuccess ? TEXT("true") : TEXT("false"));
	Output += FString::Printf(TEXT("Errors: %d\n"), Errors.Num());
	for (const FString& Err : Errors)
	{
		Output += FString::Printf(TEXT("  - %s\n"), *Err);
	}
	Output += FString::Printf(TEXT("Warnings: %d\n"), Warnings.Num());
	for (const FString& Warn : Warnings)
	{
		Output += FString::Printf(TEXT("  - %s\n"), *Warn);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("compile -> %s (%d errors, %d warnings)"),
		bAllSuccess ? TEXT("Success") : TEXT("Failed"), Errors.Num(), Warnings.Num());
	return MakeErrorResult(Output);
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
	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		return MakeErrorResult(TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor."));
	}
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
