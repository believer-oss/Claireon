// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "UObject/UObjectIterator.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "AIGraphNode.h"
#include "EdGraph/EdGraph.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"

// ============================================================================
// Asset Loading
// ============================================================================

UBehaviorTree* ClaireonBehaviorTreeHelpers::LoadBehaviorTreeAsset(const FString& AssetPath, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return nullptr;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	FSoftObjectPath SoftPath(ResolvedPath);
	UObject* LoadedObj = SoftPath.TryLoad();
	if (!LoadedObj)
	{
		OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *ResolvedPath);
		return nullptr;
	}

	UBehaviorTree* BT = Cast<UBehaviorTree>(LoadedObj);
	if (!BT)
	{
		OutError = FString::Printf(TEXT("Asset at %s is not a Behavior Tree (actual type: %s)"), *ResolvedPath, *LoadedObj->GetClass()->GetName());
		return nullptr;
	}

	return BT;
}

UBlackboardData* ClaireonBehaviorTreeHelpers::LoadBlackboardAsset(const FString& AssetPath, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return nullptr;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	FSoftObjectPath SoftPath(ResolvedPath);
	UObject* LoadedObj = SoftPath.TryLoad();
	if (!LoadedObj)
	{
		OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *ResolvedPath);
		return nullptr;
	}

	UBlackboardData* BB = Cast<UBlackboardData>(LoadedObj);
	if (!BB)
	{
		OutError = FString::Printf(TEXT("Asset at %s is not a Blackboard Data (actual type: %s)"), *ResolvedPath, *LoadedObj->GetClass()->GetName());
		return nullptr;
	}

	return BB;
}

// ============================================================================
// Formatting Helpers
// ============================================================================

namespace
{
	FString BehaviorTreeHelpers_GetNodeClassName(const UBTNode* Node)
	{
		if (!Node)
		{
			return TEXT("(null)");
		}
		FString ClassName = Node->GetClass()->GetName();
		// Strip common prefixes for readability
		ClassName.RemoveFromStart(TEXT("BTTask_"));
		ClassName.RemoveFromStart(TEXT("BTDecorator_"));
		ClassName.RemoveFromStart(TEXT("BTService_"));
		ClassName.RemoveFromStart(TEXT("BTComposite_"));
		return ClassName;
	}

	FString BehaviorTreeHelpers_GetNodeDisplayName(const UBTNode* Node)
	{
		if (!Node)
		{
			return TEXT("(null)");
		}
		FString NodeName = Node->GetNodeName();
		if (NodeName.IsEmpty())
		{
			NodeName = Node->GetClass()->GetName();
		}
		return NodeName;
	}

	FString BehaviorTreeHelpers_GetCompositeTypeName(const UBTCompositeNode* Composite)
	{
		if (!Composite)
		{
			return TEXT("Unknown");
		}
		FString ClassName = Composite->GetClass()->GetName();
		if (ClassName.Contains(TEXT("Selector")))
		{
			return TEXT("Selector");
		}
		if (ClassName.Contains(TEXT("Sequence")))
		{
			return TEXT("Sequence");
		}
		if (ClassName.Contains(TEXT("SimpleParallel")))
		{
			return TEXT("SimpleParallel");
		}
		return ClassName;
	}

