// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/FClaireonDeltaApplicator_BehaviorTree.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "Tools/ClaireonBehaviorTreeEditToolBase.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "EdGraph/EdGraphPin.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonDeltaApplicator_BehaviorTree_anon
{
	// File-local discriminator prefix to avoid anon-namespace collisions under unity batching.
	using FAnyEntry = TSharedPtr<FJsonValue>;
	using FAnyObj = TSharedPtr<FJsonObject>;

	static bool BTDelta_TryGetObject(const FAnyEntry& Entry, FAnyObj& OutObj)
	{
		if (!Entry.IsValid() || Entry->Type != EJson::Object) { return false; }
		OutObj = Entry->AsObject();
		return OutObj.IsValid();
	}

	static UBehaviorTreeGraphNode* BTDelta_FindNodeByName(UBehaviorTreeGraph* Graph, const FString& Name, int32& OutMatchCount)
	{
		OutMatchCount = 0;
		UBehaviorTreeGraphNode* Found = nullptr;
		if (!Graph) { return nullptr; }
		for (UEdGraphNode* EdNode : Graph->Nodes)
		{
			UBehaviorTreeGraphNode* BTNode = Cast<UBehaviorTreeGraphNode>(EdNode);
			if (!BTNode) { continue; }
			const FString Title = BTNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
			if (Title.Equals(Name, ESearchCase::IgnoreCase))
			{
				++OutMatchCount;
				if (!Found) { Found = BTNode; }
			}
		}
		return Found;
	}
}

UBehaviorTreeGraphNode* FClaireonDeltaApplicator_BehaviorTree::ResolveNodeRef(
	UBehaviorTreeGraph* Graph,
	const FString& Ref,
	FString& OutError) const
{
	using namespace ClaireonDeltaApplicator_BehaviorTree_anon;

	if (Ref.IsEmpty())
	{
		OutError = TEXT("empty node reference");
		return nullptr;
	}

	// 1. id_map (local id minted earlier in this call) -> resolved id
	const FString Resolved = const_cast<FClaireonDeltaApplicator_BehaviorTree*>(this)->ResolveLocalId(Ref);

	// 2. parse as GUID
	FGuid Guid;
	if (FGuid::Parse(Resolved, Guid))
	{
		if (UBehaviorTreeGraphNode* Node = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, Guid))
		{
			return Node;
		}
	}

	// 3. fall back to node-name title match (case-insensitive)
	int32 MatchCount = 0;
	UBehaviorTreeGraphNode* ByName = BTDelta_FindNodeByName(Graph, Resolved, MatchCount);
	if (MatchCount > 1)
	{
		OutError = FString::Printf(TEXT("behaviortree_apply_delta: ambiguous node reference '%s' resolves to %d nodes; use 'id' instead"),
			*Ref, MatchCount);
		return nullptr;
	}
	if (ByName) { return ByName; }

	OutError = FString::Printf(TEXT("behaviortree_apply_delta: node reference '%s' not found"), *Ref);
	return nullptr;
}

bool FClaireonDeltaApplicator_BehaviorTree::ValidateArgs(const TSharedPtr<FJsonObject>& Args, TArray<FString>& OutErrors)
{
	(void)Args; (void)OutErrors;
	// No additional family-specific validation beyond what the base driver performs.
	return true;
}

