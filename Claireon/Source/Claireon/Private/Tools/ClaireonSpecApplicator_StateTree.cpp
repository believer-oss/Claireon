// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSpecApplicator_StateTree.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonSafeExec.h"
#include "ClaireonLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"
#include "FileHelpers.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeCompilerLog.h"
#include "GameplayTagContainer.h"

namespace
{
	// Reads optional transition fields from a spec JSON object and applies them to NewTransition.
	// Mirrors ClaireonStateTreeTool_AddTransition's parsing and additionally handles
	// consume_event_on_select for the gap-#1 payload-routing pattern.
	void SpecApplicatorStateTree_ApplySpecTransitionFields(
		const TSharedPtr<FJsonObject>& TransitionJson,
		FStateTreeTransition& NewTransition)
	{
		FString EventTag;
		if (TransitionJson->TryGetStringField(TEXT("event_tag"), EventTag) && !EventTag.IsEmpty())
		{
			NewTransition.RequiredEvent.Tag = FGameplayTag::RequestGameplayTag(FName(*EventTag), /*ErrorIfNotFound=*/false);
		}

		FString PriorityStr;
		if (TransitionJson->TryGetStringField(TEXT("priority"), PriorityStr) && !PriorityStr.IsEmpty())
		{
			NewTransition.Priority = ClaireonStateTreeEditInternal::ParseTransitionPriority(PriorityStr);
		}

		bool bEnabled = true;
		if (TransitionJson->TryGetBoolField(TEXT("enabled"), bEnabled))
		{
			NewTransition.bTransitionEnabled = bEnabled;
		}

		double DelaySeconds = 0.0;
		if (TransitionJson->TryGetNumberField(TEXT("delay"), DelaySeconds) && DelaySeconds > 0.0)
		{
			NewTransition.bDelayTransition = true;
			NewTransition.DelayDuration = static_cast<float>(DelaySeconds);
		}

		bool bConsumeEvent = true;
		if (TransitionJson->TryGetBoolField(TEXT("consume_event_on_select"), bConsumeEvent))
		{
			NewTransition.RequiredEvent.bConsumeEventOnSelect = bConsumeEvent;
		}
	}
}