	void BehaviorTreeHelpers_FormatCompositeNodeRecursive(const UBTCompositeNode* Composite, FString& Output, const FString& Indent, bool bFullDetail)
	{
		if (!Composite)
		{
			return;
		}

		// Services on this composite
		for (const UBTService* Service : Composite->Services)
		{
			if (!Service)
			{
				continue;
			}
			// Interval is a protected UPROPERTY, access via reflection
			float ServiceInterval = 0.0f;
			if (const FFloatProperty* IntervalProp = FindFProperty<FFloatProperty>(Service->GetClass(), TEXT("Interval")))
			{
				ServiceInterval = IntervalProp->GetPropertyValue_InContainer(Service);
			}
			Output += FString::Printf(TEXT("%s[Service] %s (%s) tick=%.2fs\n"),
				*Indent, *BehaviorTreeHelpers_GetNodeDisplayName(Service), *Service->GetClass()->GetName(),
				ServiceInterval);
			if (bFullDetail)
			{
				Output += ClaireonBehaviorTreeHelpers::FormatNodeProperties(Service, Indent + TEXT("  "));
			}
		}

		// Children
		const int32 NumChildren = Composite->GetChildrenNum();
		for (int32 Idx = 0; Idx < NumChildren; ++Idx)
		{
			const FBTCompositeChild& Child = Composite->Children[Idx];
			const bool bIsLast = (Idx == NumChildren - 1);
			const FString Prefix = bIsLast ? TEXT("└─ ") : TEXT("├─ ");
			const FString ChildIndent = Indent + (bIsLast ? TEXT("   ") : TEXT("│  "));

			// Decorators on this child
			for (const UBTDecorator* Decorator : Child.Decorators)
			{
				if (!Decorator)
				{
					continue;
				}
				FString AbortMode;
				switch (Decorator->GetFlowAbortMode())
				{
				case EBTFlowAbortMode::None: AbortMode = TEXT("None"); break;
				case EBTFlowAbortMode::LowerPriority: AbortMode = TEXT("LowerPriority"); break;
				case EBTFlowAbortMode::Self: AbortMode = TEXT("Self"); break;
				case EBTFlowAbortMode::Both: AbortMode = TEXT("Both"); break;
				}
				Output += FString::Printf(TEXT("%s[Decorator] %s (%s) abort=%s\n"),
					*Indent, *BehaviorTreeHelpers_GetNodeDisplayName(Decorator), *Decorator->GetClass()->GetName(), *AbortMode);
				if (bFullDetail)
				{
					Output += ClaireonBehaviorTreeHelpers::FormatNodeProperties(Decorator, Indent + TEXT("  "));
				}
			}

			// The child itself (composite or task)
			if (Child.ChildComposite)
			{
				Output += FString::Printf(TEXT("%s%s[%s] %s\n"),
					*Indent, *Prefix, *BehaviorTreeHelpers_GetCompositeTypeName(Child.ChildComposite), *BehaviorTreeHelpers_GetNodeDisplayName(Child.ChildComposite));
				BehaviorTreeHelpers_FormatCompositeNodeRecursive(Child.ChildComposite, Output, ChildIndent, bFullDetail);
			}
			else if (Child.ChildTask)
			{
				Output += FString::Printf(TEXT("%s%s[Task] %s (%s)\n"),
					*Indent, *Prefix, *BehaviorTreeHelpers_GetNodeDisplayName(Child.ChildTask), *Child.ChildTask->GetClass()->GetName());
				if (bFullDetail)
				{
					Output += ClaireonBehaviorTreeHelpers::FormatNodeProperties(Child.ChildTask, ChildIndent);
				}

				// Services on the task
				for (const UBTService* Service : Child.ChildTask->Services)
				{
					if (!Service)
					{
						continue;
					}
					// Interval is a protected UPROPERTY, access via reflection
					float ServiceInterval = 0.0f;
					if (const FFloatProperty* IntervalProp = FindFProperty<FFloatProperty>(Service->GetClass(), TEXT("Interval")))
					{
						ServiceInterval = IntervalProp->GetPropertyValue_InContainer(Service);
					}
					Output += FString::Printf(TEXT("%s[Service] %s (%s) tick=%.2fs\n"),
						*ChildIndent, *BehaviorTreeHelpers_GetNodeDisplayName(Service), *Service->GetClass()->GetName(),
						ServiceInterval);
					if (bFullDetail)
					{
						Output += ClaireonBehaviorTreeHelpers::FormatNodeProperties(Service, ChildIndent + TEXT("  "));
					}
				}
			}
		}
	}
} // anonymous namespace

FString ClaireonBehaviorTreeHelpers::FormatNodeProperties(const UBTNode* Node, const FString& Indent)
{
	if (!Node)
	{
		return FString();
	}

	FString Output;
	const UClass* NodeClass = Node->GetClass();

	// Use GetStaticDescription which BT nodes implement to describe themselves
	FString StaticDesc = Node->GetStaticDescription();
	if (!StaticDesc.IsEmpty())
	{
		// Split multi-line descriptions and indent each line
		TArray<FString> Lines;
		StaticDesc.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			if (!Line.IsEmpty())
			{
				Output += FString::Printf(TEXT("%s  %s\n"), *Indent, *Line);
			}
		}
	}

	return Output;
}

