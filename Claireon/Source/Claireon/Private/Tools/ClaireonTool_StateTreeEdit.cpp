// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_StateTreeEdit.h"
#include "Tools/ClaireonSpecApplicator_StateTree.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "ClaireonLog.h"
#include "ClaireonSafeExec.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeEditorNode.h"
#include "StateTreeNodeBase.h"
#include "StateTreeSchema.h"
#include "StateTreeTypes.h"
#include "StateTreePropertyBindings.h"
#include "StateTreeEditorPropertyBindings.h"
#include "StateTreePropertyFunctionBase.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditingSubsystem.h"
#include "GameplayTagContainer.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "FileHelpers.h"
#include "Editor.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"

// Using statements
using FToolResult = IClaireonTool::FToolResult;

// Static tool data storage
TMap<FString, FStateTreeEditToolData> ClaireonTool_StateTreeEdit::ToolData;
bool ClaireonTool_StateTreeEdit::bDelegateRegistered = false;

// ============================================================================
// Helper: Parse GUID from JSON string
// ============================================================================

namespace
{
	bool ParseGuidParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, FGuid& OutGuid, FString& OutError)
	{
		FString GuidStr;
		if (!Params->TryGetStringField(FieldName, GuidStr) || GuidStr.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Missing required parameter: %s"), *FieldName);
			return false;
		}
		if (!FGuid::Parse(GuidStr, OutGuid))
		{
			OutError = FString::Printf(TEXT("Invalid GUID for %s: %s"), *FieldName, *GuidStr);
			return false;
		}
		return true;
	}

	UStateTreeEditorData* GetEditorDataFromSession(FStateTreeEditToolData* ToolData, FString& OutError)
	{
		if (!ToolData || !ToolData->IsValid())
		{
			OutError = TEXT("Session is invalid");
			return nullptr;
		}
		return ClaireonStateTreeHelpers::GetEditorData(ToolData->StateTree.Get(), OutError);
	}

	EStateTreeStateType ParseStateType(const FString& TypeStr)
	{
		if (TypeStr == TEXT("Group"))
			return EStateTreeStateType::Group;
		if (TypeStr == TEXT("Linked"))
			return EStateTreeStateType::Linked;
		if (TypeStr == TEXT("LinkedAsset"))
			return EStateTreeStateType::LinkedAsset;
		if (TypeStr == TEXT("Subtree"))
			return EStateTreeStateType::Subtree;
		return EStateTreeStateType::State;
	}

	EStateTreeStateSelectionBehavior ParseSelectionBehavior(const FString& BehaviorStr)
	{
		if (BehaviorStr == TEXT("None"))
			return EStateTreeStateSelectionBehavior::None;
		if (BehaviorStr == TEXT("TryEnterState"))
			return EStateTreeStateSelectionBehavior::TryEnterState;
		if (BehaviorStr == TEXT("TrySelectChildrenAtRandom"))
			return EStateTreeStateSelectionBehavior::TrySelectChildrenAtRandom;
		if (BehaviorStr == TEXT("TrySelectChildrenWithHighestUtility"))
			return EStateTreeStateSelectionBehavior::TrySelectChildrenWithHighestUtility;
		if (BehaviorStr == TEXT("TrySelectChildrenAtRandomWeightedByUtility"))
			return EStateTreeStateSelectionBehavior::TrySelectChildrenAtRandomWeightedByUtility;
		if (BehaviorStr == TEXT("TryFollowTransitions"))
			return EStateTreeStateSelectionBehavior::TryFollowTransitions;
		return EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder;
	}

	EStateTreeTransitionTrigger ParseTransitionTrigger(const FString& TriggerStr)
	{
		if (TriggerStr == TEXT("OnStateCompleted"))
			return EStateTreeTransitionTrigger::OnStateCompleted;
		if (TriggerStr == TEXT("OnStateSucceeded"))
			return EStateTreeTransitionTrigger::OnStateSucceeded;
		if (TriggerStr == TEXT("OnStateFailed"))
			return EStateTreeTransitionTrigger::OnStateFailed;
		if (TriggerStr == TEXT("OnTick"))
			return EStateTreeTransitionTrigger::OnTick;
		if (TriggerStr == TEXT("OnEvent"))
			return EStateTreeTransitionTrigger::OnEvent;
		return EStateTreeTransitionTrigger::None;
	}

	EStateTreeTransitionType ParseTransitionType(const FString& TypeStr)
	{
		if (TypeStr == TEXT("GotoState"))
			return EStateTreeTransitionType::GotoState;
		if (TypeStr == TEXT("NextState"))
			return EStateTreeTransitionType::NextState;
		if (TypeStr == TEXT("NextSelectableState"))
			return EStateTreeTransitionType::NextSelectableState;
		if (TypeStr == TEXT("Succeeded"))
			return EStateTreeTransitionType::Succeeded;
		if (TypeStr == TEXT("Failed"))
			return EStateTreeTransitionType::Failed;
		return EStateTreeTransitionType::None;
	}

	EStateTreeTransitionPriority ParseTransitionPriority(const FString& PriorityStr)
	{
		if (PriorityStr == TEXT("Low"))
			return EStateTreeTransitionPriority::Low;
		if (PriorityStr == TEXT("Medium"))
			return EStateTreeTransitionPriority::Medium;
		if (PriorityStr == TEXT("High"))
			return EStateTreeTransitionPriority::High;
		if (PriorityStr == TEXT("Critical"))
			return EStateTreeTransitionPriority::Critical;
		if (PriorityStr == TEXT("None"))
			return EStateTreeTransitionPriority::None;
		return EStateTreeTransitionPriority::Normal;
	}

	void SetInitialProperties(FStateTreeEditorNode& Node, const TSharedPtr<FJsonObject>& PropsObj, UObject* Outer)
	{
		if (!PropsObj.IsValid())
			return;
		for (const auto& Pair : PropsObj->Values)
		{
			FString Value;
			if (Pair.Value->TryGetString(Value))
			{
				FString Error;
				// Try on node struct first, then instance data
				if (!ClaireonStateTreeHelpers::SetNodeProperty(Node, Pair.Key, Value, false, Error))
				{
					ClaireonStateTreeHelpers::SetNodeProperty(Node, Pair.Key, Value, true, Error);
				}
			}
		}
	}
} // namespace

// ============================================================================
// Tool Interface Implementation
// ============================================================================

FString ClaireonTool_StateTreeEdit::GetName() const
{
	return TEXT("claireon.statetree_edit");
}

FString ClaireonTool_StateTreeEdit::GetDescription() const
{
	return TEXT("Session-based State Tree editor. Manage states, tasks, conditions, transitions, evaluators, and bindings. Start with 'open', build your tree, then 'compile' and 'save'.");
}

