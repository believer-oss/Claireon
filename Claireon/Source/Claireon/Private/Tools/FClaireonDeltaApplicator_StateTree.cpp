// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/FClaireonDeltaApplicator_StateTree.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/ClaireonStateTreeEditToolBase.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"
#include "GameplayTagContainer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonDeltaApplicator_StateTree_anon
{
	static bool STDelta_TryGetObject(const TSharedPtr<FJsonValue>& Entry, TSharedPtr<FJsonObject>& OutObj)
	{
		if (!Entry.IsValid() || Entry->Type != EJson::Object) { return false; }
		OutObj = Entry->AsObject();
		return OutObj.IsValid();
	}

	static EStateTreeTransitionTrigger STDelta_ParseTrigger(const FString& Trig)
	{
		if (Trig == TEXT("OnStateCompleted")) return EStateTreeTransitionTrigger::OnStateCompleted;
		if (Trig == TEXT("OnStateSucceeded")) return EStateTreeTransitionTrigger::OnStateSucceeded;
		if (Trig == TEXT("OnStateFailed")) return EStateTreeTransitionTrigger::OnStateFailed;
		if (Trig == TEXT("OnTick")) return EStateTreeTransitionTrigger::OnTick;
		if (Trig == TEXT("OnEvent")) return EStateTreeTransitionTrigger::OnEvent;
		return EStateTreeTransitionTrigger::OnStateCompleted;
	}

	/**
	 * Walks all states (subtrees and their descendants) and invokes Visit(State).
	 * Visit may return false to stop traversal.
	 */
	static void STDelta_WalkStates(UStateTreeEditorData* ED, TFunctionRef<bool(UStateTreeState*)> Visit)
	{
		if (!ED) { return; }
		TArray<UStateTreeState*> Stack;
		for (UStateTreeState* Root : ED->SubTrees) { if (Root) { Stack.Add(Root); } }
		while (Stack.Num() > 0)
		{
			UStateTreeState* S = Stack.Pop(EAllowShrinking::No);
			if (!S) { continue; }
			if (!Visit(S)) { return; }
			for (UStateTreeState* Child : S->Children) { if (Child) { Stack.Add(Child); } }
		}
	}

	/** Remove a transition by id; returns true if removed. */
	static bool STDelta_RemoveTransitionById(UStateTreeEditorData* ED, const FGuid& TransitionId)
	{
		bool bRemoved = false;
		STDelta_WalkStates(ED, [&](UStateTreeState* S) -> bool
		{
			const int32 Idx = S->Transitions.IndexOfByPredicate([&](const FStateTreeTransition& T)
			{
				return T.ID == TransitionId;
			});
			if (Idx != INDEX_NONE)
			{
				S->Transitions.RemoveAt(Idx);
				bRemoved = true;
				return false;
			}
			return true;
		});
		return bRemoved;
	}

	/** Resolve a state ref via id_map (local) or GUID lookup. */
	static UStateTreeState* STDelta_ResolveState(
		UStateTreeEditorData* ED,
		const FString& Resolved)
	{
		FGuid Guid;
		if (!FGuid::Parse(Resolved, Guid)) { return nullptr; }
		return ClaireonStateTreeHelpers::FindStateById(ED, Guid);
	}
}