FString ClaireonBehaviorTreeHelpers::FormatBehaviorTreeStructure(const UBehaviorTree* BehaviorTree, bool bFullDetail)
{
	if (!BehaviorTree)
	{
		return TEXT("(null Behavior Tree)");
	}

	FString Output;
	Output += FString::Printf(TEXT("=== Behavior Tree: %s ===\n"), *BehaviorTree->GetName());
	Output += FString::Printf(TEXT("Path: %s\n"), *BehaviorTree->GetPathName());

	// Blackboard reference
	if (BehaviorTree->BlackboardAsset)
	{
		Output += FString::Printf(TEXT("Blackboard: %s\n"), *BehaviorTree->BlackboardAsset->GetName());
	}
	else
	{
		Output += TEXT("Blackboard: (none)\n");
	}

	Output += TEXT("\n");

	// Root node
	UBTCompositeNode* RootNode = BehaviorTree->RootNode;
	if (!RootNode)
	{
		Output += TEXT("(empty tree - no root node)\n");
		return Output;
	}

	Output += FString::Printf(TEXT("[%s] %s (root)\n"), *BehaviorTreeHelpers_GetCompositeTypeName(RootNode), *BehaviorTreeHelpers_GetNodeDisplayName(RootNode));
	BehaviorTreeHelpers_FormatCompositeNodeRecursive(RootNode, Output, TEXT(""), bFullDetail);

	// Summary: collect all blackboard key references
	Output += TEXT("\n=== Blackboard Key Usage ===\n");

	TSet<FString> ReferencedKeys;
	TFunction<void(const UBTNode*)> CollectKeys = [&](const UBTNode* Node)
	{
		if (!Node)
		{
			return;
		}

		// Walk properties looking for FBlackboardKeySelector
		for (TFieldIterator<FProperty> PropIt(Node->GetClass()); PropIt; ++PropIt)
		{
			FProperty* Prop = *PropIt;
			if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			{
				if (StructProp->Struct->GetName() == TEXT("BlackboardKeySelector"))
				{
					const void* ValuePtr = StructProp->ContainerPtrToValuePtr<void>(Node);
					// Use ExportText to get the key name
					FString ExportedValue;
					StructProp->ExportText_Direct(ExportedValue, ValuePtr, ValuePtr, nullptr, PPF_None);
					if (!ExportedValue.IsEmpty())
					{
						// Parse the SelectedKeyName from the exported text
						FString KeyName;
						if (FParse::Value(*ExportedValue, TEXT("SelectedKeyName="), KeyName))
						{
							KeyName.TrimQuotesInline();
							if (!KeyName.IsEmpty() && KeyName != TEXT("None"))
							{
								ReferencedKeys.Add(KeyName);
							}
						}
					}
				}
			}
		}
	};

	// Walk all nodes to collect keys
	TFunction<void(const UBTCompositeNode*)> WalkComposite = [&](const UBTCompositeNode* Composite)
	{
		if (!Composite)
		{
			return;
		}
		CollectKeys(Composite);
		for (const UBTService* Service : Composite->Services)
		{
			CollectKeys(Service);
		}
		for (int32 Idx = 0; Idx < Composite->GetChildrenNum(); ++Idx)
		{
			const FBTCompositeChild& Child = Composite->Children[Idx];
			for (const UBTDecorator* Decorator : Child.Decorators)
			{
				CollectKeys(Decorator);
			}
			if (Child.ChildComposite)
			{
				WalkComposite(Child.ChildComposite);
			}
			if (Child.ChildTask)
			{
				CollectKeys(Child.ChildTask);
				for (const UBTService* Service : Child.ChildTask->Services)
				{
					CollectKeys(Service);
				}
			}
		}
	};

	WalkComposite(RootNode);

	if (ReferencedKeys.Num() > 0)
	{
		TArray<FString> SortedKeys = ReferencedKeys.Array();
		SortedKeys.Sort();
		for (const FString& Key : SortedKeys)
		{
			Output += FString::Printf(TEXT("  - %s\n"), *Key);
		}
	}
	else
	{
		Output += TEXT("  (no blackboard keys referenced)\n");
	}

	return Output;
}