bool FClaireonDeltaApplicator_BehaviorTree::OpenOrReuseSession(const TSharedPtr<FJsonObject>& Args, FString& OutSessionId, FString& OutError)
{
	using namespace ClaireonDeltaApplicator_BehaviorTree_anon;

	CreatedNodesThisCall.Reset();
	CachedBT.Reset();
	CachedGraph.Reset();

	FString SessionIdArg;
	const bool bHasSessionId = Args->TryGetStringField(TEXT("session_id"), SessionIdArg) && !SessionIdArg.IsEmpty();
	if (bHasSessionId)
	{
		FBehaviorTreeEditToolData* Data = ClaireonBehaviorTreeEditToolBase::ToolData.Find(SessionIdArg);
		if (!Data || !Data->IsValid())
		{
			OutError = FString::Printf(TEXT("behaviortree_apply_delta: session_id '%s' not found"), *SessionIdArg);
			return false;
		}
		CachedBT = Data->BehaviorTree;
		CachedGraph = Data->BTGraph;
		OutSessionId = SessionIdArg;
		return true;
	}

	// asset_path mode -- M5 fail-on-missing semantics.
	FString AssetPathArg;
	if (!Args->TryGetStringField(TEXT("asset_path"), AssetPathArg) || AssetPathArg.IsEmpty())
	{
		OutError = TEXT("behaviortree_apply_delta: missing asset_path");
		return false;
	}

	UBehaviorTree* BT = ClaireonBehaviorTreeHelpers::LoadBehaviorTreeAsset(AssetPathArg, OutError);
	if (!BT)
	{
		return false;
	}
	UBehaviorTreeGraph* Graph = ClaireonBehaviorTreeHelpers::GetBTGraph(BT, OutError);
	if (!Graph)
	{
		return false;
	}

	ClaireonBehaviorTreeEditToolBase::EnsureDelegateRegistered();

	const FString ResolvedAssetPath = BT->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		ResolvedAssetPath, ClaireonBehaviorTreeEditToolBase::BehaviorTreeSessionToolName);
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		OutError = FString::Printf(TEXT("behaviortree_apply_delta: asset is locked by %s session %s"),
			*Blocker.ToolName, *Blocker.SessionId);
		return false;
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		OutError = FString::Printf(TEXT("behaviortree_apply_delta: invalid asset path: %s"), *ResolvedAssetPath);
		return false;
	}

	FBehaviorTreeEditToolData NewData;
	NewData.BehaviorTree = BT;
	NewData.BTGraph = Graph;
	NewData.LastOperationStatus = TEXT("apply_delta opened");
	ClaireonBehaviorTreeEditToolBase::ToolData.Add(OpenResult.SessionId, MoveTemp(NewData));

	CachedBT = BT;
	CachedGraph = Graph;
	OutSessionId = OpenResult.SessionId;
	return true;
}

bool FClaireonDeltaApplicator_BehaviorTree::ApplyPhase1_Disconnect(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_BehaviorTree_anon;
	(void)SessionId;

	UBehaviorTreeGraph* Graph = CachedGraph.Get();
	if (!Graph)
	{
		AddError(TEXT("behaviortree_apply_delta: graph is no longer valid"));
		return false;
	}

	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		FAnyObj Obj;
		if (!BTDelta_TryGetObject(Entries[i], Obj))
		{
			AddError(FString::Printf(TEXT("behaviortree_apply_delta: disconnect[%d] must be an object"), i));
			return false;
		}

		FString ParentRef, ChildRef;
		Obj->TryGetStringField(TEXT("parent_id"), ParentRef);
		Obj->TryGetStringField(TEXT("child_id"), ChildRef);
		if (ParentRef.IsEmpty() || ChildRef.IsEmpty())
		{
			AddError(FString::Printf(TEXT("behaviortree_apply_delta: disconnect[%d] requires both 'parent_id' and 'child_id'"), i));
			return false;
		}

		FString ResolveError;
		UBehaviorTreeGraphNode* ChildNode = ResolveNodeRef(Graph, ChildRef, ResolveError);
		if (!ChildNode)
		{
			AddError(ResolveError);
			return false;
		}

		// Break only the link to ParentNode (selective disconnect).
		UBehaviorTreeGraphNode* ParentNode = ResolveNodeRef(Graph, ParentRef, ResolveError);
		if (!ParentNode)
		{
			AddError(ResolveError);
			return false;
		}

		// Break any pin links between Parent.OutputPins and Child.InputPins
		for (UEdGraphPin* ChildPin : ChildNode->Pins)
		{
			if (!ChildPin || ChildPin->Direction != EGPD_Input) { continue; }
			TArray<UEdGraphPin*> ToBreak;
			for (UEdGraphPin* LinkedPin : ChildPin->LinkedTo)
			{
				if (LinkedPin && LinkedPin->GetOwningNode() == ParentNode)
				{
					ToBreak.Add(LinkedPin);
				}
			}
			for (UEdGraphPin* LinkedPin : ToBreak)
			{
				ChildPin->BreakLinkTo(LinkedPin);
			}
		}

		RecordAffected(ChildNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	}
	return true;
}