bool FClaireonSpecApplicator_StateTree::ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors)
{
	bool bHasContent = false;

	// Check states array
	const TArray<TSharedPtr<FJsonValue>>* StatesArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("states"), StatesArray) && StatesArray)
	{
		bHasContent = true;

		// Build the spec-internal id set first so parent references can be resolved.
		TSet<FString> SpecStateIds;
		for (const TSharedPtr<FJsonValue>& Val : *StatesArray)
		{
			if (!Val.IsValid() || Val->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
			FString Id;
			if (Obj->TryGetStringField(TEXT("id"), Id) && !Id.IsEmpty())
			{
				SpecStateIds.Add(Id);
			}
		}

		for (int32 i = 0; i < StatesArray->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& StateVal = (*StatesArray)[i];
			if (!StateVal.IsValid() || StateVal->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& StateObj = StateVal->AsObject();

			FString StateId, StateName;
			if (!StateObj->TryGetStringField(TEXT("id"), StateId) || StateId.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("states[%d]: missing or empty 'id'"), i));
			}
			if (!StateObj->TryGetStringField(TEXT("name"), StateName) || StateName.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("states[%d]: missing or empty 'name'"), i));
			}

			// Parent validation: null/missing => root (allowed). Non-null must be either
			// a spec-internal id (forward reference) or a parseable FGuid (external parent
			// in the existing tree). Editor data is not yet loaded at validate-time, so
			// the FGuid path defers existence checks to Pass 1.
			if (StateObj->HasField(TEXT("parent")) && !StateObj->GetField<EJson::None>(TEXT("parent"))->IsNull())
			{
				FString ParentId;
				if (!StateObj->TryGetStringField(TEXT("parent"), ParentId) || ParentId.IsEmpty())
				{
					OutErrors.Add(FString::Printf(TEXT("states[%d]: 'parent' must be a non-empty string or null"), i));
					continue;
				}
				if (SpecStateIds.Contains(ParentId)) continue;

				FGuid ParentGuid;
				if (FGuid::Parse(ParentId, ParentGuid)) continue;

				OutErrors.Add(FString::Printf(
					TEXT("states[%d]: parent '%s' is neither in spec id_map nor a parseable FGuid"),
					i, *ParentId));
			}
		}
	}

	// Check evaluators/global_tasks
	const TArray<TSharedPtr<FJsonValue>>* EvalArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("evaluators"), EvalArray) && EvalArray)
	{
		bHasContent = true;
		for (int32 i = 0; i < EvalArray->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>& Obj = (*EvalArray)[i]->AsObject();
			if (!Obj.IsValid()) continue;
			FString Id, Type;
			if (!Obj->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
				OutErrors.Add(FString::Printf(TEXT("evaluators[%d]: missing 'id'"), i));
			if (!Obj->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty())
				OutErrors.Add(FString::Printf(TEXT("evaluators[%d]: missing 'type'"), i));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* GlobalTasksArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("global_tasks"), GlobalTasksArray) && GlobalTasksArray)
	{
		bHasContent = true;
		for (int32 i = 0; i < GlobalTasksArray->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>& Obj = (*GlobalTasksArray)[i]->AsObject();
			if (!Obj.IsValid()) continue;
			FString Id, Type;
			if (!Obj->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
				OutErrors.Add(FString::Printf(TEXT("global_tasks[%d]: missing 'id'"), i));
			if (!Obj->TryGetStringField(TEXT("type"), Type) || Type.IsEmpty())
				OutErrors.Add(FString::Printf(TEXT("global_tasks[%d]: missing 'type'"), i));
		}
	}

	if (!bHasContent)
	{
		OutErrors.Add(TEXT("StateTree spec must contain at least one of: 'states', 'evaluators', 'global_tasks'"));
		return false;
	}

	return OutErrors.Num() == 0;
}

bool FClaireonSpecApplicator_StateTree::OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return false;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	UStateTree* ST = ClaireonStateTreeHelpers::LoadStateTreeAsset(ResolvedPath, OutError);
	if (!ST)
	{
		return false;
	}

	UStateTreeEditorData* ED = ClaireonStateTreeHelpers::GetEditorData(ST, OutError);
	if (!ED)
	{
		return false;
	}

	const FString STPathName = ST->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		STPathName, TEXT("statetree_edit"));

	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		OutError = FString::Printf(TEXT("Asset is locked by %s session %s"),
			*Blocker.ToolName, *Blocker.SessionId);
		return false;
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		OutError = FString::Printf(TEXT("Invalid asset path: %s"), *STPathName);
		return false;
	}

	StateTree = ST;
	EditorData = ED;
	OutSessionId = OpenResult.SessionId;
	return true;
}