FString ClaireonTool_StateTreeEdit::GetFullDescription() const
{
	return TEXT("Interactively edit a State Tree asset using a session-based model. "
				"Start with 'open' to begin a session, then use operations to add/remove/modify states, tasks, conditions, "
				"transitions, evaluators, considerations, and property bindings. Finish with 'compile' and 'save'.\n\n"
				"Session operations: open, close\n"
				"State operations: add_state, remove_state, rename_state, move_state, set_state_type, set_state_selection_behavior, set_state_enabled\n"
				"Node operations: add_task, remove_task, add_enter_condition, remove_enter_condition, add_consideration, remove_consideration\n"
				"Global operations: add_evaluator, remove_evaluator, add_global_task, remove_global_task\n"
				"Transition operations: add_transition, remove_transition, modify_transition, add_transition_condition, remove_transition_condition\n"
				"Binding operations: add_binding, add_property_function, remove_binding\n"
				"Property operations: set_node_property\n"
				"Build operations: compile, save\n"
				"Declarative: apply_spec - Apply a declarative JSON specification to create/modify the asset atomically. "
				"Accepts: asset_path (string, required), spec (object, required), session_id (string, optional).\n\n"
				"insert_after (optional, for add_state and move_state): GUID of a sibling state. "
				"The new/moved state is inserted immediately after this sibling. "
				"If omitted, the state is appended to the end of the parent's children.");
}

TSharedPtr<FJsonObject> ClaireonTool_StateTreeEdit::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> SessionIdProp = MakeShared<FJsonObject>();
	SessionIdProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionIdProp->SetStringField(TEXT("description"), TEXT("Session identifier from a previous 'open' operation. Required for all operations except 'open'."));
	Properties->SetObjectField(TEXT("session_id"), SessionIdProp);

	TSharedPtr<FJsonObject> OperationProp = MakeShared<FJsonObject>();
	OperationProp->SetStringField(TEXT("type"), TEXT("string"));
	TArray<TSharedPtr<FJsonValue>> OperationEnum;
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("open")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_state")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_state")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("rename_state")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("move_state")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("set_state_type")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("set_state_selection_behavior")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("set_state_enabled")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_task")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_task")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_enter_condition")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_enter_condition")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_consideration")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_consideration")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_transition")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_transition")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("modify_transition")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_transition_condition")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_transition_condition")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_evaluator")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_evaluator")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_global_task")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_global_task")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_binding")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("add_property_function")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("remove_binding")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("set_node_property")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("compile")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("save")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("close")));
	OperationEnum.Add(MakeShared<FJsonValueString>(TEXT("apply_spec")));
	OperationProp->SetArrayField(TEXT("enum"), OperationEnum);
	OperationProp->SetStringField(TEXT("description"), TEXT("The editing operation to perform."));
	Properties->SetObjectField(TEXT("operation"), OperationProp);

	TSharedPtr<FJsonObject> ParamsProp = MakeShared<FJsonObject>();
	ParamsProp->SetStringField(TEXT("type"), TEXT("object"));
	ParamsProp->SetStringField(TEXT("description"), TEXT("Operation-specific parameters. See operation descriptions for details."));
	Properties->SetObjectField(TEXT("params"), ParamsProp);

	// suppress_output property
	TSharedPtr<FJsonObject> SuppressOutputProp = MakeShared<FJsonObject>();
	SuppressOutputProp->SetStringField(TEXT("type"), TEXT("boolean"));
	SuppressOutputProp->SetStringField(TEXT("description"),
		TEXT("When true, returns only a brief status instead of the full tree state. "
			 "Use this for intermediate operations in a batch to reduce response size and speed up execution, "
			 "then omit it on the final operation to get the full tree state and verify all changes."));
	Properties->SetObjectField(TEXT("suppress_output"), SuppressOutputProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("operation")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonTool_StateTreeEdit::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	if (Operation == TEXT("apply_spec"))
	{
		return Operation_ApplySpec(Params);
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

	FStateTreeEditToolData* Data = ToolData.Find(SessionId);
	if (!Data)
	{
		return MakeErrorResult(TEXT("Session tool data not found"));
	}

	Data->bSuppressOutput = bSuppressOutput;

	if (Operation == TEXT("close"))
		return Operation_Close(SessionId, Data, Params);
	if (Operation == TEXT("add_state"))
		return Operation_AddState(SessionId, Data, Params);
	if (Operation == TEXT("remove_state"))
		return Operation_RemoveState(SessionId, Data, Params);
	if (Operation == TEXT("rename_state"))
		return Operation_RenameState(SessionId, Data, Params);
	if (Operation == TEXT("move_state"))
		return Operation_MoveState(SessionId, Data, Params);
	if (Operation == TEXT("set_state_type"))
		return Operation_SetStateType(SessionId, Data, Params);
	if (Operation == TEXT("set_state_selection_behavior"))
		return Operation_SetStateSelectionBehavior(SessionId, Data, Params);
	if (Operation == TEXT("set_state_enabled"))
		return Operation_SetStateEnabled(SessionId, Data, Params);
	if (Operation == TEXT("add_task"))
		return Operation_AddTask(SessionId, Data, Params);
	if (Operation == TEXT("remove_task"))
		return Operation_RemoveTask(SessionId, Data, Params);
	if (Operation == TEXT("add_enter_condition"))
		return Operation_AddEnterCondition(SessionId, Data, Params);
	if (Operation == TEXT("remove_enter_condition"))
		return Operation_RemoveEnterCondition(SessionId, Data, Params);
	if (Operation == TEXT("add_consideration"))
		return Operation_AddConsideration(SessionId, Data, Params);
	if (Operation == TEXT("remove_consideration"))
		return Operation_RemoveConsideration(SessionId, Data, Params);
	if (Operation == TEXT("add_transition"))
		return Operation_AddTransition(SessionId, Data, Params);
	if (Operation == TEXT("remove_transition"))
		return Operation_RemoveTransition(SessionId, Data, Params);
	if (Operation == TEXT("modify_transition"))
		return Operation_ModifyTransition(SessionId, Data, Params);
	if (Operation == TEXT("add_transition_condition"))
		return Operation_AddTransitionCondition(SessionId, Data, Params);
	if (Operation == TEXT("remove_transition_condition"))
		return Operation_RemoveTransitionCondition(SessionId, Data, Params);
	if (Operation == TEXT("add_evaluator"))
		return Operation_AddEvaluator(SessionId, Data, Params);
	if (Operation == TEXT("remove_evaluator"))
		return Operation_RemoveEvaluator(SessionId, Data, Params);
	if (Operation == TEXT("add_global_task"))
		return Operation_AddGlobalTask(SessionId, Data, Params);
	if (Operation == TEXT("remove_global_task"))
		return Operation_RemoveGlobalTask(SessionId, Data, Params);
	if (Operation == TEXT("add_binding"))
		return Operation_AddBinding(SessionId, Data, Params);
	if (Operation == TEXT("add_property_function"))
		return Operation_AddPropertyFunction(SessionId, Data, Params);
	if (Operation == TEXT("remove_binding"))
		return Operation_RemoveBinding(SessionId, Data, Params);
	if (Operation == TEXT("set_node_property"))
		return Operation_SetNodeProperty(SessionId, Data, Params);
	if (Operation == TEXT("compile"))
		return Operation_Compile(SessionId, Data, Params);
	if (Operation == TEXT("save"))
		return Operation_Save(SessionId, Data, Params);

	return MakeErrorResult(FString::Printf(TEXT("Unknown operation: %s"), *Operation));
}

// ============================================================================
// Session Management
// ============================================================================

void ClaireonTool_StateTreeEdit::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == TEXT("claireon.statetree_edit"))
	{
		ToolData.Remove(Info.SessionId);
	}
}