TArray<TSharedPtr<FJsonValue>> FClaireonDeltaApplicator_StateTree::UnionAndDedupeTransitions(
	const TSharedPtr<FJsonObject>& Args,
	const TArray<TSharedPtr<FJsonValue>>& Phase4Entries) const
{
	using namespace ClaireonDeltaApplicator_StateTree_anon;

	auto MakeKey = [](const TSharedPtr<FJsonObject>& Obj, FString& OutId, FString& OutFromTo)
	{
		FString From, To;
		Obj->TryGetStringField(TEXT("id"), OutId);
		Obj->TryGetStringField(TEXT("from_state"), From);
		Obj->TryGetStringField(TEXT("to_state"), To);
		OutFromTo = FString::Printf(TEXT("%s->%s"), *From, *To);
	};

	TArray<TSharedPtr<FJsonValue>> Result;
	TSet<FString> SeenIds;
	TSet<FString> SeenPairs;

	// Top-level transitions[] wins on ties; ingest it first.
	const TArray<TSharedPtr<FJsonValue>>* TopArr = nullptr;
	if (Args.IsValid() && Args->TryGetArrayField(TEXT("transitions"), TopArr) && TopArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *TopArr)
		{
			TSharedPtr<FJsonObject> Obj;
			if (!STDelta_TryGetObject(V, Obj)) { continue; }
			FString Id, FromTo;
			MakeKey(Obj, Id, FromTo);
			if (!Id.IsEmpty()) { SeenIds.Add(Id); }
			else { SeenPairs.Add(FromTo); }
			Result.Add(V);
		}
	}

	for (const TSharedPtr<FJsonValue>& V : Phase4Entries)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!STDelta_TryGetObject(V, Obj)) { continue; }
		FString Id, FromTo;
		MakeKey(Obj, Id, FromTo);
		const bool bDupId = !Id.IsEmpty() && SeenIds.Contains(Id);
		const bool bDupPair = Id.IsEmpty() && SeenPairs.Contains(FromTo);
		if (bDupId || bDupPair)
		{
			continue;
		}
		if (!Id.IsEmpty()) { SeenIds.Add(Id); }
		else { SeenPairs.Add(FromTo); }
		Result.Add(V);
	}
	return Result;
}

bool FClaireonDeltaApplicator_StateTree::ValidateArgs(const TSharedPtr<FJsonObject>& Args, TArray<FString>& OutErrors)
{
	using namespace ClaireonDeltaApplicator_StateTree_anon;

	// Reject transition entries in phase 2 (remove_nodes).
	const TArray<TSharedPtr<FJsonValue>>* RemoveArr = nullptr;
	if (Args->TryGetArrayField(TEXT("remove_nodes"), RemoveArr) && RemoveArr)
	{
		for (int32 i = 0; i < RemoveArr->Num(); ++i)
		{
			TSharedPtr<FJsonObject> Obj;
			if (!STDelta_TryGetObject((*RemoveArr)[i], Obj)) { continue; }
			FString Kind;
			Obj->TryGetStringField(TEXT("kind"), Kind);
			if (Kind == TEXT("transition"))
			{
				OutErrors.Add(TEXT("statetree_apply_delta: 'transition' kind is not allowed in remove_nodes[]; use disconnect[] to remove transitions."));
				return false;
			}
		}
	}

	// Validate that, if present, the top-level transitions[] is an array.
	if (Args.IsValid() && Args->HasField(TEXT("transitions")))
	{
		const TArray<TSharedPtr<FJsonValue>>* TopArr = nullptr;
		if (!Args->TryGetArrayField(TEXT("transitions"), TopArr))
		{
			OutErrors.Add(TEXT("statetree_apply_delta: 'transitions' (top-level) must be an array"));
			return false;
		}
	}
	return true;
}