bool FClaireonSpecApplicator_StateTree::ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec)
{
	UStateTree* ST = StateTree.Get();
	UStateTreeEditorData* ED = EditorData.Get();
	if (!ST || !ED)
	{
		AddError(TEXT("StateTree or EditorData is no longer valid"));
		return false;
	}

	// --- Create states in dependency order (parent-first) ---
	const TArray<TSharedPtr<FJsonValue>>* StatesArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("states"), StatesArray) && StatesArray)
	{
		// Build dependency-ordered list
		TMap<FString, TSharedPtr<FJsonObject>> StateMap;
		for (const TSharedPtr<FJsonValue>& Val : *StatesArray)
		{
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
			if (!Obj.IsValid()) continue;
			FString Id;
			Obj->TryGetStringField(TEXT("id"), Id);
			StateMap.Add(Id, Obj);
		}

		// Topological sort: roots first, then children
		TArray<FString> Ordered;
		TSet<FString> Visited;
		TFunction<void(const FString&)> Visit = [&](const FString& Id)
		{
			if (Visited.Contains(Id)) return;
			Visited.Add(Id);

			const TSharedPtr<FJsonObject>* Found = StateMap.Find(Id);
			if (!Found) return;

			FString ParentId;
			if ((*Found)->TryGetStringField(TEXT("parent"), ParentId) && !ParentId.IsEmpty())
			{
				Visit(ParentId);
			}
			Ordered.Add(Id);
		};
		for (const auto& Pair : StateMap)
		{
			Visit(Pair.Key);
		}

		int32 SuccessCount = 0;
		for (const FString& StateId : Ordered)
		{
			const TSharedPtr<FJsonObject>& StateObj = StateMap[StateId];

			FString StateName;
			StateObj->TryGetStringField(TEXT("name"), StateName);

			FString ParentId;
			bool bHasParent = StateObj->TryGetStringField(TEXT("parent"), ParentId) && !ParentId.IsEmpty();

			UStateTreeState* ParentState = nullptr;
			if (bHasParent)
			{
				// Try id_map first (spec-internal forward reference); fall back to
				// external GUID lookup in the existing editor data.
				const FString ParentActualId = ResolveId(ParentId);
				if (!ParentActualId.IsEmpty())
				{
					FGuid ParentGuid;
					FGuid::Parse(ParentActualId, ParentGuid);
					ParentState = ClaireonStateTreeHelpers::FindStateById(ED, ParentGuid);
					if (!ParentState)
					{
						RecordEntryFailure(StateId, FString::Printf(TEXT("Parent state not found in editor data")));
						continue;
					}
				}
				else
				{
					FGuid ParentGuid;
					if (FGuid::Parse(ParentId, ParentGuid))
					{
						ParentState = ClaireonStateTreeHelpers::FindStateById(ED, ParentGuid);
					}
					if (!ParentState)
					{
						RecordEntryFailure(StateId, FString::Printf(
							TEXT("parent '%s' is neither in spec id_map nor in the existing tree"),
							*ParentId));
						continue;
					}
				}
			}

			if (ParentState)
			{
				UStateTreeState& NewState = ParentState->AddChildState(FName(*StateName));
				FString GuidStr = NewState.ID.ToString(EGuidFormats::DigitsWithHyphensLower);
				RegisterIdMapping(StateId, GuidStr);
				RecordEntrySuccess(StateId, GuidStr);
				SuccessCount++;
			}
			else
			{
				// Root state -- add as subtree
				UStateTreeState* NewState = NewObject<UStateTreeState>(ED, FName(*StateName));
				NewState->Name = FName(*StateName);
				ED->SubTrees.Add(NewState);
				FString GuidStr = NewState->ID.ToString(EGuidFormats::DigitsWithHyphensLower);
				RegisterIdMapping(StateId, GuidStr);
				RecordEntrySuccess(StateId, GuidStr);
				SuccessCount++;
			}
		}

		UE_LOG(LogClaireon, Log, TEXT("[apply_spec:StateTree] Pass 1: Created %d/%d states"),
			SuccessCount, StatesArray->Num());
	}

	// --- Create evaluators ---
	const TArray<TSharedPtr<FJsonValue>>* EvalArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("evaluators"), EvalArray) && EvalArray)
	{
		for (int32 i = 0; i < EvalArray->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>& Obj = (*EvalArray)[i]->AsObject();
			if (!Obj.IsValid()) continue;

			FString SpecId, NodeType;
			Obj->TryGetStringField(TEXT("id"), SpecId);
			Obj->TryGetStringField(TEXT("type"), NodeType);

			FString Error;
			UScriptStruct* NodeStruct = ClaireonStateTreeHelpers::ResolveNodeStruct(NodeType, Error);
			if (!NodeStruct)
			{
				RecordEntryFailure(SpecId, Error);
				continue;
			}

			FStateTreeEditorNode NewNode;
			if (!ClaireonStateTreeHelpers::CreateEditorNode(NewNode, NodeStruct, ED, Error))
			{
				RecordEntryFailure(SpecId, Error);
				continue;
			}

			FString NodeGuidStr = NewNode.ID.ToString(EGuidFormats::DigitsWithHyphensLower);
			ED->Evaluators.Add(MoveTemp(NewNode));
			RegisterIdMapping(SpecId, NodeGuidStr);
			RecordEntrySuccess(SpecId, NodeGuidStr);
		}
	}

	// --- Create global tasks ---
	const TArray<TSharedPtr<FJsonValue>>* GlobalTasksArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("global_tasks"), GlobalTasksArray) && GlobalTasksArray)
	{
		for (int32 i = 0; i < GlobalTasksArray->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>& Obj = (*GlobalTasksArray)[i]->AsObject();
			if (!Obj.IsValid()) continue;

			FString SpecId, NodeType;
			Obj->TryGetStringField(TEXT("id"), SpecId);
			Obj->TryGetStringField(TEXT("type"), NodeType);

			FString Error;
			UScriptStruct* NodeStruct = ClaireonStateTreeHelpers::ResolveNodeStruct(NodeType, Error);
			if (!NodeStruct)
			{
				RecordEntryFailure(SpecId, Error);
				continue;
			}

			FStateTreeEditorNode NewNode;
			if (!ClaireonStateTreeHelpers::CreateEditorNode(NewNode, NodeStruct, ED, Error))
			{
				RecordEntryFailure(SpecId, Error);
				continue;
			}

			FString NodeGuidStr = NewNode.ID.ToString(EGuidFormats::DigitsWithHyphensLower);
			ED->GlobalTasks.Add(MoveTemp(NewNode));
			RegisterIdMapping(SpecId, NodeGuidStr);
			RecordEntrySuccess(SpecId, NodeGuidStr);
		}
	}

	return true;
}