FToolResult ClaireonTool_StateTreeEdit::BuildStateResponse(const FString& SessionId, FStateTreeEditToolData* Data)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	// Fast path: skip expensive tree state output when caller doesn't need it
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

	FString Error;
	UStateTreeEditorData* EditorData = ClaireonStateTreeHelpers::GetEditorData(Data->StateTree.Get(), Error);
	if (!EditorData)
	{
		return MakeErrorResult(FString::Printf(TEXT("Session error: %s"), *Error));
	}

	FString Output;
	Output += TEXT("=== Session Status ===\n");
	Output += FString::Printf(TEXT("Session: %s\n"), *SessionId);
	Output += FString::Printf(TEXT("Asset: %s\n"), *Data->StateTree->GetPathName());

	if (Data->FocusedStateId.IsValid())
	{
		UStateTreeState* FocusState = ClaireonStateTreeHelpers::FindStateById(EditorData, Data->FocusedStateId);
		if (FocusState)
		{
			Output += FString::Printf(TEXT("Focused State: [%s] %s\n"),
				*Data->FocusedStateId.ToString(EGuidFormats::DigitsWithHyphensLower),
				*FocusState->Name.ToString());
		}
	}

	Output += FString::Printf(TEXT("Last Operation: %s\n"), *Data->LastOperationStatus);
	Output += TEXT("\n");

	// Show affected area (focused state)
	if (Data->FocusedStateId.IsValid())
	{
		UStateTreeState* FocusState = ClaireonStateTreeHelpers::FindStateById(EditorData, Data->FocusedStateId);
		if (FocusState)
		{
			Output += TEXT("=== Affected Area ===\n");
			Output += ClaireonStateTreeHelpers::FormatStateArea(FocusState);
		}
	}

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("asset_path"), Data->StateTree->GetPathName());
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

FToolResult ClaireonTool_StateTreeEdit::Operation_Open(const TSharedPtr<FJsonObject>& Params)
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

	FString Error;
	UStateTree* StateTree = ClaireonStateTreeHelpers::LoadStateTreeAsset(AssetPath, Error);
	if (!StateTree)
		return MakeErrorResult(Error);

	UStateTreeEditorData* EditorData = ClaireonStateTreeHelpers::GetEditorData(StateTree, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	// Register delegate if not done yet
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonTool_StateTreeEdit::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	const FString ResolvedAssetPath = StateTree->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, TEXT("claireon.statetree_edit"));
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
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *ResolvedAssetPath));
	}
	const FString SessionId = OpenResult.SessionId;

	// If ReusedExistingSession, still update tool data
	FStateTreeEditToolData NewData;
	NewData.StateTree = StateTree;
	NewData.LastOperationStatus = TEXT("Opened session");

	// Set cursor to first subtree root
	if (EditorData->SubTrees.Num() > 0 && EditorData->SubTrees[0])
	{
		NewData.FocusedStateId = EditorData->SubTrees[0]->ID;
	}

	ToolData.Add(SessionId, MoveTemp(NewData));

	// Return full tree structure + session ID
	FString StructureText = ClaireonStateTreeHelpers::FormatStateTreeStructure(EditorData);

	TSharedPtr<FJsonObject> OpenData = MakeShared<FJsonObject>();
	OpenData->SetStringField(TEXT("session_id"), SessionId);
	OpenData->SetStringField(TEXT("asset_path"), AssetPath);
	OpenData->SetStringField(TEXT("status"), TEXT("Opened session"));
	OpenData->SetStringField(TEXT("structure"), StructureText);

	return MakeSuccessResult(OpenData, FString::Printf(TEXT("Opened session for %s"), *FPaths::GetBaseFilename(AssetPath)));
}