bool FClaireonDeltaApplicator_StateTree::OpenOrReuseSession(const TSharedPtr<FJsonObject>& Args, FString& OutSessionId, FString& OutError)
{
	CachedStateTree.Reset();
	CachedEditorData.Reset();
	CreatedStateIdsThisCall.Reset();
	CreatedTransitionIdsThisCall.Reset();
	CreatedEvaluatorIdsThisCall.Reset();
	CreatedGlobalTaskIdsThisCall.Reset();

	FString SessionIdArg;
	const bool bHasSessionId = Args->TryGetStringField(TEXT("session_id"), SessionIdArg) && !SessionIdArg.IsEmpty();
	if (bHasSessionId)
	{
		FStateTreeEditToolData* Data = ClaireonStateTreeEditToolBase::ToolData.Find(SessionIdArg);
		if (!Data || !Data->IsValid())
		{
			OutError = FString::Printf(TEXT("statetree_apply_delta: session_id '%s' not found"), *SessionIdArg);
			return false;
		}
		UStateTree* ST = Data->StateTree.Get();
		FString GetEDError;
		UStateTreeEditorData* ED = ClaireonStateTreeHelpers::GetEditorData(ST, GetEDError);
		if (!ED)
		{
			OutError = GetEDError;
			return false;
		}
		CachedStateTree = ST;
		CachedEditorData = ED;
		OutSessionId = SessionIdArg;
		return true;
	}

	FString AssetPathArg;
	if (!Args->TryGetStringField(TEXT("asset_path"), AssetPathArg) || AssetPathArg.IsEmpty())
	{
		OutError = TEXT("statetree_apply_delta: missing asset_path");
		return false;
	}

	UStateTree* ST = ClaireonStateTreeHelpers::LoadStateTreeAsset(AssetPathArg, OutError);
	if (!ST) { return false; }
	UStateTreeEditorData* ED = ClaireonStateTreeHelpers::GetEditorData(ST, OutError);
	if (!ED) { return false; }

	ClaireonStateTreeEditToolBase::EnsureDelegateRegistered();

	const FString ResolvedAssetPath = ST->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		ResolvedAssetPath, ClaireonStateTreeEditToolBase::StateTreeSessionToolName);
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		OutError = FString::Printf(TEXT("statetree_apply_delta: asset is locked by %s session %s"),
			*Blocker.ToolName, *Blocker.SessionId);
		return false;
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		OutError = FString::Printf(TEXT("statetree_apply_delta: invalid asset path: %s"), *ResolvedAssetPath);
		return false;
	}

	FStateTreeEditToolData NewData;
	NewData.StateTree = ST;
	NewData.LastOperationStatus = TEXT("apply_delta opened");
	ClaireonStateTreeEditToolBase::ToolData.Add(OpenResult.SessionId, MoveTemp(NewData));

	CachedStateTree = ST;
	CachedEditorData = ED;
	OutSessionId = OpenResult.SessionId;
	return true;
}

bool FClaireonDeltaApplicator_StateTree::ApplyPhase1_Disconnect(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_StateTree_anon;
	(void)SessionId;
	UStateTreeEditorData* ED = CachedEditorData.Get();
	if (!ED)
	{
		AddError(TEXT("statetree_apply_delta: editor data is no longer valid"));
		return false;
	}
	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		FString IdStr;
		if (Entries[i].IsValid() && Entries[i]->Type == EJson::String)
		{
			IdStr = Entries[i]->AsString();
		}
		else
		{
			TSharedPtr<FJsonObject> Obj;
			if (STDelta_TryGetObject(Entries[i], Obj))
			{
				Obj->TryGetStringField(TEXT("id"), IdStr);
			}
		}
		if (IdStr.IsEmpty())
		{
			AddError(FString::Printf(TEXT("statetree_apply_delta: disconnect[%d] requires a transition id string"), i));
			return false;
		}
		const FString Resolved = ResolveLocalId(IdStr);
		FGuid TransGuid;
		if (!FGuid::Parse(Resolved, TransGuid))
		{
			AddError(FString::Printf(TEXT("statetree_apply_delta: disconnect[%d]: invalid transition GUID '%s'"), i, *IdStr));
			return false;
		}
		const bool bRemoved = STDelta_RemoveTransitionById(ED, TransGuid);
		if (!bRemoved)
		{
			AddError(FString::Printf(TEXT("statetree_apply_delta: disconnect[%d]: transition '%s' not found"), i, *IdStr));
			return false;
		}
		MarkRemoved();
		RecordAffected(Resolved);
	}
	return true;
}

