// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSpecApplicator_BehaviorTree.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonSafeExec.h"
#include "ClaireonLog.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"
#include "FileHelpers.h"

bool FClaireonSpecApplicator_BehaviorTree::ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors)
{
	// Must have "nodes" array
	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (!Spec->TryGetArrayField(TEXT("nodes"), NodesArray) || !NodesArray)
	{
		OutErrors.Add(TEXT("BehaviorTree spec must contain a 'nodes' array"));
		return false;
	}

	if (NodesArray->Num() == 0)
	{
		OutErrors.Add(TEXT("BehaviorTree spec 'nodes' array is empty"));
		return false;
	}

	int32 RootCount = 0;

	for (int32 i = 0; i < NodesArray->Num(); ++i)
	{
		const TSharedPtr<FJsonValue>& NodeVal = (*NodesArray)[i];
		if (!NodeVal.IsValid() || NodeVal->Type != EJson::Object)
		{
			OutErrors.Add(FString::Printf(TEXT("nodes[%d]: not a JSON object"), i));
			continue;
		}

		const TSharedPtr<FJsonObject>& NodeObj = NodeVal->AsObject();

		// Required: id
		FString NodeId;
		if (!NodeObj->TryGetStringField(TEXT("id"), NodeId) || NodeId.IsEmpty())
		{
			OutErrors.Add(FString::Printf(TEXT("nodes[%d]: missing or empty 'id'"), i));
		}

		// Required: type
		FString NodeType;
		if (!NodeObj->TryGetStringField(TEXT("type"), NodeType) || NodeType.IsEmpty())
		{
			OutErrors.Add(FString::Printf(TEXT("nodes[%d]: missing or empty 'type'"), i));
		}

		// Check parent field -- null means root child
		if (!NodeObj->HasField(TEXT("parent")) || NodeObj->GetField<EJson::None>(TEXT("parent"))->IsNull())
		{
			RootCount++;
		}

		// Validate decorators if present
		const TArray<TSharedPtr<FJsonValue>>* DecoratorsArray = nullptr;
		if (NodeObj->TryGetArrayField(TEXT("decorators"), DecoratorsArray) && DecoratorsArray)
		{
			for (int32 d = 0; d < DecoratorsArray->Num(); ++d)
			{
				const TSharedPtr<FJsonValue>& DecVal = (*DecoratorsArray)[d];
				if (!DecVal.IsValid() || DecVal->Type != EJson::Object) continue;
				const TSharedPtr<FJsonObject>& DecObj = DecVal->AsObject();

				FString DecId, DecType;
				if (!DecObj->TryGetStringField(TEXT("id"), DecId) || DecId.IsEmpty())
				{
					OutErrors.Add(FString::Printf(TEXT("nodes[%d].decorators[%d]: missing or empty 'id'"), i, d));
				}
				if (!DecObj->TryGetStringField(TEXT("type"), DecType) || DecType.IsEmpty())
				{
					OutErrors.Add(FString::Printf(TEXT("nodes[%d].decorators[%d]: missing or empty 'type'"), i, d));
				}
			}
		}

		// Validate services if present
		const TArray<TSharedPtr<FJsonValue>>* ServicesArray = nullptr;
		if (NodeObj->TryGetArrayField(TEXT("services"), ServicesArray) && ServicesArray)
		{
			for (int32 s = 0; s < ServicesArray->Num(); ++s)
			{
				const TSharedPtr<FJsonValue>& SvcVal = (*ServicesArray)[s];
				if (!SvcVal.IsValid() || SvcVal->Type != EJson::Object) continue;
				const TSharedPtr<FJsonObject>& SvcObj = SvcVal->AsObject();

				FString SvcId, SvcType;
				if (!SvcObj->TryGetStringField(TEXT("id"), SvcId) || SvcId.IsEmpty())
				{
					OutErrors.Add(FString::Printf(TEXT("nodes[%d].services[%d]: missing or empty 'id'"), i, s));
				}
				if (!SvcObj->TryGetStringField(TEXT("type"), SvcType) || SvcType.IsEmpty())
				{
					OutErrors.Add(FString::Printf(TEXT("nodes[%d].services[%d]: missing or empty 'type'"), i, s));
				}
			}
		}
	}

	if (RootCount == 0)
	{
		OutErrors.Add(TEXT("BehaviorTree spec must have exactly one node with 'parent': null (root child)"));
	}
	else if (RootCount > 1)
	{
		OutErrors.Add(FString::Printf(TEXT("BehaviorTree spec has %d nodes with 'parent': null (expected exactly 1)"), RootCount));
	}

	return OutErrors.Num() == 0;
}