FString ClaireonBehaviorTreeHelpers::FormatBlackboardData(const UBlackboardData* BlackboardData, bool bFullDetail)
{
	if (!BlackboardData)
	{
		return TEXT("(null Blackboard Data)");
	}

	FString Output;
	Output += FString::Printf(TEXT("=== Blackboard: %s ===\n"), *BlackboardData->GetName());
	Output += FString::Printf(TEXT("Path: %s\n"), *BlackboardData->GetPathName());

	// Parent blackboard
	if (BlackboardData->Parent)
	{
		Output += FString::Printf(TEXT("Parent: %s (%s)\n"), *BlackboardData->Parent->GetName(), *BlackboardData->Parent->GetPathName());
	}
	else
	{
		Output += TEXT("Parent: (none)\n");
	}

	Output += TEXT("\n");

	// Show parent keys first (inherited)
	if (BlackboardData->Parent)
	{
		Output += TEXT("--- Inherited Keys (from parent) ---\n");
		for (const FBlackboardEntry& Entry : BlackboardData->ParentKeys)
		{
			FString TypeName = Entry.KeyType ? Entry.KeyType->GetClass()->GetName() : TEXT("Unknown");
			TypeName.RemoveFromStart(TEXT("BlackboardKeyType_"));
			Output += FString::Printf(TEXT("  %s : %s"), *Entry.EntryName.ToString(), *TypeName);
			if (Entry.bInstanceSynced)
			{
				Output += TEXT(" [synced]");
			}
			Output += TEXT("\n");
		}
		Output += TEXT("\n");
	}

	// Own keys
	Output += TEXT("--- Keys ---\n");
	for (const FBlackboardEntry& Entry : BlackboardData->Keys)
	{
		FString TypeName = Entry.KeyType ? Entry.KeyType->GetClass()->GetName() : TEXT("Unknown");
		TypeName.RemoveFromStart(TEXT("BlackboardKeyType_"));
		Output += FString::Printf(TEXT("  %s : %s"), *Entry.EntryName.ToString(), *TypeName);
		if (Entry.bInstanceSynced)
		{
			Output += TEXT(" [synced]");
		}
		Output += TEXT("\n");

		if (bFullDetail && Entry.KeyType)
		{
			// Show key type details via reflection
			FString Desc = Entry.KeyType->DescribeSelf();
			if (!Desc.IsEmpty())
			{
				Output += FString::Printf(TEXT("    arithmetic: %s\n"), *Desc);
			}
		}
	}

	Output += FString::Printf(TEXT("\nTotal keys: %d (own) + %d (inherited) = %d\n"),
		BlackboardData->Keys.Num(),
		BlackboardData->ParentKeys.Num(),
		BlackboardData->Keys.Num() + BlackboardData->ParentKeys.Num());

	return Output;
}

// ============================================================================
// Graph Access Helpers (for edit sessions)
// ============================================================================

UBehaviorTreeGraph* ClaireonBehaviorTreeHelpers::GetBTGraph(UBehaviorTree* BehaviorTree, FString& OutError)
{
	if (!BehaviorTree)
	{
		OutError = TEXT("BehaviorTree is null");
		return nullptr;
	}

	UBehaviorTreeGraph* BTGraph = Cast<UBehaviorTreeGraph>(BehaviorTree->BTGraph);
	if (!BTGraph)
	{
		OutError = TEXT("BehaviorTree has no BTGraph (or cast failed)");
		return nullptr;
	}

	return BTGraph;
}