bool FClaireonDeltaApplicator_StateTree::ApplyPhase2_Remove(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_StateTree_anon;
	(void)SessionId;
	UStateTreeEditorData* ED = CachedEditorData.Get();
	if (!ED)
	{
		AddError(TEXT("statetree_apply_delta: editor data is no longer valid"));
		return false;
	}
	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!STDelta_TryGetObject(Entries[i], Obj))
		{
			AddError(FString::Printf(TEXT("statetree_apply_delta: remove_nodes[%d] must be an object"), i));
			return false;
		}
		FString Kind, IdStr;
		Obj->TryGetStringField(TEXT("kind"), Kind);
		Obj->TryGetStringField(TEXT("id"), IdStr);
		if (Kind.IsEmpty() || IdStr.IsEmpty())
		{
			AddError(FString::Printf(TEXT("statetree_apply_delta: remove_nodes[%d] requires 'kind' and 'id'"), i));
			return false;
		}
		const FString Resolved = ResolveLocalId(IdStr);
		FGuid Guid;
		if (!FGuid::Parse(Resolved, Guid))
		{
			AddError(FString::Printf(TEXT("statetree_apply_delta: remove_nodes[%d]: invalid GUID '%s'"), i, *IdStr));
			return false;
		}
		if (Kind == TEXT("state"))
		{
			UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(ED, Guid);
			if (!State)
			{
				AddError(FString::Printf(TEXT("statetree_apply_delta: remove_nodes[%d]: state '%s' not found"), i, *IdStr));
				return false;
			}
			UStateTreeState* Parent = Cast<UStateTreeState>(State->GetOuter());
			if (Parent) { Parent->Children.Remove(State); }
			else { ED->SubTrees.Remove(State); }
			MarkRemoved();
			RecordAffected(Resolved);
		}
		else if (Kind == TEXT("evaluator"))
		{
			const int32 Idx = ED->Evaluators.IndexOfByPredicate([&](const FStateTreeEditorNode& N) { return N.ID == Guid; });
			if (Idx == INDEX_NONE)
			{
				AddError(FString::Printf(TEXT("statetree_apply_delta: remove_nodes[%d]: evaluator '%s' not found"), i, *IdStr));
				return false;
			}
			ED->Evaluators.RemoveAt(Idx);
			MarkRemoved();
			RecordAffected(Resolved);
		}
		else if (Kind == TEXT("global_task"))
		{
			const int32 Idx = ED->GlobalTasks.IndexOfByPredicate([&](const FStateTreeEditorNode& N) { return N.ID == Guid; });
			if (Idx == INDEX_NONE)
			{
				AddError(FString::Printf(TEXT("statetree_apply_delta: remove_nodes[%d]: global_task '%s' not found"), i, *IdStr));
				return false;
			}
			ED->GlobalTasks.RemoveAt(Idx);
			MarkRemoved();
			RecordAffected(Resolved);
		}
		else
		{
			AddError(FString::Printf(TEXT("statetree_apply_delta: remove_nodes[%d]: unknown kind '%s'"), i, *Kind));
			return false;
		}
	}
	return true;
}

