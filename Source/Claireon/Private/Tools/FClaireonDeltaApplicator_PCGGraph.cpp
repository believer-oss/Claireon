// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/FClaireonDeltaApplicator_PCGGraph.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/ClaireonPCGGraphEditToolBase.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonDeltaApplicator_PCG_anon
{
	static bool PCGDelta_TryGetObject(const TSharedPtr<FJsonValue>& Entry, TSharedPtr<FJsonObject>& OutObj)
	{
		if (!Entry.IsValid() || Entry->Type != EJson::Object) { return false; }
		OutObj = Entry->AsObject();
		return OutObj.IsValid();
	}

	static bool PCGDelta_HasExactOutputPin(const UPCGNode* Node, const FString& Label)
	{
		if (!Node) { return false; }
		for (const TObjectPtr<UPCGPin>& Pin : Node->GetOutputPins())
		{
			if (Pin && Pin->Properties.Label.ToString() == Label) { return true; }
		}
		return false;
	}

	static bool PCGDelta_HasExactInputPin(const UPCGNode* Node, const FString& Label)
	{
		if (!Node) { return false; }
		for (const TObjectPtr<UPCGPin>& Pin : Node->GetInputPins())
		{
			if (Pin && Pin->Properties.Label.ToString() == Label) { return true; }
		}
		return false;
	}
}

UPCGNode* FClaireonDeltaApplicator_PCGGraph::ResolveNodeRef(
	UPCGGraph* Graph,
	const FString& Ref,
	FString& OutError) const
{
	if (Ref.IsEmpty())
	{
		OutError = TEXT("pcg_apply_delta: empty node reference");
		return nullptr;
	}
	const FString Resolved = const_cast<FClaireonDeltaApplicator_PCGGraph*>(this)->ResolveLocalId(Ref);

	int32 Index = INDEX_NONE;
	if (UPCGNode* Node = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Graph, Resolved, Index))
	{
		return Node;
	}
	OutError = FString::Printf(TEXT("pcg_apply_delta: node reference '%s' not found"), *Ref);
	return nullptr;
}

bool FClaireonDeltaApplicator_PCGGraph::ValidateArgs(const TSharedPtr<FJsonObject>& Args, TArray<FString>& OutErrors)
{
	(void)Args; (void)OutErrors;
	return true;
}

bool FClaireonDeltaApplicator_PCGGraph::OpenOrReuseSession(const TSharedPtr<FJsonObject>& Args, FString& OutSessionId, FString& OutError)
{
	CreatedNodesThisCall.Reset();
	CachedGraph.Reset();

	FString SessionIdArg;
	const bool bHasSessionId = Args->TryGetStringField(TEXT("session_id"), SessionIdArg) && !SessionIdArg.IsEmpty();
	if (bHasSessionId)
	{
		FPCGGraphEditToolData* Data = ClaireonPCGGraphEditToolBase::ToolData.Find(SessionIdArg);
		if (!Data || !Data->IsValid())
		{
			OutError = FString::Printf(TEXT("pcg_apply_delta: session_id '%s' not found"), *SessionIdArg);
			return false;
		}
		CachedGraph = Data->PCGGraph;
		OutSessionId = SessionIdArg;
		return true;
	}

	FString AssetPathArg;
	if (!Args->TryGetStringField(TEXT("asset_path"), AssetPathArg) || AssetPathArg.IsEmpty())
	{
		OutError = TEXT("pcg_apply_delta: missing asset_path");
		return false;
	}

	UPCGGraph* Graph = ClaireonPCGGraphHelpers::LoadPCGGraphAsset(AssetPathArg, OutError);
	if (!Graph) { return false; }

	ClaireonPCGGraphEditToolBase::EnsureDelegateRegistered();

	const FString ResolvedAssetPath = Graph->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		ResolvedAssetPath, ClaireonPCGGraphEditToolBase::PCGSessionToolName);
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		OutError = FString::Printf(TEXT("pcg_apply_delta: asset is locked by %s session %s"),
			*Blocker.ToolName, *Blocker.SessionId);
		return false;
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		OutError = FString::Printf(TEXT("pcg_apply_delta: invalid asset path: %s"), *ResolvedAssetPath);
		return false;
	}

	FPCGGraphEditToolData NewData;
	NewData.PCGGraph = Graph;
	NewData.LastOperationStatus = TEXT("apply_delta opened");
	ClaireonPCGGraphEditToolBase::ToolData.Add(OpenResult.SessionId, MoveTemp(NewData));

	CachedGraph = Graph;
	OutSessionId = OpenResult.SessionId;
	return true;
}

