// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class UBehaviorTree;
class UBTCompositeNode;
class UBTNode;
class UBlackboardData;
class UBehaviorTreeGraph;
class UBehaviorTreeGraphNode;
class UBehaviorTreeGraphNode_Root;
class UEnvQuery;

/**
 * Shared utility functions for Behavior Tree MCP tools.
 * Provides asset loading, tree traversal, and formatting for BT inspection.
 */
namespace ClaireonBehaviorTreeHelpers
{
	/** Load and validate a Behavior Tree asset from an asset path. */
	UBehaviorTree* LoadBehaviorTreeAsset(const FString& AssetPath, FString& OutError);

	/** Load and validate a Blackboard Data asset from an asset path. */
	UBlackboardData* LoadBlackboardAsset(const FString& AssetPath, FString& OutError);

	/** Format the full Behavior Tree structure as structured text. */
	FString FormatBehaviorTreeStructure(const UBehaviorTree* BehaviorTree, bool bFullDetail);

	/** Format a Blackboard Data asset as structured text. */
	FString FormatBlackboardData(const UBlackboardData* BlackboardData, bool bFullDetail);

	/** Format a single BT node's properties via reflection. */
	FString FormatNodeProperties(const UBTNode* Node, const FString& Indent);

	// === Graph access helpers (for edit sessions) ===

	/** Get the UBehaviorTreeGraph from a Behavior Tree (cast from BT->BTGraph). */
	UBehaviorTreeGraph* GetBTGraph(UBehaviorTree* BehaviorTree, FString& OutError);

	/** Find a graph node by its NodeGuid. */
	UBehaviorTreeGraphNode* FindGraphNodeByGuid(UBehaviorTreeGraph* Graph, const FGuid& NodeGuid);

	/** Find the root graph node. */
	UBehaviorTreeGraphNode_Root* FindRootGraphNode(UBehaviorTreeGraph* Graph);

	/** Create a new graph node for a given BT node class and add it to the graph. */
	UBehaviorTreeGraphNode* CreateGraphNodeForClass(UBehaviorTreeGraph* Graph, UClass* NodeClass, FVector2D Position, FString& OutError);

	/** Connect a child graph node to a parent graph node at the specified child index. */
	bool ConnectNodes(UBehaviorTreeGraphNode* Parent, UBehaviorTreeGraphNode* Child, int32 ChildIndex, FString& OutError);

	/** Disconnect a node from its parent. */
	bool DisconnectNode(UBehaviorTreeGraphNode* Node, FString& OutError);

	/** Set a property on a BT node instance using reflection (ImportText). */
	bool SetBTNodeProperty(UBTNode* Node, const FString& PropertyName, const FString& PropertyValue, FString& OutError);

	/** Format the BT graph structure with GUIDs for node addressing. */
	FString FormatBTGraphStructure(UBehaviorTreeGraph* Graph, bool bFullDetail);

	/** Load an EQS query asset from path. */
	UEnvQuery* LoadEQSAsset(const FString& AssetPath, FString& OutError);

	/** Format an EQS query structure with option/test indices. */
	FString FormatEQSStructure(const UEnvQuery* Query, bool bFullDetail);
} // namespace ClaireonBehaviorTreeHelpers