bool FClaireonSpecApplicator_BehaviorTree::OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError)
{
	// Resolve path
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return false;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	// Load BT
	UBehaviorTree* BT = ClaireonBehaviorTreeHelpers::LoadBehaviorTreeAsset(ResolvedPath, OutError);
	if (!BT)
	{
		return false;
	}

	// Get graph
	UBehaviorTreeGraph* Graph = ClaireonBehaviorTreeHelpers::GetBTGraph(BT, OutError);
	if (!Graph)
	{
		return false;
	}

	// Open session
	const FString ResolvedAssetPath = BT->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		ResolvedAssetPath, TEXT("behaviortree_edit"));

	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		OutError = FString::Printf(TEXT("Asset is locked by %s session %s"),
			*Blocker.ToolName, *Blocker.SessionId);
		return false;
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		OutError = FString::Printf(TEXT("Invalid asset path: %s"), *ResolvedAssetPath);
		return false;
	}

	// Store references locally
	BehaviorTree = BT;
	BTGraph = Graph;
	OutSessionId = OpenResult.SessionId;
	return true;
}

bool FClaireonSpecApplicator_BehaviorTree::ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec)
{
	UBehaviorTreeGraph* Graph = BTGraph.Get();
	if (!Graph)
	{
		AddError(TEXT("BT graph is no longer valid"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (!Spec->TryGetArrayField(TEXT("nodes"), NodesArray) || !NodesArray)
	{
		AddError(TEXT("No 'nodes' array in spec"));
		return false;
	}

	int32 SuccessCount = 0;

	for (int32 i = 0; i < NodesArray->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>& NodeObj = (*NodesArray)[i]->AsObject();
		if (!NodeObj.IsValid()) continue;

		FString SpecId;
		NodeObj->TryGetStringField(TEXT("id"), SpecId);

		FString NodeType;
		NodeObj->TryGetStringField(TEXT("type"), NodeType);

		// Resolve class
		FString Error;
		ClaireonNameResolver::FNameResolveResult NameResult;
		UClass* NodeClass = ClaireonNameResolver::ResolveClassName(NodeType, UBTNode::StaticClass(), NameResult);
		if (!NodeClass)
		{
			RecordEntryFailure(SpecId, FString::Printf(TEXT("Failed to resolve class '%s': %s"), *NodeType, *NameResult.Error));
			// Check if other nodes depend on this one
			// For now, continue -- Pass 2 will handle missing dependencies
			continue;
		}

		// Create graph node
		FVector2D Position(i * 200.0, 0.0);
		UBehaviorTreeGraphNode* GraphNode = ClaireonBehaviorTreeHelpers::CreateGraphNodeForClass(
			Graph, NodeClass, Position, Error);
		if (!GraphNode)
		{
			RecordEntryFailure(SpecId, FString::Printf(TEXT("Failed to create node: %s"), *Error));
			continue;
		}

		FString GuidStr = GraphNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
		RegisterIdMapping(SpecId, GuidStr);
		RecordEntrySuccess(SpecId, GuidStr);
		SuccessCount++;
	}

	UE_LOG(LogClaireon, Log, TEXT("[apply_spec:BehaviorTree] Pass 1: Created %d/%d nodes"),
		SuccessCount, NodesArray->Num());

	return true;
}

bool FClaireonSpecApplicator_BehaviorTree::ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec)
{
	UBehaviorTreeGraph* Graph = BTGraph.Get();
	if (!Graph)
	{
		AddError(TEXT("BT graph is no longer valid"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (!Spec->TryGetArrayField(TEXT("nodes"), NodesArray) || !NodesArray)
	{
		return true;
	}

	// Build a map from spec ID to children array for determining child index
	TMap<FString, TArray<FString>> ParentChildrenMap;
	for (const TSharedPtr<FJsonValue>& NodeVal : *NodesArray)
	{
		const TSharedPtr<FJsonObject>& NodeObj = NodeVal->AsObject();
		if (!NodeObj.IsValid()) continue;

		FString NodeId;
		NodeObj->TryGetStringField(TEXT("id"), NodeId);

		const TArray<TSharedPtr<FJsonValue>>* ChildrenArray = nullptr;
		if (NodeObj->TryGetArrayField(TEXT("children"), ChildrenArray) && ChildrenArray)
		{
			TArray<FString> Children;
			for (const TSharedPtr<FJsonValue>& ChildVal : *ChildrenArray)
			{
				if (ChildVal.IsValid() && ChildVal->Type == EJson::String)
				{
					Children.Add(ChildVal->AsString());
				}
			}
			ParentChildrenMap.Add(NodeId, MoveTemp(Children));
		}
	}

	for (const TSharedPtr<FJsonValue>& NodeVal : *NodesArray)
	{
		const TSharedPtr<FJsonObject>& NodeObj = NodeVal->AsObject();
		if (!NodeObj.IsValid()) continue;

		FString SpecId;
		NodeObj->TryGetStringField(TEXT("id"), SpecId);

		// Skip nodes that failed in Pass 1
		if (!IsIdCreated(SpecId))
		{
			continue;
		}

		FString ChildGuidStr = ResolveId(SpecId);
		FGuid ChildGuid;
		FGuid::Parse(ChildGuidStr, ChildGuid);
		UBehaviorTreeGraphNode* ChildGraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, ChildGuid);
		if (!ChildGraphNode)
		{
			AddWarning(FString::Printf(TEXT("Could not find created node for '%s'"), *SpecId));
			continue;
		}

		// --- Parent connection ---
		FString ParentSpecId;
		bool bHasParent = NodeObj->TryGetStringField(TEXT("parent"), ParentSpecId) && !ParentSpecId.IsEmpty();

		if (bHasParent)
		{
			if (!IsIdCreated(ParentSpecId))
			{
				AddWarning(FString::Printf(TEXT("Node '%s' parent '%s' was not created, skipping connection"),
					*SpecId, *ParentSpecId));
			}
			else
			{
				FString ParentGuidStr = ResolveId(ParentSpecId);
				FGuid ParentGuid;
				FGuid::Parse(ParentGuidStr, ParentGuid);
				UBehaviorTreeGraphNode* ParentGraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, ParentGuid);

				if (ParentGraphNode)
				{
					// Determine child index from parent's children array
					int32 ChildIndex = INDEX_NONE;
					const TArray<FString>* ParentChildren = ParentChildrenMap.Find(ParentSpecId);
					if (ParentChildren)
					{
						ChildIndex = ParentChildren->IndexOfByKey(SpecId);
					}
					if (ChildIndex == INDEX_NONE) ChildIndex = 0;

					FString ConnectError;
					if (!ClaireonBehaviorTreeHelpers::ConnectNodes(ParentGraphNode, ChildGraphNode, ChildIndex, ConnectError))
					{
						AddWarning(FString::Printf(TEXT("Failed to connect '%s' to parent '%s': %s"),
							*SpecId, *ParentSpecId, *ConnectError));
					}
				}
			}
		}
		else
		{
			// parent is null -- connect to root node
			UBehaviorTreeGraphNode_Root* RootNode = ClaireonBehaviorTreeHelpers::FindRootGraphNode(Graph);
			if (RootNode)
			{
				FString ConnectError;
				if (!ClaireonBehaviorTreeHelpers::ConnectNodes(RootNode, ChildGraphNode, 0, ConnectError))
				{
					AddWarning(FString::Printf(TEXT("Failed to connect '%s' to root: %s"), *SpecId, *ConnectError));
				}
			}
		}

		// --- Decorators ---
		const TArray<TSharedPtr<FJsonValue>>* DecoratorsArray = nullptr;
		if (NodeObj->TryGetArrayField(TEXT("decorators"), DecoratorsArray) && DecoratorsArray)
		{
			for (int32 d = 0; d < DecoratorsArray->Num(); ++d)
			{
				const TSharedPtr<FJsonObject>& DecObj = (*DecoratorsArray)[d]->AsObject();
				if (!DecObj.IsValid()) continue;

				FString DecId, DecType;
				DecObj->TryGetStringField(TEXT("id"), DecId);
				DecObj->TryGetStringField(TEXT("type"), DecType);

				FString Error;
				ClaireonNameResolver::FNameResolveResult DecNameResult;
				UClass* DecClass = ClaireonNameResolver::ResolveClassName(DecType, UBTNode::StaticClass(), DecNameResult);
				if (!DecClass)
				{
					RecordEntryFailure(DecId, FString::Printf(TEXT("Failed to resolve decorator class: %s"), *DecNameResult.Error));
					continue;
				}

				if (!DecClass->IsChildOf(UBTDecorator::StaticClass()))
				{
					RecordEntryFailure(DecId, FString::Printf(TEXT("Class '%s' is not a BTDecorator subclass"), *DecType));
					continue;
				}

				UBehaviorTreeGraphNode* DecGraphNode = ClaireonBehaviorTreeHelpers::CreateGraphNodeForClass(
					Graph, DecClass,
					FVector2D(ChildGraphNode->NodePosX, ChildGraphNode->NodePosY - 50), Error);
				if (!DecGraphNode)
				{
					RecordEntryFailure(DecId, FString::Printf(TEXT("Failed to create decorator: %s"), *Error));
					continue;
				}

				// AddSubNode manages its own FScopedTransaction
				ChildGraphNode->AddSubNode(DecGraphNode, Graph);

				FString DecGuidStr = DecGraphNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
				RegisterIdMapping(DecId, DecGuidStr);
				RecordEntrySuccess(DecId, DecGuidStr);

				// Set decorator properties
				const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
				if (DecObj->TryGetObjectField(TEXT("properties"), PropsPtr) && PropsPtr && PropsPtr->IsValid())
				{
					UBTNode* BTNode = Cast<UBTNode>(DecGraphNode->NodeInstance);
					if (BTNode)
					{
						for (const auto& Prop : (*PropsPtr)->Values)
						{
							FString PropValue;
							if (Prop.Value->TryGetString(PropValue))
							{
								FString PropError;
								if (!ClaireonBehaviorTreeHelpers::SetBTNodeProperty(BTNode, FString(*Prop.Key), PropValue, PropError))
								{
									AddWarning(FString::Printf(TEXT("Decorator '%s' property '%s': %s"), *DecId, *Prop.Key, *PropError));
								}
							}
						}
					}
				}
			}
		}

		// --- Services ---
		const TArray<TSharedPtr<FJsonValue>>* ServicesArray = nullptr;
		if (NodeObj->TryGetArrayField(TEXT("services"), ServicesArray) && ServicesArray)
		{
			for (int32 s = 0; s < ServicesArray->Num(); ++s)
			{
				const TSharedPtr<FJsonObject>& SvcObj = (*ServicesArray)[s]->AsObject();
				if (!SvcObj.IsValid()) continue;

				FString SvcId, SvcType;
				SvcObj->TryGetStringField(TEXT("id"), SvcId);
				SvcObj->TryGetStringField(TEXT("type"), SvcType);

				FString Error;
				ClaireonNameResolver::FNameResolveResult SvcNameResult;
				UClass* SvcClass = ClaireonNameResolver::ResolveClassName(SvcType, UBTNode::StaticClass(), SvcNameResult);
				if (!SvcClass)
				{
					RecordEntryFailure(SvcId, FString::Printf(TEXT("Failed to resolve service class: %s"), *SvcNameResult.Error));
					continue;
				}

				if (!SvcClass->IsChildOf(UBTService::StaticClass()))
				{
					RecordEntryFailure(SvcId, FString::Printf(TEXT("Class '%s' is not a BTService subclass"), *SvcType));
					continue;
				}

				UBehaviorTreeGraphNode* SvcGraphNode = ClaireonBehaviorTreeHelpers::CreateGraphNodeForClass(
					Graph, SvcClass,
					FVector2D(ChildGraphNode->NodePosX, ChildGraphNode->NodePosY + 50), Error);
				if (!SvcGraphNode)
				{
					RecordEntryFailure(SvcId, FString::Printf(TEXT("Failed to create service: %s"), *Error));
					continue;
				}

				// AddSubNode manages its own FScopedTransaction
				ChildGraphNode->AddSubNode(SvcGraphNode, Graph);

				FString SvcGuidStr = SvcGraphNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
				RegisterIdMapping(SvcId, SvcGuidStr);
				RecordEntrySuccess(SvcId, SvcGuidStr);

				// Set service properties
				const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
				if (SvcObj->TryGetObjectField(TEXT("properties"), PropsPtr) && PropsPtr && PropsPtr->IsValid())
				{
					UBTNode* BTNode = Cast<UBTNode>(SvcGraphNode->NodeInstance);
					if (BTNode)
					{
						for (const auto& Prop : (*PropsPtr)->Values)
						{
							FString PropValue;
							if (Prop.Value->TryGetString(PropValue))
							{
								FString PropError;
								if (!ClaireonBehaviorTreeHelpers::SetBTNodeProperty(BTNode, FString(*Prop.Key), PropValue, PropError))
								{
									AddWarning(FString::Printf(TEXT("Service '%s' property '%s': %s"), *SvcId, *Prop.Key, *PropError));
								}
							}
						}
					}
				}
			}
		}

		// --- Node properties ---
		const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
		if (NodeObj->TryGetObjectField(TEXT("properties"), PropsPtr) && PropsPtr && PropsPtr->IsValid())
		{
			UBTNode* BTNode = Cast<UBTNode>(ChildGraphNode->NodeInstance);
			if (BTNode)
			{
				for (const auto& Prop : (*PropsPtr)->Values)
				{
					FString PropValue;
					if (Prop.Value->TryGetString(PropValue))
					{
						FString PropError;
						if (!ClaireonBehaviorTreeHelpers::SetBTNodeProperty(BTNode, FString(*Prop.Key), PropValue, PropError))
						{
							AddWarning(FString::Printf(TEXT("Node '%s' property '%s': %s"), *SpecId, *Prop.Key, *PropError));
						}
					}
				}
			}
		}
	}

	return true;
}

bool FClaireonSpecApplicator_BehaviorTree::CompileAsset(const FString& SessionId, FString& OutError)
{
	UBehaviorTreeGraph* Graph = BTGraph.Get();
	if (!Graph)
	{
		OutError = TEXT("BT graph is no longer valid");
		return false;
	}

	// UpdateAsset rebuilds the runtime BT from the graph (equivalent of compile for BTs)
	Graph->UpdateAsset();
	return true;
}

bool FClaireonSpecApplicator_BehaviorTree::SaveAsset(const FString& SessionId, FString& OutError)
{
	UBehaviorTree* BT = BehaviorTree.Get();
	if (!BT)
	{
		OutError = TEXT("BehaviorTree is no longer valid");
		return false;
	}

	UPackage* Package = BT->GetPackage();
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
		OutError = TEXT("Failed to save Behavior Tree package");
		return false;
	}

	return true;
}

void FClaireonSpecApplicator_BehaviorTree::CloseSession(const FString& SessionId)
{
	FClaireonSessionManager::Get().CloseSession(SessionId);
}