bool FClaireonDeltaApplicator_StateTree::ApplyPhase3_Create(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_StateTree_anon;
	(void)SessionId;
	UStateTreeEditorData* ED = CachedEditorData.Get();
	if (!ED)
	{
		AddError(TEXT("statetree_apply_delta: editor data is no longer valid"));
		return false;
	}
	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!STDelta_TryGetObject(Entries[i], Obj))
		{
			AddError(FString::Printf(TEXT("statetree_apply_delta: nodes[%d] must be an object"), i));
			return false;
		}
		FString Kind, LocalId;
		Obj->TryGetStringField(TEXT("kind"), Kind);
		Obj->TryGetStringField(TEXT("id"), LocalId);
		if (Kind.IsEmpty() || LocalId.IsEmpty())
		{
			AddError(FString::Printf(TEXT("statetree_apply_delta: nodes[%d] requires 'kind' and 'id'"), i));
			return false;
		}
		if (Kind == TEXT("state"))
		{
			FString Name;
			Obj->TryGetStringField(TEXT("name"), Name);
			if (Name.IsEmpty()) { Name = LocalId; }

			FString ParentId;
			const bool bHasParent = Obj->TryGetStringField(TEXT("parent_id"), ParentId) && !ParentId.IsEmpty();
			UStateTreeState* ParentState = nullptr;
			if (bHasParent)
			{
				ParentState = STDelta_ResolveState(ED, ResolveLocalId(ParentId));
				if (!ParentState)
				{
					AddError(FString::Printf(TEXT("statetree_apply_delta: nodes[%d]: parent_id '%s' not found"), i, *ParentId));
					return false;
				}
			}

			FGuid NewGuid;
			if (ParentState)
			{
				UStateTreeState& NewState = ParentState->AddChildState(FName(*Name));
				NewGuid = NewState.ID;
			}
			else
			{
				UStateTreeState* NewState = NewObject<UStateTreeState>(ED, FName(*Name));
				NewState->Name = FName(*Name);
				ED->SubTrees.Add(NewState);
				NewGuid = NewState->ID;
			}
			CreatedStateIdsThisCall.Add(NewGuid);
			const FString GuidStr = NewGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
			RegisterIdMapping(LocalId, GuidStr);
			MarkCreated();
			RecordAffected(GuidStr);
		}
		else if (Kind == TEXT("evaluator") || Kind == TEXT("global_task"))
		{
			FString Type;
			Obj->TryGetStringField(TEXT("type"), Type);
			if (Type.IsEmpty())
			{
				AddError(FString::Printf(TEXT("statetree_apply_delta: nodes[%d]: '%s' requires 'type'"), i, *Kind));
				return false;
			}
			FString ResolveError;
			UScriptStruct* NodeStruct = ClaireonStateTreeHelpers::ResolveNodeStruct(Type, ResolveError);
			if (!NodeStruct)
			{
				AddError(FString::Printf(TEXT("statetree_apply_delta: nodes[%d]: %s"), i, *ResolveError));
				return false;
			}
			FStateTreeEditorNode NewNode;
			FString CreateError;
			if (!ClaireonStateTreeHelpers::CreateEditorNode(NewNode, NodeStruct, ED, CreateError))
			{
				AddError(FString::Printf(TEXT("statetree_apply_delta: nodes[%d]: %s"), i, *CreateError));
				return false;
			}
			const FGuid NewGuid = NewNode.ID;
			if (Kind == TEXT("evaluator"))
			{
				ED->Evaluators.Add(MoveTemp(NewNode));
				CreatedEvaluatorIdsThisCall.Add(NewGuid);
			}
			else
			{
				ED->GlobalTasks.Add(MoveTemp(NewNode));
				CreatedGlobalTaskIdsThisCall.Add(NewGuid);
			}
			const FString GuidStr = NewGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
			RegisterIdMapping(LocalId, GuidStr);
			MarkCreated();
			RecordAffected(GuidStr);
		}
		else
		{
			AddError(FString::Printf(TEXT("statetree_apply_delta: nodes[%d]: unknown kind '%s'"), i, *Kind));
			return false;
		}
	}
	return true;
}