bool FClaireonDeltaApplicator_BehaviorTree::ApplyPhase2_Remove(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_BehaviorTree_anon;
	(void)SessionId;

	UBehaviorTreeGraph* Graph = CachedGraph.Get();
	if (!Graph)
	{
		AddError(TEXT("behaviortree_apply_delta: graph is no longer valid"));
		return false;
	}

	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		FString Ref;
		FAnyObj Obj;
		if (Entries[i].IsValid() && Entries[i]->Type == EJson::String)
		{
			Ref = Entries[i]->AsString();
		}
		else if (BTDelta_TryGetObject(Entries[i], Obj))
		{
			Obj->TryGetStringField(TEXT("id"), Ref);
			if (Ref.IsEmpty()) { Obj->TryGetStringField(TEXT("name"), Ref); }
		}

		if (Ref.IsEmpty())
		{
			AddError(FString::Printf(TEXT("behaviortree_apply_delta: remove_nodes[%d] requires 'id' or 'name'"), i));
			return false;
		}

		FString ResolveError;
		UBehaviorTreeGraphNode* Node = ResolveNodeRef(Graph, Ref, ResolveError);
		if (!Node)
		{
			AddError(ResolveError);
			return false;
		}
		if (Cast<UBehaviorTreeGraphNode_Root>(Node))
		{
			AddError(TEXT("behaviortree_apply_delta: cannot remove the root node"));
			return false;
		}

		FString DiscError;
		ClaireonBehaviorTreeHelpers::DisconnectNode(Node, DiscError);
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin) { Pin->BreakAllPinLinks(); }
		}

		const FString GuidStr = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
		Graph->RemoveNode(Node);
		MarkRemoved();
		RecordAffected(GuidStr);
	}
	return true;
}

bool FClaireonDeltaApplicator_BehaviorTree::ApplyPhase3_Create(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_BehaviorTree_anon;
	(void)SessionId;

	UBehaviorTreeGraph* Graph = CachedGraph.Get();
	if (!Graph)
	{
		AddError(TEXT("behaviortree_apply_delta: graph is no longer valid"));
		return false;
	}

	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		FAnyObj Obj;
		if (!BTDelta_TryGetObject(Entries[i], Obj))
		{
			AddError(FString::Printf(TEXT("behaviortree_apply_delta: nodes[%d] must be an object"), i));
			return false;
		}

		FString LocalId, ClassName;
		Obj->TryGetStringField(TEXT("id"), LocalId);
		Obj->TryGetStringField(TEXT("class"), ClassName);
		if (LocalId.IsEmpty() || ClassName.IsEmpty())
		{
			AddError(FString::Printf(TEXT("behaviortree_apply_delta: nodes[%d] requires 'id' and 'class'"), i));
			return false;
		}

		ClaireonNameResolver::FNameResolveResult NameResult;
		UClass* NodeClass = ClaireonNameResolver::ResolveClassName(ClassName, UBTNode::StaticClass(), NameResult);
		if (!NodeClass)
		{
			AddError(FString::Printf(TEXT("behaviortree_apply_delta: nodes[%d]: failed to resolve class '%s': %s"),
				i, *ClassName, *NameResult.Error));
			return false;
		}

		FString CreateError;
		UBehaviorTreeGraphNode* NewNode = ClaireonBehaviorTreeHelpers::CreateGraphNodeForClass(
			Graph, NodeClass, FVector2D(i * 200.0, 0.0), CreateError);
		if (!NewNode)
		{
			AddError(FString::Printf(TEXT("behaviortree_apply_delta: nodes[%d]: %s"), i, *CreateError));
			return false;
		}

		CreatedNodesThisCall.Add(NewNode);

		const FString GuidStr = NewNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
		RegisterIdMapping(LocalId, GuidStr);
		MarkCreated();
		RecordAffected(GuidStr);

		// Optional immediate parent attach -- only when parent_id is provided here.
		FString ParentRef;
		bool bHasParent =
			(Obj->TryGetStringField(TEXT("parent_id"), ParentRef) && !ParentRef.IsEmpty()) ||
			(Obj->TryGetStringField(TEXT("parent_local_id"), ParentRef) && !ParentRef.IsEmpty());
		if (bHasParent)
		{
			FString ResolveError;
			UBehaviorTreeGraphNode* ParentNode = ResolveNodeRef(Graph, ParentRef, ResolveError);
			if (ParentNode)
			{
				FString ConnError;
				if (!ClaireonBehaviorTreeHelpers::ConnectNodes(ParentNode, NewNode, -1, ConnError))
				{
					AddWarning(FString::Printf(TEXT("behaviortree_apply_delta: nodes[%d]: parent attach failed: %s"), i, *ConnError));
				}
			}
			else
			{
				AddWarning(FString::Printf(TEXT("behaviortree_apply_delta: nodes[%d]: parent_id '%s' not found"), i, *ParentRef));
			}
		}

		// Optional properties
		const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
		if (Obj->TryGetObjectField(TEXT("properties"), PropsPtr) && PropsPtr && PropsPtr->IsValid())
		{
			UBTNode* InstNode = Cast<UBTNode>(NewNode->NodeInstance);
			if (InstNode)
			{
				for (const auto& Prop : (*PropsPtr)->Values)
				{
					FString PropValue;
					if (Prop.Value->TryGetString(PropValue))
					{
						FString PropError;
						if (!ClaireonBehaviorTreeHelpers::SetBTNodeProperty(InstNode, Prop.Key, PropValue, PropError))
						{
							AddWarning(FString::Printf(TEXT("behaviortree_apply_delta: nodes[%d].properties.%s: %s"),
								i, *Prop.Key, *PropError));
						}
					}
				}
			}
		}
	}
	return true;
}