UBehaviorTreeGraphNode* ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(UBehaviorTreeGraph* Graph, const FGuid& NodeGuid)
{
	if (!Graph)
	{
		return nullptr;
	}

	for (UEdGraphNode* GraphNode : Graph->Nodes)
	{
		UBehaviorTreeGraphNode* BTGraphNode = Cast<UBehaviorTreeGraphNode>(GraphNode);
		if (BTGraphNode && BTGraphNode->NodeGuid == NodeGuid)
		{
			return BTGraphNode;
		}
		// Also check sub-nodes (decorators, services)
		if (BTGraphNode)
		{
			for (UAIGraphNode* SubNode : BTGraphNode->SubNodes)
			{
				UBehaviorTreeGraphNode* SubBTNode = Cast<UBehaviorTreeGraphNode>(SubNode);
				if (SubBTNode && SubBTNode->NodeGuid == NodeGuid)
				{
					return SubBTNode;
				}
			}
		}
	}

	return nullptr;
}

UBehaviorTreeGraphNode_Root* ClaireonBehaviorTreeHelpers::FindRootGraphNode(UBehaviorTreeGraph* Graph)
{
	if (!Graph)
	{
		return nullptr;
	}

	for (UEdGraphNode* GraphNode : Graph->Nodes)
	{
		if (UBehaviorTreeGraphNode_Root* RootNode = Cast<UBehaviorTreeGraphNode_Root>(GraphNode))
		{
			return RootNode;
		}
	}

	return nullptr;
}

UBehaviorTreeGraphNode* ClaireonBehaviorTreeHelpers::CreateGraphNodeForClass(UBehaviorTreeGraph* Graph, UClass* NodeClass, FVector2D Position, FString& OutError)
{
	if (!Graph || !NodeClass)
	{
		OutError = TEXT("Invalid Graph or NodeClass");
		return nullptr;
	}

	// Determine which graph node class to use based on the BT node class
	UClass* GraphNodeClass = nullptr;
	if (NodeClass->IsChildOf(UBTCompositeNode::StaticClass()))
	{
		GraphNodeClass = UBehaviorTreeGraphNode_Composite::StaticClass();
	}
	else if (NodeClass->IsChildOf(UBTTaskNode::StaticClass()))
	{
		GraphNodeClass = UBehaviorTreeGraphNode_Task::StaticClass();
	}
	else if (NodeClass->IsChildOf(UBTDecorator::StaticClass()))
	{
		GraphNodeClass = UBehaviorTreeGraphNode_Decorator::StaticClass();
	}
	else if (NodeClass->IsChildOf(UBTService::StaticClass()))
	{
		GraphNodeClass = UBehaviorTreeGraphNode_Service::StaticClass();
	}
	else
	{
		OutError = FString::Printf(TEXT("Class %s is not a recognized BT node type"), *NodeClass->GetName());
		return nullptr;
	}

	// Create the graph node
	FGraphNodeCreator<UBehaviorTreeGraphNode> NodeCreator(*Graph);
	UBehaviorTreeGraphNode* NewGraphNode = NodeCreator.CreateNode(false, GraphNodeClass);
	if (!NewGraphNode)
	{
		OutError = TEXT("Failed to create graph node");
		return nullptr;
	}

	NewGraphNode->NodePosX = static_cast<int32>(Position.X);
	NewGraphNode->NodePosY = static_cast<int32>(Position.Y);

	// Create the runtime BT node instance
	UBTNode* NewNodeInstance = NewObject<UBTNode>(NewGraphNode, NodeClass);
	NewGraphNode->ClassData = FGraphNodeClassData(NodeClass, FString());
	NewGraphNode->NodeInstance = NewNodeInstance;

	NodeCreator.Finalize();

	return NewGraphNode;
}

bool ClaireonBehaviorTreeHelpers::ConnectNodes(UBehaviorTreeGraphNode* Parent, UBehaviorTreeGraphNode* Child, int32 ChildIndex, FString& OutError)
{
	if (!Parent || !Child)
	{
		OutError = TEXT("Invalid parent or child node");
		return false;
	}

	// Find the output pin on the parent
	UEdGraphPin* ParentOutputPin = nullptr;
	for (UEdGraphPin* Pin : Parent->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output)
		{
			ParentOutputPin = Pin;
			break;
		}
	}

	if (!ParentOutputPin)
	{
		OutError = TEXT("Parent node has no output pin");
		return false;
	}

	// Find the input pin on the child
	UEdGraphPin* ChildInputPin = nullptr;
	for (UEdGraphPin* Pin : Child->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input)
		{
			ChildInputPin = Pin;
			break;
		}
	}

	if (!ChildInputPin)
	{
		OutError = TEXT("Child node has no input pin");
		return false;
	}

	// Make the connection
	ParentOutputPin->MakeLinkTo(ChildInputPin);

	return true;
}