bool FClaireonDeltaApplicator_PCGGraph::ApplyPhase1_Disconnect(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_PCG_anon;
	(void)SessionId;
	UPCGGraph* Graph = CachedGraph.Get();
	if (!Graph)
	{
		AddError(TEXT("pcg_apply_delta: graph is no longer valid"));
		return false;
	}

	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!PCGDelta_TryGetObject(Entries[i], Obj))
		{
			AddError(FString::Printf(TEXT("pcg_apply_delta: disconnect[%d] must be an object"), i));
			return false;
		}
		FString SN, SP, TN, TP;
		Obj->TryGetStringField(TEXT("source_node"), SN);
		Obj->TryGetStringField(TEXT("source_pin"), SP);
		Obj->TryGetStringField(TEXT("target_node"), TN);
		Obj->TryGetStringField(TEXT("target_pin"), TP);
		if (SN.IsEmpty() || SP.IsEmpty() || TN.IsEmpty() || TP.IsEmpty())
		{
			AddError(FString::Printf(TEXT("pcg_apply_delta: disconnect[%d] requires source_node, source_pin, target_node, target_pin"), i));
			return false;
		}
		FString ResolveError;
		UPCGNode* SourceNode = ResolveNodeRef(Graph, SN, ResolveError);
		if (!SourceNode) { AddError(ResolveError); return false; }
		UPCGNode* TargetNode = ResolveNodeRef(Graph, TN, ResolveError);
		if (!TargetNode) { AddError(ResolveError); return false; }

		// Exact-match pin name validation
		if (!PCGDelta_HasExactOutputPin(SourceNode, SP))
		{
			AddError(FString::Printf(TEXT("pcg_apply_delta: disconnect[%d]: source output pin '%s' not found (exact-match) on '%s'"),
				i, *SP, *ClaireonPCGGraphHelpers::GetNodeDisplayName(SourceNode)));
			return false;
		}
		if (!PCGDelta_HasExactInputPin(TargetNode, TP))
		{
			AddError(FString::Printf(TEXT("pcg_apply_delta: disconnect[%d]: target input pin '%s' not found (exact-match) on '%s'"),
				i, *TP, *ClaireonPCGGraphHelpers::GetNodeDisplayName(TargetNode)));
			return false;
		}

		const bool bRemoved = Graph->RemoveEdge(SourceNode, FName(*SP), TargetNode, FName(*TP));
		if (!bRemoved)
		{
			AddError(FString::Printf(TEXT("pcg_apply_delta: disconnect[%d]: no matching edge to remove"), i));
			return false;
		}
		RecordAffected(ClaireonPCGGraphHelpers::GetNodeDisplayName(SourceNode));
		RecordAffected(ClaireonPCGGraphHelpers::GetNodeDisplayName(TargetNode));
	}
	return true;
}

bool FClaireonDeltaApplicator_PCGGraph::ApplyPhase2_Remove(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_PCG_anon;
	(void)SessionId;
	UPCGGraph* Graph = CachedGraph.Get();
	if (!Graph)
	{
		AddError(TEXT("pcg_apply_delta: graph is no longer valid"));
		return false;
	}
	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		FString Ref;
		TSharedPtr<FJsonObject> Obj;
		if (Entries[i].IsValid() && Entries[i]->Type == EJson::String)
		{
			Ref = Entries[i]->AsString();
		}
		else if (PCGDelta_TryGetObject(Entries[i], Obj))
		{
			Obj->TryGetStringField(TEXT("id"), Ref);
			if (Ref.IsEmpty()) { Obj->TryGetStringField(TEXT("name"), Ref); }
		}
		if (Ref.IsEmpty())
		{
			AddError(FString::Printf(TEXT("pcg_apply_delta: remove_nodes[%d] requires 'id' or 'name'"), i));
			return false;
		}
		FString ResolveError;
		UPCGNode* Node = ResolveNodeRef(Graph, Ref, ResolveError);
		if (!Node) { AddError(ResolveError); return false; }
		if (Node == Graph->GetInputNode() || Node == Graph->GetOutputNode())
		{
			AddError(TEXT("pcg_apply_delta: cannot remove the graph's built-in Input or Output node"));
			return false;
		}
		const FString DisplayName = ClaireonPCGGraphHelpers::GetNodeDisplayName(Node);
		Graph->RemoveNode(Node);
		MarkRemoved();
		RecordAffected(DisplayName);
	}
	return true;
}