bool FClaireonSpecApplicator_StateTree::ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec)
{
	UStateTree* ST = StateTree.Get();
	UStateTreeEditorData* ED = EditorData.Get();
	if (!ST || !ED)
	{
		AddError(TEXT("StateTree or EditorData is no longer valid"));
		return false;
	}

	// --- Add tasks, conditions, transitions to states ---
	const TArray<TSharedPtr<FJsonValue>>* StatesArray = nullptr;
	if (!Spec->TryGetArrayField(TEXT("states"), StatesArray) || !StatesArray)
	{
		return true;
	}

	for (const TSharedPtr<FJsonValue>& StateVal : *StatesArray)
	{
		const TSharedPtr<FJsonObject>& StateObj = StateVal->AsObject();
		if (!StateObj.IsValid()) continue;

		FString SpecId;
		StateObj->TryGetStringField(TEXT("id"), SpecId);
		if (!IsIdCreated(SpecId)) continue;

		FString StateGuidStr = ResolveId(SpecId);
		FGuid StateGuid;
		FGuid::Parse(StateGuidStr, StateGuid);
		UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(ED, StateGuid);
		if (!State) continue;

		// --- Tasks ---
		const TArray<TSharedPtr<FJsonValue>>* TasksArray = nullptr;
		if (StateObj->TryGetArrayField(TEXT("tasks"), TasksArray) && TasksArray)
		{
			for (int32 t = 0; t < TasksArray->Num(); ++t)
			{
				const TSharedPtr<FJsonObject>& TaskObj = (*TasksArray)[t]->AsObject();
				if (!TaskObj.IsValid()) continue;

				FString TaskId, TaskType;
				TaskObj->TryGetStringField(TEXT("id"), TaskId);
				TaskObj->TryGetStringField(TEXT("type"), TaskType);

				FString Error;
				UScriptStruct* NodeStruct = ClaireonStateTreeHelpers::ResolveNodeStruct(TaskType, Error);
				if (!NodeStruct)
				{
					RecordEntryFailure(TaskId, Error);
					continue;
				}

				FStateTreeEditorNode NewNode;
				if (!ClaireonStateTreeHelpers::CreateEditorNode(NewNode, NodeStruct, State, Error))
				{
					RecordEntryFailure(TaskId, Error);
					continue;
				}

				// Set properties
				const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
				if (TaskObj->TryGetObjectField(TEXT("properties"), PropsPtr) && PropsPtr && (*PropsPtr).IsValid())
				{
					for (const auto& Prop : (*PropsPtr)->Values)
					{
						FString PropValue;
						if (Prop.Value->TryGetString(PropValue))
						{
							FString PropError;
							ClaireonStateTreeHelpers::SetNodeProperty(NewNode, Prop.Key, PropValue, false, PropError);
						}
					}
				}

				FString NodeGuidStr = NewNode.ID.ToString(EGuidFormats::DigitsWithHyphensLower);

				const UStateTreeSchema* Schema = ST->GetSchema();
				if (Schema && !Schema->AllowMultipleTasks())
				{
					State->SingleTask = MoveTemp(NewNode);
				}
				else
				{
					State->Tasks.Add(MoveTemp(NewNode));
				}

				RegisterIdMapping(TaskId, NodeGuidStr);
				RecordEntrySuccess(TaskId, NodeGuidStr);
			}
		}

		// --- Enter conditions ---
		const TArray<TSharedPtr<FJsonValue>>* ConditionsArray = nullptr;
		if (StateObj->TryGetArrayField(TEXT("enter_conditions"), ConditionsArray) && ConditionsArray)
		{
			for (int32 c = 0; c < ConditionsArray->Num(); ++c)
			{
				const TSharedPtr<FJsonObject>& CondObj = (*ConditionsArray)[c]->AsObject();
				if (!CondObj.IsValid()) continue;

				FString CondId, CondType;
				CondObj->TryGetStringField(TEXT("id"), CondId);
				CondObj->TryGetStringField(TEXT("type"), CondType);

				FString Error;
				UScriptStruct* NodeStruct = ClaireonStateTreeHelpers::ResolveNodeStruct(CondType, Error);
				if (!NodeStruct)
				{
					RecordEntryFailure(CondId, Error);
					continue;
				}

				FStateTreeEditorNode NewNode;
				if (!ClaireonStateTreeHelpers::CreateEditorNode(NewNode, NodeStruct, State, Error))
				{
					RecordEntryFailure(CondId, Error);
					continue;
				}

				FString NodeGuidStr = NewNode.ID.ToString(EGuidFormats::DigitsWithHyphensLower);
				State->EnterConditions.Add(MoveTemp(NewNode));
				RegisterIdMapping(CondId, NodeGuidStr);
				RecordEntrySuccess(CondId, NodeGuidStr);
			}
		}

		// --- Transitions ---
		const TArray<TSharedPtr<FJsonValue>>* TransitionsArray = nullptr;
		if (StateObj->TryGetArrayField(TEXT("transitions"), TransitionsArray) && TransitionsArray)
		{
			for (int32 tr = 0; tr < TransitionsArray->Num(); ++tr)
			{
				const TSharedPtr<FJsonObject>& TransObj = (*TransitionsArray)[tr]->AsObject();
				if (!TransObj.IsValid()) continue;

				FString TransId, TriggerStr, TargetStateSpecId;
				TransObj->TryGetStringField(TEXT("id"), TransId);
				TransObj->TryGetStringField(TEXT("trigger"), TriggerStr);
				TransObj->TryGetStringField(TEXT("target_state"), TargetStateSpecId);

				FStateTreeTransition NewTransition;

				// Parse trigger
				if (TriggerStr == TEXT("OnStateCompleted"))
					NewTransition.Trigger = EStateTreeTransitionTrigger::OnStateCompleted;
				else if (TriggerStr == TEXT("OnStateFailed"))
					NewTransition.Trigger = EStateTreeTransitionTrigger::OnStateFailed;
				else if (TriggerStr == TEXT("OnTick"))
					NewTransition.Trigger = EStateTreeTransitionTrigger::OnTick;
				else if (TriggerStr == TEXT("OnEvent"))
					NewTransition.Trigger = EStateTreeTransitionTrigger::OnEvent;

				// Resolve target state
				if (!TargetStateSpecId.IsEmpty())
				{
					FString TargetGuidStr = ResolveId(TargetStateSpecId);
					if (!TargetGuidStr.IsEmpty())
					{
						FGuid TargetGuid;
						FGuid::Parse(TargetGuidStr, TargetGuid);
						UStateTreeState* TargetState = ClaireonStateTreeHelpers::FindStateById(ED, TargetGuid);
						if (TargetState)
						{
							NewTransition.State = TargetState->GetLinkToState();
						}
					}
				}

				SpecApplicatorStateTree_ApplySpecTransitionFields(TransObj, NewTransition);

				FString TransGuidStr = NewTransition.ID.ToString(EGuidFormats::DigitsWithHyphensLower);
				State->Transitions.Add(MoveTemp(NewTransition));
				RegisterIdMapping(TransId, TransGuidStr);
				RecordEntrySuccess(TransId, TransGuidStr);
			}
		}
	}

	// --- Set properties on evaluators and global tasks ---
	auto SetNodeProperties = [&](const TArray<TSharedPtr<FJsonValue>>* SpecArray, TArray<FStateTreeEditorNode>& NodeArray)
	{
		if (!SpecArray) return;
		for (const TSharedPtr<FJsonValue>& Val : *SpecArray)
		{
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
			if (!Obj.IsValid()) continue;

			FString SpecNodeId;
			Obj->TryGetStringField(TEXT("id"), SpecNodeId);
			if (!IsIdCreated(SpecNodeId)) continue;

			const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
			if (!Obj->TryGetObjectField(TEXT("properties"), PropsPtr) || !PropsPtr || !(*PropsPtr).IsValid()) continue;

			FString NodeGuidStr = ResolveId(SpecNodeId);
			FGuid NodeGuid;
			FGuid::Parse(NodeGuidStr, NodeGuid);

			for (FStateTreeEditorNode& Node : NodeArray)
			{
				if (Node.ID == NodeGuid)
				{
					for (const auto& Prop : (*PropsPtr)->Values)
					{
						FString PropValue;
						if (Prop.Value->TryGetString(PropValue))
						{
							FString PropError;
							ClaireonStateTreeHelpers::SetNodeProperty(Node, Prop.Key, PropValue, false, PropError);
						}
					}
					break;
				}
			}
		}
	};

	const TArray<TSharedPtr<FJsonValue>>* EvalArray = nullptr;
	Spec->TryGetArrayField(TEXT("evaluators"), EvalArray);
	SetNodeProperties(EvalArray, ED->Evaluators);

	const TArray<TSharedPtr<FJsonValue>>* GlobalTasksArray = nullptr;
	Spec->TryGetArrayField(TEXT("global_tasks"), GlobalTasksArray);
	SetNodeProperties(GlobalTasksArray, ED->GlobalTasks);

	// --- Top-level transitions: { "from": specId, "to": specId, "trigger": "OnStateCompleted" } ---
	// Per-state transitions (states[].transitions) are processed above; this top-level array is
	// useful when authoring transitions independently of state definitions.
	const TArray<TSharedPtr<FJsonValue>>* TopTransitionsArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("transitions"), TopTransitionsArray) && TopTransitionsArray)
	{
		for (int32 tr = 0; tr < TopTransitionsArray->Num(); ++tr)
		{
			const TSharedPtr<FJsonObject>& TransObj = (*TopTransitionsArray)[tr]->AsObject();
			if (!TransObj.IsValid()) continue;

			FString FromSpecId, ToSpecId, TriggerStr, TransSpecId;
			TransObj->TryGetStringField(TEXT("from"), FromSpecId);
			TransObj->TryGetStringField(TEXT("to"), ToSpecId);
			TransObj->TryGetStringField(TEXT("trigger"), TriggerStr);
			TransObj->TryGetStringField(TEXT("id"), TransSpecId);

			if (FromSpecId.IsEmpty())
			{
				AddWarning(FString::Printf(TEXT("transitions[%d]: missing 'from'"), tr));
				continue;
			}

			if (!IsIdCreated(FromSpecId))
			{
				AddWarning(FString::Printf(TEXT("transitions[%d]: source state '%s' not found in id_map"), tr, *FromSpecId));
				continue;
			}

			FString FromGuidStr = ResolveId(FromSpecId);
			FGuid FromGuid;
			FGuid::Parse(FromGuidStr, FromGuid);
			UStateTreeState* FromState = ClaireonStateTreeHelpers::FindStateById(ED, FromGuid);
			if (!FromState)
			{
				AddWarning(FString::Printf(TEXT("transitions[%d]: source state '%s' resolved but not found in tree"), tr, *FromSpecId));
				continue;
			}

			FStateTreeTransition NewTransition;

			if (TriggerStr == TEXT("OnStateCompleted"))
				NewTransition.Trigger = EStateTreeTransitionTrigger::OnStateCompleted;
			else if (TriggerStr == TEXT("OnStateFailed"))
				NewTransition.Trigger = EStateTreeTransitionTrigger::OnStateFailed;
			else if (TriggerStr == TEXT("OnTick"))
				NewTransition.Trigger = EStateTreeTransitionTrigger::OnTick;
			else if (TriggerStr == TEXT("OnEvent"))
				NewTransition.Trigger = EStateTreeTransitionTrigger::OnEvent;

			if (!ToSpecId.IsEmpty())
			{
				if (!IsIdCreated(ToSpecId))
				{
					AddWarning(FString::Printf(TEXT("transitions[%d]: target state '%s' not found in id_map"), tr, *ToSpecId));
					continue;
				}
				FString ToGuidStr = ResolveId(ToSpecId);
				FGuid ToGuid;
				FGuid::Parse(ToGuidStr, ToGuid);
				if (UStateTreeState* TargetState = ClaireonStateTreeHelpers::FindStateById(ED, ToGuid))
				{
					NewTransition.State = TargetState->GetLinkToState();
				}
			}

			SpecApplicatorStateTree_ApplySpecTransitionFields(TransObj, NewTransition);

			FString TransGuidStr = NewTransition.ID.ToString(EGuidFormats::DigitsWithHyphensLower);
			FromState->Transitions.Add(MoveTemp(NewTransition));

			if (!TransSpecId.IsEmpty())
			{
				RegisterIdMapping(TransSpecId, TransGuidStr);
				RecordEntrySuccess(TransSpecId, TransGuidStr);
			}
		}
	}

	return true;
}