bool ClaireonBehaviorTreeHelpers::DisconnectNode(UBehaviorTreeGraphNode* Node, FString& OutError)
{
	if (!Node)
	{
		OutError = TEXT("Node is null");
		return false;
	}

	// Find the input pin and break all its links
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Input)
		{
			Pin->BreakAllPinLinks();
			return true;
		}
	}

	OutError = TEXT("Node has no input pin to disconnect");
	return false;
}

bool ClaireonBehaviorTreeHelpers::SetBTNodeProperty(UBTNode* Node, const FString& PropertyName, const FString& PropertyValue, FString& OutError)
{
	if (!Node)
	{
		OutError = TEXT("Node is null");
		return false;
	}

	FProperty* Property = FindFProperty<FProperty>(Node->GetClass(), *PropertyName);
	if (!Property)
	{
		OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *Node->GetClass()->GetName());
		return false;
	}

	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Node);
	const TCHAR* Result = Property->ImportText_Direct(*PropertyValue, ValuePtr, Node, PPF_None);
	if (Result)
	{
		return true;
	}

	// Many BT properties are FValueOrBBKey_* structs (e.g. WaitTime, RandomDeviation).
	// A bare scalar like "0.5" can't be imported as a struct -- the engine expects
	// "(DefaultValue=0.5)". Detect that shape and retry transparently so callers
	// don't have to know about the wrapper.
	if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		if (StructProp->Struct && StructProp->Struct->GetName().StartsWith(TEXT("ValueOrBBKey_")))
		{
			const FString WrappedValue = FString::Printf(TEXT("(DefaultValue=%s)"), *PropertyValue);
			const TCHAR* WrappedResult = Property->ImportText_Direct(*WrappedValue, ValuePtr, Node, PPF_None);
			if (WrappedResult)
			{
				return true;
			}
			OutError = FString::Printf(
				TEXT("Failed to set property '%s' to '%s' on %s. Expected struct format like '(DefaultValue=%s)' for %s."),
				*PropertyName, *PropertyValue, *Node->GetClass()->GetName(),
				*PropertyValue, *StructProp->Struct->GetName());
			return false;
		}
	}

	OutError = FString::Printf(TEXT("Failed to set property '%s' to '%s' on %s"), *PropertyName, *PropertyValue, *Node->GetClass()->GetName());
	return false;
}