FToolResult ClaireonTool_StateTreeEdit::Operation_Close(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	bool bCompileFirst = false;
	bool bSaveFirst = false;
	Params->TryGetBoolField(TEXT("compile_first"), bCompileFirst);
	Params->TryGetBoolField(TEXT("save_first"), bSaveFirst);

	if (bCompileFirst)
	{
		Operation_Compile(SessionId, Data, MakeShared<FJsonObject>());
	}
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

// ============================================================================
// State Operations
// ============================================================================

FToolResult ClaireonTool_StateTreeEdit::Operation_AddState(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid ParentStateId;
	if (!ParseGuidParam(Params, TEXT("parent_state_id"), ParentStateId, Error))
	{
		return MakeErrorResult(Error);
	}

	FString Name;
	if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: name"));
	}

	FString StateTypeStr = TEXT("State");
	Params->TryGetStringField(TEXT("state_type"), StateTypeStr);

	UStateTreeState* ParentState = ClaireonStateTreeHelpers::FindStateById(EditorData, ParentStateId);
	if (!ParentState)
	{
		return MakeErrorResult(FString::Printf(TEXT("Parent state not found: %s"), *ParentStateId.ToString()));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add State")));
	Data->StateTree->Modify();

	UStateTreeState& NewState = ParentState->AddChildState(FName(*Name), ParseStateType(StateTypeStr));

	// insert_after: optionally reorder the newly-appended child
	FGuid InsertAfterId;
	{
		FString InsertAfterStr;
		if (Params->TryGetStringField(TEXT("insert_after"), InsertAfterStr) && !InsertAfterStr.IsEmpty())
		{
			FGuid::Parse(InsertAfterStr, InsertAfterId);
		}
	}
	if (InsertAfterId.IsValid())
	{
		int32 AfterIndex = ParentState->Children.IndexOfByPredicate(
			[&InsertAfterId](const UStateTreeState* S)
		{
			return S && S->ID == InsertAfterId;
		});
		if (AfterIndex == INDEX_NONE)
		{
			return MakeErrorResult(FString::Printf(TEXT("insert_after state '%s' not found among children of '%s'"),
				*InsertAfterId.ToString(), *ParentState->Name.ToString()));
		}
		int32 LastIndex = ParentState->Children.Num() - 1;
		if (LastIndex != AfterIndex + 1)
		{
			TObjectPtr<UStateTreeState> NewChild = ParentState->Children[LastIndex];
			ParentState->Children.RemoveAt(LastIndex, EAllowShrinking::No);
			ParentState->Children.Insert(NewChild, AfterIndex + 1);
		}
	}

	Data->PushHistory();
	Data->FocusedStateId = NewState.ID;
	Data->LastOperationStatus = FString::Printf(TEXT("add_state â Added '%s' to '%s'"), *Name, *ParentState->Name.ToString());

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_RemoveState(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid StateId;
	if (!ParseGuidParam(Params, TEXT("state_id"), StateId, Error))
	{
		return MakeErrorResult(Error);
	}

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
	{
		return MakeErrorResult(FString::Printf(TEXT("State not found: %s"), *StateId.ToString()));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove State")));
	Data->StateTree->Modify();

	FString StateName = State->Name.ToString();
	UStateTreeState* ParentState = Cast<UStateTreeState>(State->GetOuter());

	if (ParentState)
	{
		ParentState->Children.Remove(State);
		Data->FocusedStateId = ParentState->ID;
	}
	else
	{
		EditorData->SubTrees.Remove(State);
		Data->FocusedStateId = FGuid();
	}

	Data->LastOperationStatus = FString::Printf(TEXT("remove_state â Removed '%s'"), *StateName);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_RenameState(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid StateId;
	if (!ParseGuidParam(Params, TEXT("state_id"), StateId, Error))
	{
		return MakeErrorResult(Error);
	}

	FString Name;
	if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: name"));
	}

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Rename State")));
	Data->StateTree->Modify();

	FString OldName = State->Name.ToString();
	State->Name = FName(*Name);
	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("rename_state â '%s' â '%s'"), *OldName, *Name);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_MoveState(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid StateId, NewParentId;
	if (!ParseGuidParam(Params, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);
	if (!ParseGuidParam(Params, TEXT("new_parent_id"), NewParentId, Error))
		return MakeErrorResult(Error);

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	UStateTreeState* NewParent = ClaireonStateTreeHelpers::FindStateById(EditorData, NewParentId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));
	if (!NewParent)
		return MakeErrorResult(TEXT("New parent state not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Move State")));
	Data->StateTree->Modify();

	// Parse optional insert_after
	FGuid InsertAfterId;
	{
		FString InsertAfterStr;
		if (Params->TryGetStringField(TEXT("insert_after"), InsertAfterStr) && !InsertAfterStr.IsEmpty())
		{
			FGuid::Parse(InsertAfterStr, InsertAfterId);
		}
	}
	if (InsertAfterId.IsValid() && InsertAfterId == State->ID)
	{
		return MakeErrorResult(TEXT("insert_after cannot reference the state being moved"));
	}

	// Remove from old parent
	UStateTreeState* OldParent = Cast<UStateTreeState>(State->GetOuter());
	if (OldParent)
	{
		OldParent->Children.Remove(State);
	}
	else
	{
		EditorData->SubTrees.Remove(State);
	}

	// Add to new parent
	if (InsertAfterId.IsValid())
	{
		int32 AfterIndex = NewParent->Children.IndexOfByPredicate(
			[&InsertAfterId](const UStateTreeState* S)
		{
			return S && S->ID == InsertAfterId;
		});
		if (AfterIndex == INDEX_NONE)
		{
			// Restore: put state back in old parent to avoid orphaning
			if (OldParent)
			{
				OldParent->Children.Add(State);
			}
			else
			{
				EditorData->SubTrees.Add(State);
			}
			return MakeErrorResult(FString::Printf(TEXT("insert_after state '%s' not found among children of '%s'"),
				*InsertAfterId.ToString(), *NewParent->Name.ToString()));
		}
		NewParent->Children.Insert(State, AfterIndex + 1);
	}
	else
	{
		NewParent->Children.Add(State);
	}
	State->Rename(nullptr, NewParent);

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("move_state â Moved '%s' to '%s'"), *State->Name.ToString(), *NewParent->Name.ToString());

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_SetStateType(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid StateId;
	if (!ParseGuidParam(Params, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);

	FString TypeStr;
	if (!Params->TryGetStringField(TEXT("type"), TypeStr))
		return MakeErrorResult(TEXT("Missing parameter: type"));

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set State Type")));
	Data->StateTree->Modify();
	State->Type = ParseStateType(TypeStr);

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("set_state_type â '%s' type set to %s"), *State->Name.ToString(), *TypeStr);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_SetStateSelectionBehavior(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid StateId;
	if (!ParseGuidParam(Params, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);

	FString BehaviorStr;
	if (!Params->TryGetStringField(TEXT("behavior"), BehaviorStr))
		return MakeErrorResult(TEXT("Missing parameter: behavior"));

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set State Selection Behavior")));
	Data->StateTree->Modify();
	State->SelectionBehavior = ParseSelectionBehavior(BehaviorStr);

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("set_state_selection_behavior â '%s' set to %s"), *State->Name.ToString(), *BehaviorStr);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_SetStateEnabled(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid StateId;
	if (!ParseGuidParam(Params, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);

	bool bEnabled = true;
	Params->TryGetBoolField(TEXT("enabled"), bEnabled);

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set State Enabled")));
	Data->StateTree->Modify();
	State->bEnabled = bEnabled;

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("set_state_enabled â '%s' %s"), *State->Name.ToString(), bEnabled ? TEXT("enabled") : TEXT("disabled"));

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Node Operations
// ============================================================================

FToolResult ClaireonTool_StateTreeEdit::Operation_AddTask(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid StateId;
	if (!ParseGuidParam(Params, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);

	FString NodeType;
	if (!Params->TryGetStringField(TEXT("node_type"), NodeType))
		return MakeErrorResult(TEXT("Missing parameter: node_type"));

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	UScriptStruct* NodeStruct = ClaireonStateTreeHelpers::ResolveNodeStruct(NodeType, Error);
	if (!NodeStruct)
		return MakeErrorResult(Error);

	FStateTreeEditorNode NewNode;
	if (!ClaireonStateTreeHelpers::CreateEditorNode(NewNode, NodeStruct, State, Error))
	{
		return MakeErrorResult(Error);
	}

	// Set initial properties
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
	{
		SetInitialProperties(NewNode, *PropsObj, State);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Task")));
	Data->StateTree->Modify();

	// Check schema for single vs multiple tasks
	const UStateTreeSchema* Schema = Data->StateTree->GetSchema();
	if (Schema && !Schema->AllowMultipleTasks())
	{
		State->SingleTask = MoveTemp(NewNode);
	}
	else
	{
		State->Tasks.Add(MoveTemp(NewNode));
	}

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("add_task â Added %s to '%s'"), *NodeType, *State->Name.ToString());

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_RemoveTask(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid StateId, NodeId;
	if (!ParseGuidParam(Params, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);
	if (!ParseGuidParam(Params, TEXT("node_id"), NodeId, Error))
		return MakeErrorResult(Error);

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Task")));
	Data->StateTree->Modify();

	// Check SingleTask
	if (State->SingleTask.ID == NodeId)
	{
		State->SingleTask = FStateTreeEditorNode();
		Data->LastOperationStatus = TEXT("remove_task â Removed single task");
	}
	else
	{
		int32 Index = State->Tasks.IndexOfByPredicate([&NodeId](const FStateTreeEditorNode& N)
		{
			return N.ID == NodeId;
		});
		if (Index == INDEX_NONE)
			return MakeErrorResult(TEXT("Task node not found in state"));
		State->Tasks.RemoveAt(Index);
		Data->LastOperationStatus = TEXT("remove_task â Removed task");
	}

	Data->FocusedStateId = StateId;
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_AddEnterCondition(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid StateId;
	if (!ParseGuidParam(Params, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);

	FString NodeType;
	if (!Params->TryGetStringField(TEXT("node_type"), NodeType))
		return MakeErrorResult(TEXT("Missing parameter: node_type"));

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	UScriptStruct* NodeStruct = ClaireonStateTreeHelpers::ResolveNodeStruct(NodeType, Error);
	if (!NodeStruct)
		return MakeErrorResult(Error);

	FStateTreeEditorNode NewNode;
	if (!ClaireonStateTreeHelpers::CreateEditorNode(NewNode, NodeStruct, State, Error))
	{
		return MakeErrorResult(Error);
	}

	// Set expression operand and indent
	FString OperandStr = TEXT("And");
	Params->TryGetStringField(TEXT("expression_operand"), OperandStr);
	if (OperandStr == TEXT("Or"))
		NewNode.ExpressionOperand = EStateTreeExpressionOperand::Or;
	else if (OperandStr == TEXT("Copy"))
		NewNode.ExpressionOperand = EStateTreeExpressionOperand::Copy;
	else
		NewNode.ExpressionOperand = EStateTreeExpressionOperand::And;

	int32 Indent = 0;
	if (Params->HasField(TEXT("expression_indent")))
	{
		Indent = FMath::Clamp(static_cast<int32>(Params->GetNumberField(TEXT("expression_indent"))), 0, 4);
	}
	NewNode.ExpressionIndent = static_cast<uint8>(Indent);

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
	{
		SetInitialProperties(NewNode, *PropsObj, State);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Enter Condition")));
	Data->StateTree->Modify();
	State->EnterConditions.Add(MoveTemp(NewNode));

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("add_enter_condition â Added %s to '%s'"), *NodeType, *State->Name.ToString());

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_RemoveEnterCondition(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid StateId, NodeId;
	if (!ParseGuidParam(Params, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);
	if (!ParseGuidParam(Params, TEXT("node_id"), NodeId, Error))
		return MakeErrorResult(Error);

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	int32 Index = State->EnterConditions.IndexOfByPredicate([&NodeId](const FStateTreeEditorNode& N)
	{
		return N.ID == NodeId;
	});
	if (Index == INDEX_NONE)
		return MakeErrorResult(TEXT("Condition not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Enter Condition")));
	Data->StateTree->Modify();
	State->EnterConditions.RemoveAt(Index);

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = TEXT("remove_enter_condition â Removed condition");
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_AddConsideration(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid StateId;
	if (!ParseGuidParam(Params, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);

	FString NodeType;
	if (!Params->TryGetStringField(TEXT("node_type"), NodeType))
		return MakeErrorResult(TEXT("Missing parameter: node_type"));

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	UScriptStruct* NodeStruct = ClaireonStateTreeHelpers::ResolveNodeStruct(NodeType, Error);
	if (!NodeStruct)
		return MakeErrorResult(Error);

	FStateTreeEditorNode NewNode;
	if (!ClaireonStateTreeHelpers::CreateEditorNode(NewNode, NodeStruct, State, Error))
	{
		return MakeErrorResult(Error);
	}

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
	{
		SetInitialProperties(NewNode, *PropsObj, State);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Consideration")));
	Data->StateTree->Modify();
	State->Considerations.Add(MoveTemp(NewNode));

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("add_consideration â Added %s to '%s'"), *NodeType, *State->Name.ToString());
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_RemoveConsideration(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid StateId, NodeId;
	if (!ParseGuidParam(Params, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);
	if (!ParseGuidParam(Params, TEXT("node_id"), NodeId, Error))
		return MakeErrorResult(Error);

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	int32 Index = State->Considerations.IndexOfByPredicate([&NodeId](const FStateTreeEditorNode& N)
	{
		return N.ID == NodeId;
	});
	if (Index == INDEX_NONE)
		return MakeErrorResult(TEXT("Consideration not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Consideration")));
	Data->StateTree->Modify();
	State->Considerations.RemoveAt(Index);

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = TEXT("remove_consideration â Removed consideration");
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_AddEvaluator(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FString NodeType;
	if (!Params->TryGetStringField(TEXT("node_type"), NodeType))
		return MakeErrorResult(TEXT("Missing parameter: node_type"));

	UScriptStruct* NodeStruct = ClaireonStateTreeHelpers::ResolveNodeStruct(NodeType, Error);
	if (!NodeStruct)
		return MakeErrorResult(Error);

	FStateTreeEditorNode NewNode;
	if (!ClaireonStateTreeHelpers::CreateEditorNode(NewNode, NodeStruct, EditorData, Error))
	{
		return MakeErrorResult(Error);
	}

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
	{
		SetInitialProperties(NewNode, *PropsObj, EditorData);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Evaluator")));
	Data->StateTree->Modify();
	EditorData->Evaluators.Add(MoveTemp(NewNode));

	Data->LastOperationStatus = FString::Printf(TEXT("add_evaluator â Added global evaluator %s"), *NodeType);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_RemoveEvaluator(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid NodeId;
	if (!ParseGuidParam(Params, TEXT("node_id"), NodeId, Error))
		return MakeErrorResult(Error);

	int32 Index = EditorData->Evaluators.IndexOfByPredicate([&NodeId](const FStateTreeEditorNode& N)
	{
		return N.ID == NodeId;
	});
	if (Index == INDEX_NONE)
		return MakeErrorResult(TEXT("Evaluator not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Evaluator")));
	Data->StateTree->Modify();
	EditorData->Evaluators.RemoveAt(Index);

	Data->LastOperationStatus = TEXT("remove_evaluator â Removed global evaluator");
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_AddGlobalTask(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FString NodeType;
	if (!Params->TryGetStringField(TEXT("node_type"), NodeType))
		return MakeErrorResult(TEXT("Missing parameter: node_type"));

	UScriptStruct* NodeStruct = ClaireonStateTreeHelpers::ResolveNodeStruct(NodeType, Error);
	if (!NodeStruct)
		return MakeErrorResult(Error);

	FStateTreeEditorNode NewNode;
	if (!ClaireonStateTreeHelpers::CreateEditorNode(NewNode, NodeStruct, EditorData, Error))
	{
		return MakeErrorResult(Error);
	}

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
	{
		SetInitialProperties(NewNode, *PropsObj, EditorData);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Global Task")));
	Data->StateTree->Modify();
	EditorData->GlobalTasks.Add(MoveTemp(NewNode));

	Data->LastOperationStatus = FString::Printf(TEXT("add_global_task â Added global task %s"), *NodeType);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_RemoveGlobalTask(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid NodeId;
	if (!ParseGuidParam(Params, TEXT("node_id"), NodeId, Error))
		return MakeErrorResult(Error);

	int32 Index = EditorData->GlobalTasks.IndexOfByPredicate([&NodeId](const FStateTreeEditorNode& N)
	{
		return N.ID == NodeId;
	});
	if (Index == INDEX_NONE)
		return MakeErrorResult(TEXT("Global task not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Global Task")));
	Data->StateTree->Modify();
	EditorData->GlobalTasks.RemoveAt(Index);

	Data->LastOperationStatus = TEXT("remove_global_task â Removed global task");
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_SetNodeProperty(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid NodeId;
	if (!ParseGuidParam(Params, TEXT("node_id"), NodeId, Error))
		return MakeErrorResult(Error);

	FString PropertyName, PropertyValue;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
		return MakeErrorResult(TEXT("Missing parameter: property_name"));
	if (!Params->TryGetStringField(TEXT("property_value"), PropertyValue))
		return MakeErrorResult(TEXT("Missing parameter: property_value"));

	bool bOnInstanceData = false;
	Params->TryGetBoolField(TEXT("on_instance_data"), bOnInstanceData);

	FStateTreeEditorNode* Node = ClaireonStateTreeHelpers::FindNodeById(EditorData, NodeId);
	if (!Node)
		return MakeErrorResult(TEXT("Node not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Node Property")));
	Data->StateTree->Modify();

	if (!ClaireonStateTreeHelpers::SetNodeProperty(*Node, PropertyName, PropertyValue, bOnInstanceData, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("set_node_property â Set %s=%s"), *PropertyName, *PropertyValue);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Transition Operations
// ============================================================================

FToolResult ClaireonTool_StateTreeEdit::Operation_AddTransition(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid StateId;
	if (!ParseGuidParam(Params, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);

	FString TriggerStr, TargetTypeStr;
	if (!Params->TryGetStringField(TEXT("trigger"), TriggerStr))
		return MakeErrorResult(TEXT("Missing parameter: trigger"));
	if (!Params->TryGetStringField(TEXT("target_type"), TargetTypeStr))
		return MakeErrorResult(TEXT("Missing parameter: target_type"));

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	EStateTreeTransitionTrigger Trigger = ParseTransitionTrigger(TriggerStr);
	EStateTreeTransitionType TransType = ParseTransitionType(TargetTypeStr);

	// Resolve target state if GotoState
	UStateTreeState* TargetState = nullptr;
	if (TransType == EStateTreeTransitionType::GotoState)
	{
		FGuid TargetStateId;
		if (!ParseGuidParam(Params, TEXT("target_state_id"), TargetStateId, Error))
		{
			return MakeErrorResult(TEXT("target_state_id required when target_type is GotoState"));
		}
		TargetState = ClaireonStateTreeHelpers::FindStateById(EditorData, TargetStateId);
		if (!TargetState)
			return MakeErrorResult(TEXT("Target state not found"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Transition")));
	Data->StateTree->Modify();

	FStateTreeTransition& NewTransition = State->AddTransition(Trigger, TransType, TargetState);

	// Set event tag if OnEvent trigger
	FString EventTag;
	if (Params->TryGetStringField(TEXT("event_tag"), EventTag) && !EventTag.IsEmpty())
	{
		NewTransition.RequiredEvent.Tag = FGameplayTag::RequestGameplayTag(FName(*EventTag), false);
	}

	// Set priority
	FString PriorityStr;
	if (Params->TryGetStringField(TEXT("priority"), PriorityStr))
	{
		NewTransition.Priority = ParseTransitionPriority(PriorityStr);
	}

	// Set enabled
	bool bEnabled = true;
	if (Params->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		NewTransition.bTransitionEnabled = bEnabled;
	}

	// Set delay
	double Delay = 0.0;
	if (Params->TryGetNumberField(TEXT("delay"), Delay) && Delay > 0.0)
	{
		NewTransition.bDelayTransition = true;
		NewTransition.DelayDuration = static_cast<float>(Delay);
	}

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("add_transition â Added %s transition to '%s'"), *TriggerStr, *State->Name.ToString());
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_RemoveTransition(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid StateId, TransitionId;
	if (!ParseGuidParam(Params, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);
	if (!ParseGuidParam(Params, TEXT("transition_id"), TransitionId, Error))
		return MakeErrorResult(Error);

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	int32 Index = State->Transitions.IndexOfByPredicate([&TransitionId](const FStateTreeTransition& T)
	{
		return T.ID == TransitionId;
	});
	if (Index == INDEX_NONE)
		return MakeErrorResult(TEXT("Transition not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Transition")));
	Data->StateTree->Modify();
	State->Transitions.RemoveAt(Index);

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = TEXT("remove_transition â Removed transition");
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_ModifyTransition(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid StateId, TransitionId;
	if (!ParseGuidParam(Params, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);
	if (!ParseGuidParam(Params, TEXT("transition_id"), TransitionId, Error))
		return MakeErrorResult(Error);

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	FStateTreeTransition* Trans = ClaireonStateTreeHelpers::FindTransitionById(State, TransitionId);
	if (!Trans)
		return MakeErrorResult(TEXT("Transition not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Modify Transition")));
	Data->StateTree->Modify();

	FString TriggerStr;
	if (Params->TryGetStringField(TEXT("trigger"), TriggerStr))
	{
		Trans->Trigger = ParseTransitionTrigger(TriggerStr);
	}

	FString EventTag;
	if (Params->TryGetStringField(TEXT("event_tag"), EventTag))
	{
		Trans->RequiredEvent.Tag = FGameplayTag::RequestGameplayTag(FName(*EventTag), false);
	}

	FString PriorityStr;
	if (Params->TryGetStringField(TEXT("priority"), PriorityStr))
	{
		Trans->Priority = ParseTransitionPriority(PriorityStr);
	}

	bool bEnabled;
	if (Params->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		Trans->bTransitionEnabled = bEnabled;
	}

#if WITH_EDITORONLY_DATA
	FString TargetTypeStr;
	if (Params->TryGetStringField(TEXT("target_type"), TargetTypeStr))
	{
		Trans->State.LinkType = ParseTransitionType(TargetTypeStr);

		if (Trans->State.LinkType == EStateTreeTransitionType::GotoState)
		{
			FGuid TargetStateId;
			if (ParseGuidParam(Params, TEXT("target_state_id"), TargetStateId, Error))
			{
				UStateTreeState* TargetState = ClaireonStateTreeHelpers::FindStateById(EditorData, TargetStateId);
				if (TargetState)
				{
					Trans->State.ID = TargetState->ID;
					Trans->State.Name = TargetState->Name;
				}
			}
		}
	}
#endif

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = TEXT("modify_transition â Modified transition");
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_AddTransitionCondition(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid StateId, TransitionId;
	if (!ParseGuidParam(Params, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);
	if (!ParseGuidParam(Params, TEXT("transition_id"), TransitionId, Error))
		return MakeErrorResult(Error);

	FString NodeType;
	if (!Params->TryGetStringField(TEXT("node_type"), NodeType))
		return MakeErrorResult(TEXT("Missing parameter: node_type"));

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	FStateTreeTransition* Trans = ClaireonStateTreeHelpers::FindTransitionById(State, TransitionId);
	if (!Trans)
		return MakeErrorResult(TEXT("Transition not found"));

	UScriptStruct* NodeStruct = ClaireonStateTreeHelpers::ResolveNodeStruct(NodeType, Error);
	if (!NodeStruct)
		return MakeErrorResult(Error);

	FStateTreeEditorNode NewNode;
	if (!ClaireonStateTreeHelpers::CreateEditorNode(NewNode, NodeStruct, State, Error))
	{
		return MakeErrorResult(Error);
	}

	FString OperandStr = TEXT("And");
	Params->TryGetStringField(TEXT("expression_operand"), OperandStr);
	if (OperandStr == TEXT("Or"))
		NewNode.ExpressionOperand = EStateTreeExpressionOperand::Or;
	else if (OperandStr == TEXT("Copy"))
		NewNode.ExpressionOperand = EStateTreeExpressionOperand::Copy;

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
	{
		SetInitialProperties(NewNode, *PropsObj, State);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Transition Condition")));
	Data->StateTree->Modify();
	Trans->Conditions.Add(MoveTemp(NewNode));

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("add_transition_condition â Added %s"), *NodeType);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_RemoveTransitionCondition(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid StateId, TransitionId, NodeId;
	if (!ParseGuidParam(Params, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);
	if (!ParseGuidParam(Params, TEXT("transition_id"), TransitionId, Error))
		return MakeErrorResult(Error);
	if (!ParseGuidParam(Params, TEXT("node_id"), NodeId, Error))
		return MakeErrorResult(Error);

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	FStateTreeTransition* Trans = ClaireonStateTreeHelpers::FindTransitionById(State, TransitionId);
	if (!Trans)
		return MakeErrorResult(TEXT("Transition not found"));

	int32 Index = Trans->Conditions.IndexOfByPredicate([&NodeId](const FStateTreeEditorNode& N)
	{
		return N.ID == NodeId;
	});
	if (Index == INDEX_NONE)
		return MakeErrorResult(TEXT("Condition not found in transition"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Transition Condition")));
	Data->StateTree->Modify();
	Trans->Conditions.RemoveAt(Index);

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = TEXT("remove_transition_condition â Removed condition");
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Binding Operations
// ============================================================================

FToolResult ClaireonTool_StateTreeEdit::Operation_AddBinding(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid SourceNodeId, TargetNodeId;
	if (!ParseGuidParam(Params, TEXT("source_node_id"), SourceNodeId, Error))
		return MakeErrorResult(Error);
	if (!ParseGuidParam(Params, TEXT("target_node_id"), TargetNodeId, Error))
		return MakeErrorResult(Error);

	FString SourceProperty, TargetProperty;
	if (!Params->TryGetStringField(TEXT("source_property"), SourceProperty))
		return MakeErrorResult(TEXT("Missing parameter: source_property"));
	if (!Params->TryGetStringField(TEXT("target_property"), TargetProperty))
		return MakeErrorResult(TEXT("Missing parameter: target_property"));

#if WITH_EDITORONLY_DATA
	FStateTreePropertyPath SourcePath(SourceNodeId);
	SourcePath.FromString(SourceProperty);

	FStateTreePropertyPath TargetPath(TargetNodeId);
	TargetPath.FromString(TargetProperty);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Property Binding")));
	Data->StateTree->Modify();

	FStateTreePropertyPathBinding Binding(SourcePath, TargetPath);
	EditorData->EditorBindings.AddPropertyBinding(Binding);

	Data->LastOperationStatus = FString::Printf(TEXT("add_binding â Bound %s â %s"), *SourceProperty, *TargetProperty);
#else
	Data->LastOperationStatus = TEXT("add_binding â Not available in non-editor builds");
#endif

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_AddPropertyFunction(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	// Required: struct type name of the property function
	FString StructName;
	if (!Params->TryGetStringField(TEXT("struct_name"), StructName))
		return MakeErrorResult(TEXT("Missing required parameter: struct_name"));

	// Required: target node and property to bind the function's output to
	FGuid TargetNodeId;
	if (!ParseGuidParam(Params, TEXT("target_node_id"), TargetNodeId, Error))
		return MakeErrorResult(Error);

	FString TargetProperty;
	if (!Params->TryGetStringField(TEXT("target_property"), TargetProperty))
		return MakeErrorResult(TEXT("Missing required parameter: target_property"));

	// Optional: source property path within the property function's instance data (output property)
	// If omitted, uses the single output property if one exists.
	FString SourceProperty;
	Params->TryGetStringField(TEXT("source_property"), SourceProperty);

#if WITH_EDITORONLY_DATA
	// Resolve the struct
	UScriptStruct* NodeStruct = ClaireonStateTreeHelpers::ResolveNodeStruct(StructName, Error);
	if (!NodeStruct)
		return MakeErrorResult(Error);

	// Verify it's a property function
	if (!NodeStruct->IsChildOf(FStateTreePropertyFunctionBase::StaticStruct()))
	{
		return MakeErrorResult(FString::Printf(TEXT("'%s' is not a property function (must derive from FStateTreePropertyFunctionBase)"), *StructName));
	}

	// Build target path
	FStateTreePropertyPath TargetPath(TargetNodeId);
	TargetPath.FromString(TargetProperty);

	// Build source path segments
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
	TArray<FPropertyBindingPathSegment> SourceSegments;
#else
	TArray<FStateTreePropertyPathSegment> SourceSegments;
#endif
	if (!SourceProperty.IsEmpty())
	{
		FStateTreePropertyPath TempPath;
		TempPath.FromString(SourceProperty);
		SourceSegments = TempPath.GetSegments();
	}
	else
	{
		// Auto-detect single output property from the property function's instance data type
		FInstancedStruct TempInstance;
		TempInstance.InitializeAs(NodeStruct);
		const FStateTreePropertyFunctionBase& TempFunction = TempInstance.Get<FStateTreePropertyFunctionBase>();
		if (const UStruct* InstanceType = Cast<const UStruct>(TempFunction.GetInstanceDataType()))
		{
			for (TFieldIterator<FProperty> It(InstanceType); It; ++It)
			{
				if (It->HasMetaData(TEXT("Category")) && It->GetMetaData(TEXT("Category")) == TEXT("Output"))
				{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
					SourceSegments.Add(FPropertyBindingPathSegment(It->GetFName()));
#else
					SourceSegments.Add(FStateTreePropertyPathSegment(It->GetFName()));
#endif
					break;
				}
			}
		}
		if (SourceSegments.IsEmpty())
		{
			return MakeErrorResult(TEXT("Could not auto-detect output property. Specify source_property explicitly."));
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Property Function Binding")));
	Data->StateTree->Modify();

	FStateTreePropertyPath ResultSourcePath = EditorData->EditorBindings.AddFunctionPropertyBinding(NodeStruct, SourceSegments, TargetPath);

	// Set parameter properties if provided
	const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), PropertiesObj) && PropertiesObj)
	{
		// Find the newly created binding to access the property function node
		for (FStateTreePropertyPathBinding& Binding : EditorData->EditorBindings.GetMutableBindings())
		{
			if (Binding.GetTargetPath() == TargetPath)
			{
				FStructView PropertyFunctionNodeView = Binding.GetMutablePropertyFunctionNode();
				if (PropertyFunctionNodeView.IsValid())
				{
					FStateTreeEditorNode& EditorNode = PropertyFunctionNodeView.Get<FStateTreeEditorNode>();
					if (EditorNode.Instance.IsValid())
					{
						for (const auto& Pair : (*PropertiesObj)->Values)
						{
							FString PropValue;
							if (Pair.Value->TryGetString(PropValue))
							{
								ClaireonStateTreeHelpers::SetNodeProperty(EditorNode, Pair.Key, PropValue, true, Error);
								if (!Error.IsEmpty())
								{
									UE_LOG(LogClaireon, Warning, TEXT("AddPropertyFunction: Failed to set property '%s': %s"), *Pair.Key, *Error);
									Error.Empty();
								}
							}
						}
					}
				}
				break;
			}
		}
	}

	Data->LastOperationStatus = FString::Printf(TEXT("add_property_function → Added %s → %s.%s"), *StructName, *TargetNodeId.ToString(), *TargetProperty);
#else
	Data->LastOperationStatus = TEXT("add_property_function → Not available in non-editor builds");
#endif

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_RemoveBinding(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UStateTreeEditorData* EditorData = GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid TargetNodeId;
	if (!ParseGuidParam(Params, TEXT("target_node_id"), TargetNodeId, Error))
		return MakeErrorResult(Error);

	FString TargetProperty;
	if (!Params->TryGetStringField(TEXT("target_property"), TargetProperty))
		return MakeErrorResult(TEXT("Missing parameter: target_property"));

#if WITH_EDITORONLY_DATA
	FStateTreePropertyPath TargetPath(TargetNodeId);
	TargetPath.FromString(TargetProperty);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Property Binding")));
	Data->StateTree->Modify();
	EditorData->EditorBindings.RemovePropertyBindings(TargetPath);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_binding â Removed binding to %s"), *TargetProperty);
#else
	Data->LastOperationStatus = TEXT("remove_binding â Not available in non-editor builds");
#endif

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Compile and Save
// ============================================================================

FToolResult ClaireonTool_StateTreeEdit::Operation_Compile(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	UStateTree* StateTree = Data->StateTree.Get();

	FStateTreeCompilerLog CompilerLog;
	bool bSuccess = UStateTreeEditingSubsystem::CompileStateTree(StateTree, CompilerLog);

	FString Output;
	if (bSuccess)
	{
		Output = TEXT("=== Compilation Succeeded ===\n");
	}
	else
	{
		Output = TEXT("=== Compilation Failed ===\n");
	}

	// Dump compiler messages to log for debugging
	CompilerLog.DumpToLog(LogClaireon);

	if (!bSuccess)
	{
		Output += TEXT("Check editor log (LogClaireon) for detailed compilation errors.\n");
	}

	Data->LastOperationStatus = bSuccess ? TEXT("compile â Succeeded") : TEXT("compile â Failed");
	return MakeErrorResult(Output);
}

FToolResult ClaireonTool_StateTreeEdit::Operation_Save(const FString& SessionId, FStateTreeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	UStateTree* StateTree = Data->StateTree.Get();
	UPackage* Package = StateTree->GetPackage();
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
		Data->LastOperationStatus = FString::Printf(TEXT("save â Saved %s"), *StateTree->GetPathName());
		return BuildStateResponse(SessionId, Data);
	}
	else
	{
		Data->LastOperationStatus = TEXT("save â Failed");
		return MakeErrorResult(TEXT("Failed to save State Tree package"));
	}
}

// ============================================================================
// apply_spec
// ============================================================================

FToolResult ClaireonTool_StateTreeEdit::Operation_ApplySpec(const TSharedPtr<FJsonObject>& Params)
{
	// Extract asset_path -- required
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("apply_spec requires 'asset_path' parameter"));
	}

	// Extract spec -- required JSON object
	const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("spec"), SpecPtr) || !SpecPtr || !SpecPtr->IsValid())
	{
		return MakeErrorResult(TEXT("apply_spec requires 'spec' parameter (JSON object)"));
	}

	// Optional: reuse an existing session
	FString SessionId;
	Params->TryGetStringField(TEXT("session_id"), SessionId);

	FClaireonSpecApplicator_StateTree Applicator;
	return Applicator.ApplySpec(*SpecPtr, AssetPath, SessionId);
}