bool FClaireonSpecApplicator_StateTree::CompileAsset(const FString& SessionId, FString& OutError)
{
	UStateTree* ST = StateTree.Get();
	if (!ST)
	{
		OutError = TEXT("StateTree is no longer valid");
		return false;
	}

	FStateTreeCompilerLog CompilerLog;
	bool bSuccess = UStateTreeEditingSubsystem::CompileStateTree(ST, CompilerLog);
	if (!bSuccess)
	{
		OutError = TEXT("StateTree compilation failed");
		return false;
	}

	return true;
}

bool FClaireonSpecApplicator_StateTree::SaveAsset(const FString& SessionId, FString& OutError)
{
	UStateTree* ST = StateTree.Get();
	if (!ST)
	{
		OutError = TEXT("StateTree is no longer valid");
		return false;
	}

	UPackage* Package = ST->GetPackage();
	Package->SetDirtyFlag(true);

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);

	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		OutError = TEXT("Save blocked: editor state may be corrupted after a previous crash");
		return false;
	}

	bool bSuccess = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);
	if (!bSuccess)
	{
		OutError = TEXT("Failed to save StateTree package");
		return false;
	}

	return true;
}

void FClaireonSpecApplicator_StateTree::CloseSession(const FString& SessionId)
{
	FClaireonSessionManager::Get().CloseSession(SessionId);
}