bool FClaireonDeltaApplicator_StateTree::ApplyPhase4_Connect(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_StateTree_anon;
	(void)SessionId;
	UStateTreeEditorData* ED = CachedEditorData.Get();
	if (!ED)
	{
		AddError(TEXT("statetree_apply_delta: editor data is no longer valid"));
		return false;
	}

	// AR4: union top-level transitions[] with phase-4 connections[], dedup per M3.
	const TArray<TSharedPtr<FJsonValue>> Union = UnionAndDedupeTransitions(GetCachedArgs(), Entries);

	for (int32 i = 0; i < Union.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!STDelta_TryGetObject(Union[i], Obj))
		{
			AddError(FString::Printf(TEXT("statetree_apply_delta: connections[%d] must be an object"), i));
			return false;
		}
		FString FromRef, ToRef, TrigStr;
		Obj->TryGetStringField(TEXT("from_state"), FromRef);
		Obj->TryGetStringField(TEXT("to_state"), ToRef);
		Obj->TryGetStringField(TEXT("trigger"), TrigStr);
		if (FromRef.IsEmpty() || ToRef.IsEmpty())
		{
			AddError(FString::Printf(TEXT("statetree_apply_delta: connections[%d] requires 'from_state' and 'to_state'"), i));
			return false;
		}
		UStateTreeState* FromState = STDelta_ResolveState(ED, ResolveLocalId(FromRef));
		if (!FromState)
		{
			AddError(FString::Printf(TEXT("statetree_apply_delta: connections[%d]: from_state '%s' not found"), i, *FromRef));
			return false;
		}
		UStateTreeState* ToState = STDelta_ResolveState(ED, ResolveLocalId(ToRef));
		if (!ToState)
		{
			AddError(FString::Printf(TEXT("statetree_apply_delta: connections[%d]: to_state '%s' not found"), i, *ToRef));
			return false;
		}

		const EStateTreeTransitionTrigger Trigger = TrigStr.IsEmpty()
			? EStateTreeTransitionTrigger::OnStateCompleted
			: STDelta_ParseTrigger(TrigStr);

		FStateTreeTransition& NewTrans = FromState->AddTransition(Trigger, EStateTreeTransitionType::GotoState, ToState);

		// Optional id override -- if absent, the AddTransition() assigned a fresh GUID.
		FString LocalId;
		Obj->TryGetStringField(TEXT("id"), LocalId);
		const FString GuidStr = NewTrans.ID.ToString(EGuidFormats::DigitsWithHyphensLower);
		if (!LocalId.IsEmpty())
		{
			RegisterIdMapping(LocalId, GuidStr);
		}
		CreatedTransitionIdsThisCall.Add(NewTrans.ID);
		MarkConnection();
		RecordAffected(GuidStr);
	}
	return true;
}

void FClaireonDeltaApplicator_StateTree::FinalizeSession(const FString& SessionId)
{
	(void)SessionId;
	// No compile step here -- compile is a separate explicit tool. The session stays open.
}

void FClaireonDeltaApplicator_StateTree::CloseSessionIfOwned(const FString& SessionId)
{
	if (DoesOwnSession() && !SessionId.IsEmpty())
	{
		ClaireonStateTreeEditToolBase::ToolData.Remove(SessionId);
		FClaireonSessionManager::Get().CloseSession(SessionId);
	}
}

void FClaireonDeltaApplicator_StateTree::Phase3CleanupOnFailure(const FString& SessionId)
{
	using namespace ClaireonDeltaApplicator_StateTree_anon;
	(void)SessionId;
	UStateTreeEditorData* ED = CachedEditorData.Get();
	if (!ED) { return; }

	// Remove transitions first (children of states), then states, then evaluators / global tasks.
	for (const FGuid& Id : CreatedTransitionIdsThisCall)
	{
		STDelta_RemoveTransitionById(ED, Id);
	}
	for (const FGuid& Id : CreatedStateIdsThisCall)
	{
		UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(ED, Id);
		if (!State) { continue; }
		UStateTreeState* Parent = Cast<UStateTreeState>(State->GetOuter());
		if (Parent) { Parent->Children.Remove(State); }
		else { ED->SubTrees.Remove(State); }
	}
	for (const FGuid& Id : CreatedEvaluatorIdsThisCall)
	{
		const int32 Idx = ED->Evaluators.IndexOfByPredicate([&](const FStateTreeEditorNode& N) { return N.ID == Id; });
		if (Idx != INDEX_NONE) { ED->Evaluators.RemoveAt(Idx); }
	}
	for (const FGuid& Id : CreatedGlobalTaskIdsThisCall)
	{
		const int32 Idx = ED->GlobalTasks.IndexOfByPredicate([&](const FStateTreeEditorNode& N) { return N.ID == Id; });
		if (Idx != INDEX_NONE) { ED->GlobalTasks.RemoveAt(Idx); }
	}
	CreatedTransitionIdsThisCall.Reset();
	CreatedStateIdsThisCall.Reset();
	CreatedEvaluatorIdsThisCall.Reset();
	CreatedGlobalTaskIdsThisCall.Reset();
}