FString ClaireonBehaviorTreeHelpers::FormatBTGraphStructure(UBehaviorTreeGraph* Graph, bool bFullDetail)
{
	if (!Graph)
	{
		return TEXT("(null graph)");
	}

	FString Output;

	// Find root node
	UBehaviorTreeGraphNode_Root* RootNode = FindRootGraphNode(Graph);
	if (!RootNode)
	{
		return TEXT("(no root node found in graph)");
	}

	// Helper to format a single graph node
	TFunction<void(UBehaviorTreeGraphNode*, const FString&, bool)> FormatGraphNode;
	FormatGraphNode = [&](UBehaviorTreeGraphNode* GraphNode, const FString& Indent, bool bIsLast)
	{
		if (!GraphNode)
		{
			return;
		}

		FString Prefix = bIsLast ? TEXT("└─ ") : TEXT("├─ ");
		FString ChildIndent = Indent + (bIsLast ? TEXT("   ") : TEXT("│  "));
		FString GuidStr = GraphNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);

		// Determine node type label
		FString TypeLabel;
		FString NodeName;
		if (UBTNode* NodeInstance = Cast<UBTNode>(GraphNode->NodeInstance))
		{
			FString ClassName = NodeInstance->GetClass()->GetName();
			if (NodeInstance->IsA(UBTCompositeNode::StaticClass()))
			{
				if (ClassName.Contains(TEXT("Selector")))
					TypeLabel = TEXT("Selector");
				else if (ClassName.Contains(TEXT("Sequence")))
					TypeLabel = TEXT("Sequence");
				else if (ClassName.Contains(TEXT("SimpleParallel")))
					TypeLabel = TEXT("SimpleParallel");
				else
					TypeLabel = ClassName;
			}
			else if (NodeInstance->IsA(UBTTaskNode::StaticClass()))
			{
				TypeLabel = TEXT("Task");
			}
			else
			{
				TypeLabel = ClassName;
			}
			NodeName = NodeInstance->GetNodeName();
			if (NodeName.IsEmpty())
			{
				NodeName = ClassName;
			}
		}
		else
		{
			TypeLabel = TEXT("Root");
			NodeName = TEXT("Root");
		}

		Output += FString::Printf(TEXT("%s%s[%s] %s {%s}\n"), *Indent, *Prefix, *TypeLabel, *NodeName, *GuidStr);

		// Show sub-nodes (decorators, services) with their GUIDs
		for (UAIGraphNode* SubNode : GraphNode->SubNodes)
		{
			UBehaviorTreeGraphNode* SubBTNode = Cast<UBehaviorTreeGraphNode>(SubNode);
			if (!SubBTNode)
			{
				continue;
			}

			FString SubGuidStr = SubBTNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
			UBTNode* SubNodeInstance = Cast<UBTNode>(SubBTNode->NodeInstance);
			if (SubNodeInstance)
			{
				FString SubClassName = SubNodeInstance->GetClass()->GetName();
				FString SubTypeLabel;
				if (SubNodeInstance->IsA(UBTDecorator::StaticClass()))
				{
					SubTypeLabel = TEXT("Decorator");
				}
				else if (SubNodeInstance->IsA(UBTService::StaticClass()))
				{
					SubTypeLabel = TEXT("Service");
				}
				else
				{
					SubTypeLabel = TEXT("SubNode");
				}

				FString SubNodeName = SubNodeInstance->GetNodeName();
				if (SubNodeName.IsEmpty())
				{
					SubNodeName = SubClassName;
				}

				Output += FString::Printf(TEXT("%s  [%s] %s (%s) {%s}\n"),
					*ChildIndent, *SubTypeLabel, *SubNodeName, *SubClassName, *SubGuidStr);

				if (bFullDetail)
				{
					Output += FormatNodeProperties(SubNodeInstance, ChildIndent + TEXT("    "));
				}
			}
		}

		if (bFullDetail)
		{
			UBTNode* NodeInst = Cast<UBTNode>(GraphNode->NodeInstance);
			if (NodeInst)
			{
				Output += FormatNodeProperties(NodeInst, ChildIndent + TEXT("  "));
			}
		}

		// Find children connected via output pin
		TArray<UBehaviorTreeGraphNode*> Children;
		for (UEdGraphPin* Pin : GraphNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output)
			{
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (LinkedPin)
					{
						UBehaviorTreeGraphNode* ChildNode = Cast<UBehaviorTreeGraphNode>(LinkedPin->GetOwningNode());
						if (ChildNode)
						{
							Children.Add(ChildNode);
						}
					}
				}
			}
		}

		for (int32 i = 0; i < Children.Num(); ++i)
		{
			FormatGraphNode(Children[i], ChildIndent, i == Children.Num() - 1);
		}
	};

	Output += TEXT("[Root] {") + RootNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower) + TEXT("}\n");

	// Get children of root
	TArray<UBehaviorTreeGraphNode*> RootChildren;
	for (UEdGraphPin* Pin : RootNode->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output)
		{
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (LinkedPin)
				{
					UBehaviorTreeGraphNode* ChildNode = Cast<UBehaviorTreeGraphNode>(LinkedPin->GetOwningNode());
					if (ChildNode)
					{
						RootChildren.Add(ChildNode);
					}
				}
			}
		}
	}

	for (int32 i = 0; i < RootChildren.Num(); ++i)
	{
		FormatGraphNode(RootChildren[i], TEXT(""), i == RootChildren.Num() - 1);
	}

	return Output;
}