bool FClaireonDeltaApplicator_BehaviorTree::ApplyPhase4_Connect(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_BehaviorTree_anon;
	(void)SessionId;

	UBehaviorTreeGraph* Graph = CachedGraph.Get();
	if (!Graph)
	{
		AddError(TEXT("behaviortree_apply_delta: graph is no longer valid"));
		return false;
	}

	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		FAnyObj Obj;
		if (!BTDelta_TryGetObject(Entries[i], Obj))
		{
			AddError(FString::Printf(TEXT("behaviortree_apply_delta: connections[%d] must be an object"), i));
			return false;
		}
		FString ParentRef, ChildRef;
		Obj->TryGetStringField(TEXT("parent_id"), ParentRef);
		Obj->TryGetStringField(TEXT("child_id"), ChildRef);
		if (ParentRef.IsEmpty() || ChildRef.IsEmpty())
		{
			AddError(FString::Printf(TEXT("behaviortree_apply_delta: connections[%d] requires 'parent_id' and 'child_id'"), i));
			return false;
		}

		int32 ChildIndex = -1;
		double ChildIndexD = -1.0;
		if (Obj->TryGetNumberField(TEXT("child_index"), ChildIndexD))
		{
			ChildIndex = static_cast<int32>(ChildIndexD);
		}

		FString ResolveError;
		UBehaviorTreeGraphNode* ParentNode = ResolveNodeRef(Graph, ParentRef, ResolveError);
		if (!ParentNode)
		{
			AddError(ResolveError);
			return false;
		}
		UBehaviorTreeGraphNode* ChildNode = ResolveNodeRef(Graph, ChildRef, ResolveError);
		if (!ChildNode)
		{
			AddError(ResolveError);
			return false;
		}

		FString ConnError;
		if (!ClaireonBehaviorTreeHelpers::ConnectNodes(ParentNode, ChildNode, ChildIndex, ConnError))
		{
			AddError(FString::Printf(TEXT("behaviortree_apply_delta: connections[%d]: %s"), i, *ConnError));
			return false;
		}
		MarkConnection();
	}
	return true;
}

void FClaireonDeltaApplicator_BehaviorTree::FinalizeSession(const FString& SessionId)
{
	(void)SessionId;
	UBehaviorTreeGraph* Graph = CachedGraph.Get();
	if (Graph)
	{
		Graph->UpdateAsset();
	}
}

void FClaireonDeltaApplicator_BehaviorTree::CloseSessionIfOwned(const FString& SessionId)
{
	if (DoesOwnSession() && !SessionId.IsEmpty())
	{
		ClaireonBehaviorTreeEditToolBase::ToolData.Remove(SessionId);
		FClaireonSessionManager::Get().CloseSession(SessionId);
	}
}

void FClaireonDeltaApplicator_BehaviorTree::Phase3CleanupOnFailure(const FString& SessionId)
{
	(void)SessionId;
	UBehaviorTreeGraph* Graph = CachedGraph.Get();
	if (!Graph) { return; }
	for (const TWeakObjectPtr<UBehaviorTreeGraphNode>& Weak : CreatedNodesThisCall)
	{
		UBehaviorTreeGraphNode* Node = Weak.Get();
		if (Node && IsValid(Node) && Graph->Nodes.Contains(Node))
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin) { Pin->BreakAllPinLinks(); }
			}
			Graph->RemoveNode(Node);
		}
	}
	CreatedNodesThisCall.Reset();
}