bool FClaireonDeltaApplicator_PCGGraph::ApplyPhase3_Create(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_PCG_anon;
	(void)SessionId;
	UPCGGraph* Graph = CachedGraph.Get();
	if (!Graph)
	{
		AddError(TEXT("pcg_apply_delta: graph is no longer valid"));
		return false;
	}
	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!PCGDelta_TryGetObject(Entries[i], Obj))
		{
			AddError(FString::Printf(TEXT("pcg_apply_delta: nodes[%d] must be an object"), i));
			return false;
		}
		FString LocalId, NodeType;
		Obj->TryGetStringField(TEXT("id"), LocalId);
		Obj->TryGetStringField(TEXT("type"), NodeType);
		if (LocalId.IsEmpty() || NodeType.IsEmpty())
		{
			AddError(FString::Printf(TEXT("pcg_apply_delta: nodes[%d] requires 'id' and 'type'"), i));
			return false;
		}
		FString ResolveError;
		UClass* SettingsClass = ClaireonPCGGraphHelpers::ResolveSettingsClass(NodeType, ResolveError);
		if (!SettingsClass)
		{
			AddError(FString::Printf(TEXT("pcg_apply_delta: nodes[%d]: %s"), i, *ResolveError));
			return false;
		}
		UPCGSettings* DefaultSettings = nullptr;
		UPCGNode* NewNode = Graph->AddNodeOfType(TSubclassOf<UPCGSettings>(SettingsClass), DefaultSettings);
		if (!NewNode)
		{
			AddError(FString::Printf(TEXT("pcg_apply_delta: nodes[%d]: failed to add node of type '%s'"), i, *NodeType));
			return false;
		}
		CreatedNodesThisCall.Add(NewNode);

		const int32 NodeIndex = Graph->GetNodes().IndexOfByKey(NewNode);
		const FString IndexStr = FString::FromInt(NodeIndex);
		RegisterIdMapping(LocalId, IndexStr);
		MarkCreated();
		RecordAffected(IndexStr);

		const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
		if (Obj->TryGetObjectField(TEXT("properties"), PropsPtr) && PropsPtr && PropsPtr->IsValid())
		{
			for (const auto& Prop : (*PropsPtr)->Values)
			{
				FString PropValue;
				if (Prop.Value->TryGetString(PropValue))
				{
					FString PropError;
					if (!ClaireonPCGGraphHelpers::SetNodeProperty(NewNode, Prop.Key, PropValue, PropError))
					{
						AddWarning(FString::Printf(TEXT("pcg_apply_delta: nodes[%d].properties.%s: %s"), i, *Prop.Key, *PropError));
					}
				}
			}
		}
	}
	return true;
}

bool FClaireonDeltaApplicator_PCGGraph::ApplyPhase4_Connect(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_PCG_anon;
	(void)SessionId;
	UPCGGraph* Graph = CachedGraph.Get();
	if (!Graph)
	{
		AddError(TEXT("pcg_apply_delta: graph is no longer valid"));
		return false;
	}
	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!PCGDelta_TryGetObject(Entries[i], Obj))
		{
			AddError(FString::Printf(TEXT("pcg_apply_delta: connections[%d] must be an object"), i));
			return false;
		}
		FString SN, SP, TN, TP;
		Obj->TryGetStringField(TEXT("source_node"), SN);
		Obj->TryGetStringField(TEXT("source_pin"), SP);
		Obj->TryGetStringField(TEXT("target_node"), TN);
		Obj->TryGetStringField(TEXT("target_pin"), TP);
		if (SN.IsEmpty() || SP.IsEmpty() || TN.IsEmpty() || TP.IsEmpty())
		{
			AddError(FString::Printf(TEXT("pcg_apply_delta: connections[%d] requires source_node, source_pin, target_node, target_pin"), i));
			return false;
		}
		FString ResolveError;
		UPCGNode* SourceNode = ResolveNodeRef(Graph, SN, ResolveError);
		if (!SourceNode) { AddError(ResolveError); return false; }
		UPCGNode* TargetNode = ResolveNodeRef(Graph, TN, ResolveError);
		if (!TargetNode) { AddError(ResolveError); return false; }

		if (!PCGDelta_HasExactOutputPin(SourceNode, SP))
		{
			AddError(FString::Printf(TEXT("pcg_apply_delta: connections[%d]: source output pin '%s' not found (exact-match) on '%s'"),
				i, *SP, *ClaireonPCGGraphHelpers::GetNodeDisplayName(SourceNode)));
			return false;
		}
		if (!PCGDelta_HasExactInputPin(TargetNode, TP))
		{
			AddError(FString::Printf(TEXT("pcg_apply_delta: connections[%d]: target input pin '%s' not found (exact-match) on '%s'"),
				i, *TP, *ClaireonPCGGraphHelpers::GetNodeDisplayName(TargetNode)));
			return false;
		}
		Graph->AddEdge(SourceNode, FName(*SP), TargetNode, FName(*TP));
		MarkConnection();
	}
	return true;
}

void FClaireonDeltaApplicator_PCGGraph::FinalizeSession(const FString& SessionId)
{
	(void)SessionId;
	UPCGGraph* Graph = CachedGraph.Get();
	if (Graph)
	{
		ClaireonPCGGraphHelpers::NotifyGraphChanged(Graph);
	}
}

void FClaireonDeltaApplicator_PCGGraph::CloseSessionIfOwned(const FString& SessionId)
{
	if (DoesOwnSession() && !SessionId.IsEmpty())
	{
		ClaireonPCGGraphEditToolBase::ToolData.Remove(SessionId);
		FClaireonSessionManager::Get().CloseSession(SessionId);
	}
}

void FClaireonDeltaApplicator_PCGGraph::Phase3CleanupOnFailure(const FString& SessionId)
{
	(void)SessionId;
	UPCGGraph* Graph = CachedGraph.Get();
	if (!Graph) { return; }
	for (const TWeakObjectPtr<UPCGNode>& Weak : CreatedNodesThisCall)
	{
		UPCGNode* Node = Weak.Get();
		if (Node && IsValid(Node) && Graph->GetNodes().Contains(Node))
		{
			Graph->RemoveNode(Node);
		}
	}
	CreatedNodesThisCall.Reset();
}