UEnvQuery* ClaireonBehaviorTreeHelpers::LoadEQSAsset(const FString& AssetPath, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return nullptr;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	FSoftObjectPath SoftPath(ResolvedPath);
	UObject* LoadedObj = SoftPath.TryLoad();
	if (!LoadedObj)
	{
		OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *ResolvedPath);
		return nullptr;
	}

	UEnvQuery* Query = Cast<UEnvQuery>(LoadedObj);
	if (!Query)
	{
		OutError = FString::Printf(TEXT("Asset at %s is not an EQS Query (actual type: %s)"), *ResolvedPath, *LoadedObj->GetClass()->GetName());
		return nullptr;
	}

	return Query;
}

FString ClaireonBehaviorTreeHelpers::FormatEQSStructure(const UEnvQuery* Query, bool bFullDetail)
{
	if (!Query)
	{
		return TEXT("(null EQS query)");
	}

	FString Output;
	Output += FString::Printf(TEXT("=== EQS Query: %s ===\n"), *Query->GetName());
	Output += FString::Printf(TEXT("Path: %s\n"), *Query->GetPathName());
	Output += FString::Printf(TEXT("Options: %d\n\n"), Query->GetOptions().Num());

	for (int32 OptIdx = 0; OptIdx < Query->GetOptions().Num(); ++OptIdx)
	{
		UEnvQueryOption* Option = Query->GetOptions()[OptIdx];
		if (!Option)
		{
			continue;
		}

		Output += FString::Printf(TEXT("--- Option %d ---\n"), OptIdx);

		UEnvQueryGenerator* Generator = Option->Generator;
		if (Generator)
		{
			Output += FString::Printf(TEXT("  [Generator] %s\n"), *Generator->GetClass()->GetName());
			if (bFullDetail)
			{
				// Show generator properties via reflection
				for (TFieldIterator<FProperty> PropIt(Generator->GetClass(), EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
				{
					FProperty* Prop = *PropIt;
					if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
					{
						continue;
					}
					const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Generator);
					FString ValueStr;
					Prop->ExportText_Direct(ValueStr, ValuePtr, ValuePtr, nullptr, PPF_None);
					if (!ValueStr.IsEmpty() && ValueStr != TEXT("None") && ValueStr != TEXT("0") && ValueStr != TEXT("()"))
					{
						if (ValueStr.Len() > 120)
						{
							ValueStr = ValueStr.Left(117) + TEXT("...");
						}
						Output += FString::Printf(TEXT("    %s = %s\n"), *Prop->GetName(), *ValueStr);
					}
				}
			}
		}

		for (int32 TestIdx = 0; TestIdx < Option->Tests.Num(); ++TestIdx)
		{
			UEnvQueryTest* Test = Option->Tests[TestIdx];
			if (!Test)
			{
				continue;
			}

			Output += FString::Printf(TEXT("  [Test %d] %s\n"), TestIdx, *Test->GetClass()->GetName());

			FString Purpose;
			switch (Test->TestPurpose)
			{
			case EEnvTestPurpose::Filter: Purpose = TEXT("Filter"); break;
			case EEnvTestPurpose::Score: Purpose = TEXT("Score"); break;
			case EEnvTestPurpose::FilterAndScore: Purpose = TEXT("FilterAndScore"); break;
			default: Purpose = TEXT("Unknown"); break;
			}
			Output += FString::Printf(TEXT("    Purpose: %s\n"), *Purpose);

			if (bFullDetail)
			{
				for (TFieldIterator<FProperty> PropIt(Test->GetClass(), EFieldIteratorFlags::ExcludeSuper); PropIt; ++PropIt)
				{
					FProperty* Prop = *PropIt;
					if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
					{
						continue;
					}
					const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Test);
					FString ValueStr;
					Prop->ExportText_Direct(ValueStr, ValuePtr, ValuePtr, nullptr, PPF_None);
					if (!ValueStr.IsEmpty() && ValueStr != TEXT("None") && ValueStr != TEXT("0") && ValueStr != TEXT("()"))
					{
						if (ValueStr.Len() > 120)
						{
							ValueStr = ValueStr.Left(117) + TEXT("...");
						}
						Output += FString::Printf(TEXT("    %s = %s\n"), *Prop->GetName(), *ValueStr);
					}
				}
			}
		}

		Output += TEXT("\n");
	}

	return Output;
}
