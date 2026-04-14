// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_BlueprintTranslateScaffold.h"

#include "ClaireonBPNodeMapper.h"
#include "ClaireonBPTranslateSession.h"
#include "ClaireonLog.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Timeline.h"
#include "K2Node_BaseAsyncTask.h"
#include "Engine/LatentActionManager.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "Misc/SecureHash.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/JsonSerializer.h"

namespace ScaffoldInternal
{
	// Get class prefix (A for actors, U for UObjects, F for structs)
	FString GetClassCppName(const UClass* Class)
	{
		if (!Class)
		{
			return TEXT("UObject");
		}
		return FString::Printf(TEXT("%s%s"), Class->GetPrefixCPP(), *Class->GetName());
	}

	// Get parent header include path heuristic
	FString InferParentIncludePath(const UClass* ParentClass)
	{
		if (!ParentClass)
		{
			return TEXT("UObject/Object.h");
		}

		// Try authoritative metadata first
		FString ModuleRelPath = ParentClass->GetMetaData(TEXT("ModuleRelativePath"));
		if (!ModuleRelPath.IsEmpty())
		{
			return FClaireonBPNodeMapper::StripModuleRelativePrefix(ModuleRelPath); // V4-7
		}

		// Fallback: hardcoded engine class table
		FString ClassName = ParentClass->GetName();
		FString ModuleName = ParentClass->GetOutermost()->GetName();
		ModuleName.RemoveFromStart(TEXT("/Script/"));

		// Common engine parent classes
		if (ClassName == TEXT("Actor"))          return TEXT("GameFramework/Actor.h");
		if (ClassName == TEXT("Pawn"))           return TEXT("GameFramework/Pawn.h");
		if (ClassName == TEXT("Character"))      return TEXT("GameFramework/Character.h");
		if (ClassName == TEXT("PlayerController")) return TEXT("GameFramework/PlayerController.h");
		if (ClassName == TEXT("GameModeBase"))   return TEXT("GameFramework/GameModeBase.h");
		if (ClassName == TEXT("GameStateBase"))  return TEXT("GameFramework/GameStateBase.h");
		if (ClassName == TEXT("PlayerState"))    return TEXT("GameFramework/PlayerState.h");
		if (ClassName == TEXT("ActorComponent")) return TEXT("Components/ActorComponent.h");
		if (ClassName == TEXT("SceneComponent")) return TEXT("Components/SceneComponent.h");
		if (ClassName == TEXT("UserWidget"))     return TEXT("Blueprint/UserWidget.h");

		// Fallback: ModuleName/PrefixClassName.h
		return FString::Printf(TEXT("%s/%s%s.h"), *ModuleName, ParentClass->GetPrefixCPP(), *ClassName);
	}

	// Determine event function name from a K2Node_Event
	FString GetEventFunctionName(const UK2Node_Event* EventNode)
	{
		FString Name = EventNode->EventReference.GetMemberName().ToString();
		if (Name.IsEmpty())
		{
			Name = EventNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
		}
		// Strip "Event " or "Receive" prefix for common events
		Name.RemoveFromStart(TEXT("Event "));
		return Name;
	}

	// Check if a function name is a common virtual override
	bool IsVirtualOverride(const FString& FuncName)
	{
		static const TSet<FString> Overrides = {
			TEXT("BeginPlay"), TEXT("EndPlay"), TEXT("Tick"),
			TEXT("BeginDestroy"), TEXT("PostInitializeComponents"),
			TEXT("OnConstruction"), TEXT("PreInitializeComponents"),
			TEXT("ReceiveBeginPlay"), TEXT("ReceiveEndPlay"), TEXT("ReceiveTick"),
			TEXT("ReceiveAnyDamage"), TEXT("ReceivePointDamage"), TEXT("ReceiveRadialDamage"),
			TEXT("ReceiveHit"), TEXT("ReceiveActorBeginOverlap"), TEXT("ReceiveActorEndOverlap"),
		};
		return Overrides.Contains(FuncName);
	}

	// Normalize a function name (strip "Receive" prefix for implementation)
	FString NormalizeFuncName(const FString& FuncName)
	{
		FString Result = FuncName;
		Result.RemoveFromStart(TEXT("Receive"));
		return Result;
	}

	// Generate a short hash from content for session ID
	FString MakeShortHash(const FString& Content)
	{
		FSHAHash Hash;
		FSHA1 Sha1;
		Sha1.Update(reinterpret_cast<const uint8*>(*Content), Content.Len() * sizeof(TCHAR));
		Sha1.Final();
		Sha1.GetHash(Hash.Hash);
		return Hash.ToString().Left(6).ToLower();
	}

	// Sanitize a name to be a valid C++ identifier (delegates to shared static method)
	FString SanitizeCppIdentifier(const FString& Name)
	{
		return FClaireonBPNodeMapper::SanitizeCppIdentifier(Name);
	}

	// Get variable names from parent Blueprint SCS nodes (for filtering inherited components)
	TSet<FName> GetParentSCSVariableNames(const UBlueprint* BP)
	{
		TSet<FName> ParentVarNames;
		UBlueprint* ParentBP = BP->ParentClass ? Cast<UBlueprint>(BP->ParentClass->ClassGeneratedBy) : nullptr;
		while (ParentBP)
		{
			if (ParentBP->SimpleConstructionScript)
			{
				for (const USCS_Node* Node : ParentBP->SimpleConstructionScript->GetAllNodes())
				{
					if (Node)
					{
						ParentVarNames.Add(Node->GetVariableName());
					}
				}
			}
			ParentBP = ParentBP->ParentClass ? Cast<UBlueprint>(ParentBP->ParentClass->ClassGeneratedBy) : nullptr;
		}
		return ParentVarNames;
	}

	// BFS traversal state for a function graph
	struct FGraphTraversalResult
	{
		// Generated source code for the function body
		FString SourceCode;
		// Member variable declarations needed in header (from macros like DoOnce)
		FString MemberDeclarations;
		// Node GUIDs visited and their status
		TMap<FString, FClaireonBPTranslateNodeStatus> Nodes;
		// Orphan node tags
		FString OrphanCode;
	};

	// Compute the immediate post-dominator of a branch node.
	// For a node with N exec output pins, finds the first node reachable from ALL N paths.
	// Returns nullptr if paths terminate independently (no common join point).
	const UEdGraphNode* ComputePostDominator(const UEdGraphNode* BranchNode, const TArray<FBranchOutput>& Branches)
	{
		if (Branches.Num() < 2)
		{
			return nullptr;
		}

		// Step 1: For each branch output, collect all reachable nodes via exec edges with distances
		TArray<TMap<const UEdGraphNode*, int32>> PerBranchReachable;
		PerBranchReachable.SetNum(Branches.Num());

		for (int32 BranchIdx = 0; BranchIdx < Branches.Num(); ++BranchIdx)
		{
			const UEdGraphPin* ExecPin = Branches[BranchIdx].ExecPin;
			if (!ExecPin) continue;

			// BFS from this pin's connected node
			TArray<TPair<const UEdGraphNode*, int32>> Queue;
			for (UEdGraphPin* LinkedPin : ExecPin->LinkedTo)
			{
				if (LinkedPin && LinkedPin->GetOwningNode())
				{
					Queue.Add(TPair<const UEdGraphNode*, int32>(LinkedPin->GetOwningNode(), 1));
				}
			}

			TSet<const UEdGraphNode*> Seen;
			int32 QueueIdx = 0;
			while (QueueIdx < Queue.Num())
			{
				const UEdGraphNode* Current = Queue[QueueIdx].Key;
				int32 Dist = Queue[QueueIdx].Value;
				++QueueIdx;

				if (!Current || Seen.Contains(Current)) continue;
				Seen.Add(Current);
				PerBranchReachable[BranchIdx].Add(Current, Dist);

				// Follow exec output pins
				for (UEdGraphPin* Pin : Current->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output
						&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
					{
						for (UEdGraphPin* Linked : Pin->LinkedTo)
						{
							if (Linked && Linked->GetOwningNode() && !Seen.Contains(Linked->GetOwningNode()))
							{
								Queue.Add(TPair<const UEdGraphNode*, int32>(Linked->GetOwningNode(), Dist + 1));
							}
						}
					}
				}
			}
		}

		// Step 2: Intersect all reachable sets
		if (PerBranchReachable[0].Num() == 0)
		{
			return nullptr;
		}

		TMap<const UEdGraphNode*, int32> Candidates; // Node -> max distance across all branches
		for (const auto& Pair : PerBranchReachable[0])
		{
			bool bInAll = true;
			int32 MaxDist = Pair.Value;
			for (int32 i = 1; i < PerBranchReachable.Num(); ++i)
			{
				const int32* Dist = PerBranchReachable[i].Find(Pair.Key);
				if (!Dist)
				{
					bInAll = false;
					break;
				}
				MaxDist = FMath::Max(MaxDist, *Dist);
			}
			if (bInAll)
			{
				Candidates.Add(Pair.Key, MaxDist);
			}
		}

		// Step 3: Among candidates, find the one with the smallest max distance
		const UEdGraphNode* BestNode = nullptr;
		int32 BestMaxDist = INT32_MAX;
		for (const auto& Pair : Candidates)
		{
			if (Pair.Value < BestMaxDist)
			{
				BestMaxDist = Pair.Value;
				BestNode = Pair.Key;
			}
		}

		return BestNode;
	}

	// Helper: make indent string
	FString MakeScopeIndent(int32 Level)
	{
		FString Str;
		for (int32 i = 0; i < Level; ++i) Str += TEXT("\t");
		return Str;
	}

	// V3-4: Shared subgraph descriptor
	struct FSharedSubgraph
	{
		TArray<const UEdGraphNode*> Nodes;      // Nodes in the shared subgraph
		TArray<FString> EntryPointNames;         // Entry points that reach this subgraph
		FString HelperFunctionName;              // Generated helper function name
		const UEdGraphNode* EntryNode = nullptr; // First node in the subgraph
	};

	// V3-8: Cross-entry data dependency
	struct FCrossEntryDataDep
	{
		const UEdGraphNode* SourceNode = nullptr;
		const UEdGraphNode* DestNode = nullptr;
		FString SourceEntryPoint;
		FString DestEntryPoint;
		FString PinType;
		FString MemberVarName;
		FString SourcePinName;
		FString DestPinName;
	};

	// Scope-tree traversal context for V3
	struct FEmitScopeContext
	{
		FClaireonBPNodeMapper& NodeMapper;
		TSet<const UEdGraphNode*> Visited;
		TSet<const UEdGraphNode*> OnStack; // V3-cycle detection
		TSet<const UEdGraphNode*> SharedSubgraphEntryNodes; // V3-4: nodes to replace with helper calls
		TMap<const UEdGraphNode*, FString> SharedNodeToHelper; // V3-4: maps entry nodes -> helper func name
		FGraphTraversalResult Result;

		FEmitScopeContext(FClaireonBPNodeMapper& InMapper) : NodeMapper(InMapper) {}

		void EmitScope(const UEdGraphNode* Node, int32 IndentLevel,
			const UEdGraphNode* StopAt = nullptr, const UEdGraphPin* ArrivalPin = nullptr);
	};

	void FEmitScopeContext::EmitScope(const UEdGraphNode* Node, int32 IndentLevel,
		const UEdGraphNode* StopAt, const UEdGraphPin* ArrivalPin)
	{
		if (!Node || Node == StopAt || Visited.Contains(Node))
		{
			return;
		}

		// Cycle detection
		if (OnStack.Contains(Node))
		{
			FString Indent = MakeScopeIndent(IndentLevel);
			Result.SourceCode += FString::Printf(TEXT("%s// [BP:ERROR] Cycle detected at node %s\n"),
				*Indent, *Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces));
			return;
		}

		// V3-4: If this node is a shared subgraph entry, emit a helper call instead
		if (const FString* HelperName = SharedNodeToHelper.Find(Node))
		{
			FString Indent = MakeScopeIndent(IndentLevel);
			Result.SourceCode += FString::Printf(TEXT("%s%s(); // [BP:SHARED_SUBGRAPH]\n"), *Indent, **HelperName);
			Visited.Add(Node);
			return;
		}

		OnStack.Add(Node);
		Visited.Add(Node);

		// Record node status
		FString GuidStr = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces);
		FClaireonBPTranslateNodeStatus NodeStatus;
		NodeStatus.Status = TEXT("pending");
		NodeStatus.Type = Node->GetClass()->GetName();
		NodeStatus.Name = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		Result.Nodes.Add(GuidStr, NodeStatus);

		// Map this node via MapNodeEx (unified dispatch for nodes and macros)
		FMapNodeResult MapResult = NodeMapper.MapNodeEx(Node, IndentLevel, ArrivalPin);
		Result.MemberDeclarations += MapResult.MemberDeclarations;

		if (MapResult.bIsBranchNode && MapResult.Branches.Num() > 1)
		{
			// Branch node: compute post-dominator and emit scoped blocks
			const UEdGraphNode* JoinNode = ComputePostDominator(Node, MapResult.Branches);

			FString Indent = MakeScopeIndent(IndentLevel);

			// Emit the branch header (already in MapResult.Code -- includes opening brace for first branch)
			Result.SourceCode += MapResult.Code;

			// V4-6: Filter out empty branches (no connections or leads directly to join)
			// Keep first branch always (header already emitted), keep Default cases always
			TArray<int32> ActiveBranchIndices;
			ActiveBranchIndices.Add(0); // First branch always active
			for (int32 i = 1; i < MapResult.Branches.Num(); ++i)
			{
				const FBranchOutput& Branch = MapResult.Branches[i];
				bool bIsDefaultCase = (Branch.Label == TEXT("Default"));

				bool bBranchHasContent = false;
				if (Branch.ExecPin && Branch.ExecPin->LinkedTo.Num() > 0)
				{
					for (const UEdGraphPin* LinkedPin : Branch.ExecPin->LinkedTo)
					{
						if (LinkedPin && LinkedPin->GetOwningNode() && LinkedPin->GetOwningNode() != JoinNode)
						{
							bBranchHasContent = true;
							break;
						}
					}
				}

				if (bBranchHasContent || bIsDefaultCase)
				{
					ActiveBranchIndices.Add(i);
				}
			}

			for (int32 ActiveIdx = 0; ActiveIdx < ActiveBranchIndices.Num(); ++ActiveIdx)
			{
				const FBranchOutput& Branch = MapResult.Branches[ActiveBranchIndices[ActiveIdx]];

				if (ActiveIdx > 0)
				{
					// Close previous branch block and open new one
					Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);

					if (Branch.Label == TEXT("False") || Branch.Label == TEXT("else")
						|| Branch.Label == TEXT("Remote") || Branch.Label == TEXT("B")
						|| Branch.Label == TEXT("IsNotValid"))
					{
						Result.SourceCode += FString::Printf(TEXT("%selse\n"), *Indent);
					}
					else if (Branch.Label == TEXT("Default"))
					{
						Result.SourceCode += FString::Printf(TEXT("%selse // Default\n"), *Indent);
					}
					else if (Branch.Label.StartsWith(TEXT("Sequence block")))
					{
						Result.SourceCode += FString::Printf(TEXT("%s// %s\n"), *Indent, *Branch.Label);
					}
					else if (Branch.Label.StartsWith(TEXT("case ")))
					{
						// switch/case: emit case label
						Result.SourceCode += FString::Printf(TEXT("%s%s:\n"), *Indent, *Branch.Label);
					}
					else
					{
						// else-if style (string/name/tag comparisons)
						Result.SourceCode += FString::Printf(TEXT("%s// %s\n"), *Indent, *Branch.Label);
					}
					Result.SourceCode += FString::Printf(TEXT("%s{\n"), *Indent);
				}

				// Emit branch body
				if (Branch.ExecPin)
				{
					const UEdGraphNode* BranchTarget = nullptr;
					const UEdGraphPin* TargetArrivalPin = nullptr;
					if (Branch.ExecPin->LinkedTo.Num() > 0 && Branch.ExecPin->LinkedTo[0])
					{
						BranchTarget = Branch.ExecPin->LinkedTo[0]->GetOwningNode();
						TargetArrivalPin = Branch.ExecPin->LinkedTo[0];
					}

					if (BranchTarget && BranchTarget != JoinNode)
					{
						EmitScope(BranchTarget, IndentLevel + 1, JoinNode, TargetArrivalPin);
					}
				}
			}

			// Close final brace
			Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);

			// Continue from the join node at the original indent level
			if (JoinNode && JoinNode != StopAt)
			{
				EmitScope(JoinNode, IndentLevel, StopAt);
			}
		}
		else if (MapResult.bIsBranchNode && MapResult.Branches.Num() == 1)
		{
			// Single-arm branch (e.g., Gate Enter): emit header + body + close
			FString Indent = MakeScopeIndent(IndentLevel);
			Result.SourceCode += MapResult.Code;

			const FBranchOutput& Branch = MapResult.Branches[0];
			if (Branch.ExecPin)
			{
				const UEdGraphNode* BranchTarget = nullptr;
				const UEdGraphPin* TargetArrivalPin = nullptr;
				if (Branch.ExecPin->LinkedTo.Num() > 0 && Branch.ExecPin->LinkedTo[0])
				{
					BranchTarget = Branch.ExecPin->LinkedTo[0]->GetOwningNode();
					TargetArrivalPin = Branch.ExecPin->LinkedTo[0];
				}
				if (BranchTarget)
				{
					EmitScope(BranchTarget, IndentLevel + 1, StopAt, TargetArrivalPin);
				}
			}

			Result.SourceCode += FString::Printf(TEXT("%s}\n"), *Indent);
		}
		else if (MapResult.bIsLatentSplit)
		{
			// V8: Latent split -- emit timer setup + return. Do NOT follow exec chain.
			// Continuation is emitted in the post-emission callback pass.
			Result.SourceCode += MapResult.Code;
		}
		else
		{
			// Linear node (0 or 1 exec outputs): emit code and follow exec chain
			Result.SourceCode += MapResult.Code;

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output
					&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
					&& Pin->LinkedTo.Num() > 0)
				{
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (LinkedPin && LinkedPin->GetOwningNode())
						{
							const UEdGraphNode* NextNode = LinkedPin->GetOwningNode();
							if (NextNode != StopAt)
							{
								EmitScope(NextNode, IndentLevel, StopAt, LinkedPin);
							}
						}
					}
					break; // Only follow the first exec output for linear nodes
				}
			}
		}

		OnStack.Remove(Node);
	}

	// Find all graphs from a blueprint (uber + function graphs)
	TArray<UEdGraph*> CollectAllGraphs(const UBlueprint* Blueprint)
	{
		TArray<UEdGraph*> AllGraphs;
		AllGraphs.Append(Blueprint->UbergraphPages);
		AllGraphs.Append(Blueprint->FunctionGraphs);
		return AllGraphs;
	}

	// Root node descriptor (events, function entries)
	struct FRootNode
	{
		const UEdGraphNode* Node;
		FString FunctionName;
		FString ReturnType;
		TArray<TPair<FString, FString>> Parameters; // type, name pairs
		bool bIsVirtualOverride;
		bool bIsConstructor;
		uint64 FunctionFlags = 0;
		bool bIsRPC = false; // true if Server/Client/NetMulticast
	};

	// V3-4: Detect shared subgraphs across entry-point trees
	// Requires RootNodes to be populated first, so this is called after FindRootNodes
	TArray<FSharedSubgraph> DetectSharedSubgraphs(
		const TArray<FRootNode>& RootNodes,
		const TArray<UEdGraph*>& Graphs,
		TMap<FString, TSet<const UEdGraphNode*>>& OutPerEntryReachable)
	{
		// Step 1: For each root node, compute the set of all exec-reachable nodes
		for (const auto& Root : RootNodes)
		{
			TSet<const UEdGraphNode*> Reachable;
			TArray<const UEdGraphNode*> Stack;
			Stack.Add(Root.Node);
			while (Stack.Num() > 0)
			{
				const UEdGraphNode* Current = Stack.Pop();
				if (!Current || Reachable.Contains(Current)) continue;
				Reachable.Add(Current);
				for (UEdGraphPin* Pin : Current->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output
						&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
					{
						for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
						{
							if (LinkedPin && LinkedPin->GetOwningNode())
							{
								Stack.Add(LinkedPin->GetOwningNode());
							}
						}
					}
				}
			}
			OutPerEntryReachable.Add(Root.FunctionName, Reachable);
		}

		// Step 2: Find nodes reachable from multiple entry points
		TMap<const UEdGraphNode*, TArray<FString>> NodeToEntryPoints;
		for (const auto& Pair : OutPerEntryReachable)
		{
			for (const UEdGraphNode* Node : Pair.Value)
			{
				NodeToEntryPoints.FindOrAdd(Node).Add(Pair.Key);
			}
		}

		// Step 3: Filter to nodes reachable from 2+ entry points (excluding root/event nodes)
		TSet<const UEdGraphNode*> SharedNodes;
		for (const auto& Pair : NodeToEntryPoints)
		{
			if (Pair.Value.Num() >= 2
				&& !Cast<UK2Node_Event>(Pair.Key)
				&& !Cast<UK2Node_CustomEvent>(Pair.Key)
				&& !Cast<UK2Node_FunctionEntry>(Pair.Key))
			{
				SharedNodes.Add(Pair.Key);
			}
		}

		// Step 4: Cluster shared nodes into connected subgraphs
		TArray<FSharedSubgraph> Subgraphs;
		TSet<const UEdGraphNode*> Assigned;
		for (const UEdGraphNode* StartNode : SharedNodes)
		{
			if (Assigned.Contains(StartNode)) continue;

			FSharedSubgraph Subgraph;
			TArray<const UEdGraphNode*> Queue;
			Queue.Add(StartNode);
			while (Queue.Num() > 0)
			{
				const UEdGraphNode* Current = Queue.Pop();
				if (!Current || Assigned.Contains(Current) || !SharedNodes.Contains(Current)) continue;
				Assigned.Add(Current);
				Subgraph.Nodes.Add(Current);
				for (UEdGraphPin* Pin : Current->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output
						&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
					{
						for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
						{
							if (LinkedPin && LinkedPin->GetOwningNode()
								&& SharedNodes.Contains(LinkedPin->GetOwningNode()))
							{
								Queue.Add(LinkedPin->GetOwningNode());
							}
						}
					}
				}
			}

			// Minimum subgraph size threshold: >= 3 nodes to warrant extraction
			if (Subgraph.Nodes.Num() < 3) continue;

			// V4-2: Find entry node -- node reachable from outside the shared set via exec pins
			// Prefer nodes with exec input from non-shared predecessors
			Subgraph.EntryNode = nullptr;
			TSet<const UEdGraphNode*> SharedNodeSet(Subgraph.Nodes);
			for (const UEdGraphNode* Node : Subgraph.Nodes)
			{
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Input
						&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
					{
						for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
						{
							if (LinkedPin && LinkedPin->GetOwningNode()
								&& !SharedNodeSet.Contains(LinkedPin->GetOwningNode()))
							{
								Subgraph.EntryNode = Node;
								break;
							}
						}
					}
					if (Subgraph.EntryNode) break;
				}
				if (Subgraph.EntryNode) break;
			}

			// Fallback: pick first node with exec output connections within the shared set
			if (!Subgraph.EntryNode)
			{
				for (const UEdGraphNode* Node : Subgraph.Nodes)
				{
					for (const UEdGraphPin* Pin : Node->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Output
							&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
							&& Pin->LinkedTo.Num() > 0)
						{
							Subgraph.EntryNode = Node;
							break;
						}
					}
					if (Subgraph.EntryNode) break;
				}
			}

			// Last resort: first node in the subgraph
			if (!Subgraph.EntryNode)
			{
				Subgraph.EntryNode = Subgraph.Nodes[0];
			}

			// Collect entry point names
			if (const TArray<FString>* EntryPoints = NodeToEntryPoints.Find(Subgraph.EntryNode))
			{
				Subgraph.EntryPointNames = *EntryPoints;
			}

			// Generate helper function name
			FString FirstNodeName = Subgraph.EntryNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
			Subgraph.HelperFunctionName = FString::Printf(TEXT("Helper_%s"),
				*SanitizeCppIdentifier(FirstNodeName));

			Subgraphs.Add(MoveTemp(Subgraph));
		}

		return Subgraphs;
	}

	// V3-8: Detect cross-entry data dependencies
	TArray<FCrossEntryDataDep> DetectCrossEntryDataDeps(
		const TMap<FString, TSet<const UEdGraphNode*>>& PerEntryReachable,
		FClaireonBPNodeMapper& NodeMapper)
	{
		TArray<FCrossEntryDataDep> Deps;

		// Build reverse map: node -> entry point name
		TMap<const UEdGraphNode*, FString> NodeToEntry;
		for (const auto& Pair : PerEntryReachable)
		{
			for (const UEdGraphNode* Node : Pair.Value)
			{
				if (!NodeToEntry.Contains(Node))
				{
					NodeToEntry.Add(Node, Pair.Key);
				}
			}
		}

		// Scan all data edges
		for (const auto& Pair : NodeToEntry)
		{
			const UEdGraphNode* Node = Pair.Key;
			if (!Node) continue;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;

				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;
					const FString* DestEntry = NodeToEntry.Find(LinkedPin->GetOwningNode());
					if (!DestEntry || *DestEntry == Pair.Value) continue;

					// Cross-entry data edge found
					// Skip if source node is a pure getter (no exec pins)
					bool bHasExecPin = false;
					for (UEdGraphPin* SrcPin : Node->Pins)
					{
						if (SrcPin && SrcPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
						{
							bHasExecPin = true;
							break;
						}
					}
					if (!bHasExecPin) continue;

					FCrossEntryDataDep Dep;
					Dep.SourceNode = Node;
					Dep.DestNode = LinkedPin->GetOwningNode();
					Dep.SourceEntryPoint = Pair.Value;
					Dep.DestEntryPoint = *DestEntry;
					Dep.PinType = NodeMapper.PinTypeToCppType(Pin->PinType);
					Dep.SourcePinName = Pin->PinName.ToString();
					Dep.DestPinName = LinkedPin->PinName.ToString();

					// Generate member variable name
					FString NodeName = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
					Dep.MemberVarName = FString::Printf(TEXT("CrossEntry_%s_%s"),
						*SanitizeCppIdentifier(NodeName),
						*SanitizeCppIdentifier(Dep.SourcePinName));

					Deps.Add(MoveTemp(Dep));
				}
			}
		}

		return Deps;
	}

	TArray<FRootNode> FindRootNodes(const TArray<UEdGraph*>& Graphs, FClaireonBPNodeMapper& Mapper)
	{
		TArray<FRootNode> Roots;

		for (UEdGraph* Graph : Graphs)
		{
			if (!Graph)
			{
				continue;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				// Custom events (must be checked BEFORE UK2Node_Event since UK2Node_CustomEvent derives from it)
				if (UK2Node_CustomEvent* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
				{
					FRootNode Root;
					Root.Node = CustomEvent;
					Root.FunctionName = CustomEvent->CustomFunctionName.ToString();
					if (Root.FunctionName.IsEmpty() || Root.FunctionName == TEXT("None"))
					{
						Root.FunctionName = CustomEvent->GetNodeTitle(ENodeTitleType::ListView).ToString();
						Root.FunctionName.RemoveFromStart(TEXT("Custom Event "));
					}
					Root.FunctionName = SanitizeCppIdentifier(Root.FunctionName);
					Root.ReturnType = TEXT("void");
					Root.bIsVirtualOverride = false;
					Root.bIsConstructor = false;
					Root.FunctionFlags = CustomEvent->FunctionFlags;
					Root.bIsRPC = (Root.FunctionFlags & FUNC_Net) != 0
						&& ((Root.FunctionFlags & FUNC_NetServer) != 0
							|| (Root.FunctionFlags & FUNC_NetClient) != 0
							|| (Root.FunctionFlags & FUNC_NetMulticast) != 0);

					for (UEdGraphPin* Pin : CustomEvent->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Output
							&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
							&& Pin->PinName != UK2Node_Event::DelegateOutputName) // V5-7: filter BP-internal delegate pin
						{
							FString ParamType = Mapper.PinTypeToCppType(Pin->PinType);
							FString ParamName = SanitizeCppIdentifier(Pin->PinName.ToString()); // V4-4
							Root.Parameters.Add(TPair<FString, FString>(ParamType, ParamName));
						}
					}

					Roots.Add(MoveTemp(Root));
					continue;
				}

				// Event nodes
				if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
				{
					FRootNode Root;
					Root.Node = EventNode;
					Root.FunctionName = SanitizeCppIdentifier(GetEventFunctionName(EventNode));
					Root.ReturnType = TEXT("void");
					Root.bIsVirtualOverride = IsVirtualOverride(Root.FunctionName);
					Root.bIsConstructor = false;
					if (UFunction* EventFunc = EventNode->FindEventSignatureFunction())
					{
						Root.FunctionFlags = EventFunc->FunctionFlags;
					}
					Root.bIsRPC = (Root.FunctionFlags & FUNC_Net) != 0
						&& ((Root.FunctionFlags & FUNC_NetServer) != 0
							|| (Root.FunctionFlags & FUNC_NetClient) != 0
							|| (Root.FunctionFlags & FUNC_NetMulticast) != 0);

					// Collect event output params
					for (UEdGraphPin* Pin : EventNode->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Output
							&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
							&& Pin->PinName != UK2Node_Event::DelegateOutputName)
						{
							FString ParamType = Mapper.PinTypeToCppType(Pin->PinType);
							FString ParamName = SanitizeCppIdentifier(Pin->PinName.ToString()); // V4-4
							Root.Parameters.Add(TPair<FString, FString>(ParamType, ParamName));
						}
					}

					Roots.Add(MoveTemp(Root));
					continue;
				}

				// Function entry nodes
				if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
				{
					FRootNode Root;
					Root.Node = EntryNode;
					Root.FunctionName = SanitizeCppIdentifier(Graph->GetName());
					Root.ReturnType = TEXT("void");
					Root.bIsVirtualOverride = false;
					Root.bIsConstructor = Graph->GetName().Contains(TEXT("ConstructionScript"));

					// Get function flags from the generated class function
					if (UBlueprint* OwnerBP = Cast<UBlueprint>(Graph->GetOuter()))
					{
						if (OwnerBP->GeneratedClass)
						{
							if (UFunction* Func = OwnerBP->GeneratedClass->FindFunctionByName(*Graph->GetName()))
							{
								Root.FunctionFlags = Func->FunctionFlags;
							}
						}
					}
					Root.bIsRPC = (Root.FunctionFlags & FUNC_Net) != 0
						&& ((Root.FunctionFlags & FUNC_NetServer) != 0
							|| (Root.FunctionFlags & FUNC_NetClient) != 0
							|| (Root.FunctionFlags & FUNC_NetMulticast) != 0);

					// Extract parameters from output pins
					for (UEdGraphPin* Pin : EntryNode->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Output
							&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
						{
							FString ParamType = Mapper.PinTypeToCppType(Pin->PinType);
							FString ParamName = SanitizeCppIdentifier(Pin->PinName.ToString()); // V4-4
							Root.Parameters.Add(TPair<FString, FString>(ParamType, ParamName));
						}
					}

					// Find return type from FunctionResult nodes in this graph
					for (UEdGraphNode* GNode : Graph->Nodes)
					{
						if (UK2Node_FunctionResult* ResultNode = Cast<UK2Node_FunctionResult>(GNode))
						{
							for (UEdGraphPin* Pin : ResultNode->Pins)
							{
								if (Pin && Pin->Direction == EGPD_Input
									&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
								{
									Root.ReturnType = Mapper.PinTypeToCppType(Pin->PinType);
									break;
								}
							}
							break;
						}
					}

					Roots.Add(MoveTemp(Root));
				}
			}
		}

		return Roots;
	}
}

FString ClaireonTool_BlueprintTranslateScaffold::GetName() const
{
	return TEXT("claireon.blueprint_translate_scaffold");
}

FString ClaireonTool_BlueprintTranslateScaffold::GetDescription() const
{
	return TEXT("Generate annotated C++ skeleton files from Blueprint graphs. Phase 1 of the BP-to-C++ "
		"translation pipeline. Extracts class hierarchy, properties, components, and function graphs "
		"into .h/.cpp pairs with //[BP] metadata tags for interactive Phase 2 implementation.");
}

TSharedPtr<FJsonObject> ClaireonTool_BlueprintTranslateScaffold::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> BlueprintsProp = MakeShared<FJsonObject>();
	BlueprintsProp->SetStringField(TEXT("type"), TEXT("array"));
	BlueprintsProp->SetStringField(TEXT("description"),
		TEXT("Asset paths to Blueprint assets to translate (e.g., [\"/Game/Path/BP_MyActor\"])."));
	TSharedPtr<FJsonObject> BlueprintsItems = MakeShared<FJsonObject>();
	BlueprintsItems->SetStringField(TEXT("type"), TEXT("string"));
	BlueprintsProp->SetObjectField(TEXT("items"), BlueprintsItems);
	Properties->SetObjectField(TEXT("blueprints"), BlueprintsProp);

	TSharedPtr<FJsonObject> ModuleProp = MakeShared<FJsonObject>();
	ModuleProp->SetStringField(TEXT("type"), TEXT("string"));
	ModuleProp->SetStringField(TEXT("description"),
		TEXT("UE module name for generated code (e.g., \"MyGame\"). Used for API macro."));
	Properties->SetObjectField(TEXT("target_module"), ModuleProp);

	TSharedPtr<FJsonObject> DirProp = MakeShared<FJsonObject>();
	DirProp->SetStringField(TEXT("type"), TEXT("string"));
	DirProp->SetStringField(TEXT("description"),
		TEXT("Relative path from project root for generated .h/.cpp files."));
	Properties->SetObjectField(TEXT("target_directory"), DirProp);

	TSharedPtr<FJsonObject> PrefixProp = MakeShared<FJsonObject>();
	PrefixProp->SetStringField(TEXT("type"), TEXT("string"));
	PrefixProp->SetStringField(TEXT("description"),
		TEXT("Prefix prepended to generated class names. Empty means Blueprint class name is used directly."));
	Properties->SetObjectField(TEXT("output_class_prefix"), PrefixProp);

	TSharedPtr<FJsonObject> IniProp = MakeShared<FJsonObject>();
	IniProp->SetStringField(TEXT("type"), TEXT("string"));
	IniProp->SetStringField(TEXT("description"),
		TEXT("INI section name for config-driven asset path references. Default: \"BlueprintTranslation\"."));
	Properties->SetObjectField(TEXT("ini_section"), IniProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("blueprints")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("target_module")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("target_directory")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_BlueprintTranslateScaffold::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	using namespace ScaffoldInternal;

	// Parse input parameters
	const TArray<TSharedPtr<FJsonValue>>* BlueprintPaths = nullptr;
	if (!Arguments->TryGetArrayField(TEXT("blueprints"), BlueprintPaths) || !BlueprintPaths || BlueprintPaths->Num() == 0)
	{
		return MakeErrorResult(TEXT("'blueprints' is required and must be a non-empty array of asset paths."));
	}

	FString TargetModule = Arguments->GetStringField(TEXT("target_module"));
	if (TargetModule.IsEmpty())
	{
		return MakeErrorResult(TEXT("'target_module' is required."));
	}

	FString TargetDirectory = Arguments->GetStringField(TEXT("target_directory"));
	if (TargetDirectory.IsEmpty())
	{
		return MakeErrorResult(TEXT("'target_directory' is required."));
	}

	FString OutputClassPrefix = Arguments->GetStringField(TEXT("output_class_prefix"));
	FString IniSection = Arguments->GetStringField(TEXT("ini_section"));
	if (IniSection.IsEmpty())
	{
		IniSection = TEXT("BlueprintTranslation");
	}

	FString ApiMacro = TargetModule.ToUpper() + TEXT("_API");
	FString AbsTargetDir = FPaths::Combine(FPaths::ProjectDir(), TargetDirectory);
	FPaths::NormalizeDirectoryName(AbsTargetDir);

	// Step 1: Pre-validation -- load and compile all blueprints
	TArray<UBlueprint*> Blueprints;
	TArray<FString> AssetPaths;

	for (const TSharedPtr<FJsonValue>& PathVal : *BlueprintPaths)
	{
		FString AssetPath = PathVal->AsString();
		if (AssetPath.IsEmpty())
		{
			continue;
		}
		AssetPaths.Add(AssetPath);

		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!BP)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("LoadObject<UBlueprint> returned nullptr for path: %s"), *AssetPath));
		}

		// Compile to validate
		FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::BatchCompile, nullptr);
		if (BP->Status == BS_Error)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Blueprint compile failed for %s. Fix compile errors before scaffolding."), *AssetPath));
		}

		Blueprints.Add(BP);
	}

	if (Blueprints.Num() == 0)
	{
		return MakeErrorResult(TEXT("No valid blueprints provided."));
	}

	// Check for existing session
	FString SessionCheckPattern = FPaths::Combine(AbsTargetDir, TEXT(".bp_translate_session_*.json"));
	TArray<FString> ExistingSessionFiles;
	IFileManager::Get().FindFiles(ExistingSessionFiles, *AbsTargetDir, TEXT(".json"));
	for (const FString& Filename : ExistingSessionFiles)
	{
		if (Filename.StartsWith(TEXT(".bp_translate_session_")))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Session already exists in %s (%s). Use claireon.blueprint_translate_implement to continue, or delete the session file and re-scaffold."),
				*TargetDirectory, *Filename));
		}
	}

	// Ensure target directory exists
	IFileManager::Get().MakeDirectory(*AbsTargetDir, true);

	// Step 2: Cross-reference analysis (simplified -- record references between BPs in the batch)
	TArray<FClaireonBPTranslateCrossRef> CrossRefs;
	TSet<FString> BatchPaths(AssetPaths);

	// Step 3-7: Process each blueprint
	FClaireonBPNodeMapper NodeMapper;
	FClaireonBPTranslateSession Session;
	Session.SessionId = FString::Printf(TEXT("translate_%s_%s"),
		*FDateTime::Now().ToString(TEXT("%Y%m%d")),
		*MakeShortHash(FString::Join(AssetPaths, TEXT(","))));
	Session.Created = FDateTime::Now().ToIso8601();
	Session.TargetModule = TargetModule;
	Session.TargetDirectory = TargetDirectory;
	Session.IniSection = IniSection;
	Session.Status = TEXT("in_progress");

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> GeneratedFiles;
	TSharedPtr<FJsonObject> PerBPSummary = MakeShared<FJsonObject>();
	FString IniBlock;

	TArray<FString> RollbackFiles; // Files to delete on failure

	for (int32 BPIndex = 0; BPIndex < Blueprints.Num(); ++BPIndex)
	{
		UBlueprint* BP = Blueprints[BPIndex];
		const FString& AssetPath = AssetPaths[BPIndex];

		UClass* ParentClass = BP->ParentClass;
		UClass* GeneratedClass = BP->GeneratedClass;
		FString BPName = BP->GetName();
		FString ClassName = OutputClassPrefix + BPName;

		// Get the C++ class name with prefix
		FString CppClassName;
		if (GeneratedClass)
		{
			CppClassName = FString::Printf(TEXT("%s%s"), GeneratedClass->GetPrefixCPP(), *ClassName);
		}
		else if (ParentClass && ParentClass->IsChildOf<AActor>())
		{
			CppClassName = FString::Printf(TEXT("A%s"), *ClassName);
		}
		else
		{
			CppClassName = FString::Printf(TEXT("U%s"), *ClassName);
		}

		FString ParentCppName = GetClassCppName(ParentClass);
		FString ParentInclude = InferParentIncludePath(ParentClass);

		// Collect all graphs
		TArray<UEdGraph*> AllGraphs = CollectAllGraphs(BP);

		// Validate: at least one graph with nodes
		bool bHasContent = false;
		for (UEdGraph* Graph : AllGraphs)
		{
			if (Graph && Graph->Nodes.Num() > 0)
			{
				bHasContent = true;
				break;
			}
		}
		if (!bHasContent)
		{
			// Rollback any files already written
			for (const FString& Path : RollbackFiles)
			{
				IFileManager::Get().Delete(*Path);
			}
			return MakeErrorResult(FString::Printf(
				TEXT("Blueprint has no translatable graphs: %s"), *AssetPath));
		}

		// Find root nodes
		TArray<FRootNode> RootNodes = FindRootNodes(AllGraphs, NodeMapper);

		// P1-7: Collect component-bound events for BeginPlay AddDynamic injection
		TArray<TPair<FString, FString>> ComponentDelegateBindings; // ComponentName->Delegate, HandlerName
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_ComponentBoundEvent* CompEvent = Cast<UK2Node_ComponentBoundEvent>(Node))
				{
					FString CompName = CompEvent->ComponentPropertyName.ToString();
					FString DelegateName = CompEvent->DelegatePropertyName.ToString();
					FString HandlerName = CompEvent->GetNodeTitle(ENodeTitleType::ListView).ToString();
					HandlerName = SanitizeCppIdentifier(HandlerName);
					if (CompName != TEXT("None") && DelegateName != TEXT("None"))
					{
						ComponentDelegateBindings.Add(TPair<FString, FString>(
							FString::Printf(TEXT("%s->%s"), *CompName, *DelegateName),
							HandlerName));
					}
				}
			}
		}

		// Deduplicate function names
		TMap<FString, int32> FuncNameCounts;
		for (FRootNode& Root : RootNodes)
		{
			int32& Count = FuncNameCounts.FindOrAdd(Root.FunctionName, 0);
			++Count;
			if (Count > 1)
			{
				Root.FunctionName = FString::Printf(TEXT("%s_%d"), *Root.FunctionName, Count);
			}
		}

		// V3-4: Shared subgraph detection
		TMap<FString, TSet<const UEdGraphNode*>> PerEntryReachable;
		TArray<FSharedSubgraph> SharedSubgraphs = DetectSharedSubgraphs(RootNodes, AllGraphs, PerEntryReachable);

		// Build shared subgraph entry node map for EmitScope interception
		TMap<const UEdGraphNode*, FString> SharedNodeToHelperMap;
		for (const FSharedSubgraph& SG : SharedSubgraphs)
		{
			SharedNodeToHelperMap.Add(SG.EntryNode, SG.HelperFunctionName);
		}

		// V3-8: Cross-entry data dependency detection
		TArray<FCrossEntryDataDep> CrossEntryDeps = DetectCrossEntryDataDeps(PerEntryReachable, NodeMapper);

		// Traverse each root and generate code
		FString AllSourceCode;
		FString AllMemberDeclarations;
		TMap<FString, FClaireonBPTranslateNodeStatus> AllNodes;

		// V3-4: Generate shared subgraph helper function bodies
		FString SharedHelperSource;
		FString SharedHelperDeclarations;
		for (const FSharedSubgraph& SG : SharedSubgraphs)
		{
			UE_LOG(LogClaireon, Log, TEXT("Emitting helper body for '%s': EntryNode='%s' (Class=%s), SubgraphSize=%d"),
				*SG.HelperFunctionName,
				*SG.EntryNode->GetNodeTitle(ENodeTitleType::ListView).ToString(),
				*SG.EntryNode->GetClass()->GetName(),
				SG.Nodes.Num());

			FEmitScopeContext HelperCtx(NodeMapper);
			// Helpers use fresh visited set -- do NOT set SharedNodeToHelper on helper traversal
			HelperCtx.EmitScope(SG.EntryNode, 1);

			UE_LOG(LogClaireon, Log, TEXT("Helper '%s' body length: %d chars, visited %d nodes"),
				*SG.HelperFunctionName,
				HelperCtx.Result.SourceCode.Len(),
				HelperCtx.Result.Nodes.Num());

			SharedHelperDeclarations += FString::Printf(TEXT("\t// [BP:SHARED_SUBGRAPH] Extracted from entry points: %s\n"),
				*FString::Join(SG.EntryPointNames, TEXT(", ")));
			SharedHelperDeclarations += FString::Printf(TEXT("\tvoid %s();\n\n"), *SG.HelperFunctionName);

			SharedHelperSource += FString::Printf(TEXT("// [BP:SHARED_SUBGRAPH] Extracted from entry points: %s\n"),
				*FString::Join(SG.EntryPointNames, TEXT(", ")));
			SharedHelperSource += FString::Printf(TEXT("void CLASSNAME::%s()\n{\n"), *SG.HelperFunctionName);
			SharedHelperSource += HelperCtx.Result.SourceCode;
			SharedHelperSource += TEXT("}\n\n");
			AllMemberDeclarations += HelperCtx.Result.MemberDeclarations;

			// V4-2: Verify helper body produced nodes and track them
			if (HelperCtx.Result.Nodes.Num() == 0)
			{
				UE_LOG(LogClaireon, Warning, TEXT("Helper '%s' produced no nodes -- check entry node selection"),
					*SG.HelperFunctionName);
			}
			AllNodes.Append(HelperCtx.Result.Nodes);
		}

		// V3-8: Cross-entry data dep member declarations
		for (const FCrossEntryDataDep& Dep : CrossEntryDeps)
		{
			AllMemberDeclarations += FString::Printf(TEXT("\t// [BP:CROSS_ENTRY_DATA] Source=%s Dest=%s Via=%s\n"),
				*Dep.SourceEntryPoint, *Dep.DestEntryPoint, *Dep.MemberVarName);
			AllMemberDeclarations += FString::Printf(TEXT("\tUPROPERTY()\n\t%s %s{};\n\n"),
				*Dep.PinType, *Dep.MemberVarName);
		}

		for (const FRootNode& Root : RootNodes)
		{
			FEmitScopeContext ScopeCtx(NodeMapper);
			ScopeCtx.SharedNodeToHelper = SharedNodeToHelperMap;
			ScopeCtx.EmitScope(Root.Node, 1);
			AllSourceCode += ScopeCtx.Result.SourceCode;
			AllMemberDeclarations += ScopeCtx.Result.MemberDeclarations;
			AllNodes.Append(ScopeCtx.Result.Nodes);
		}

		// V5-4: Group-based UPROPERTY/UFUNCTION dedup pass -- process specifier+declaration groups
		// to avoid leaving bare UPROPERTY()/UFUNCTION() lines when duplicates are removed
		{
			TArray<FString> Lines;
			AllMemberDeclarations.ParseIntoArrayLines(Lines);
			TSet<FString> SeenNames;
			FString DeduplicatedDeclarations;
			TArray<FString> Buffer; // Accumulates specifier + interleaved lines before the declaration

			for (int32 LineIdx = 0; LineIdx < Lines.Num(); ++LineIdx)
			{
				const FString& Line = Lines[LineIdx];
				FString Trimmed = Line.TrimStartAndEnd();

				// Check if this is a specifier line
				if (Trimmed.Contains(TEXT("UPROPERTY(")) || Trimmed.Contains(TEXT("UFUNCTION(")))
				{
					// If we were already buffering (orphan specifier with no declaration), flush it
					if (Buffer.Num() > 0)
					{
						for (const FString& Buffered : Buffer)
						{
							DeduplicatedDeclarations += Buffered + TEXT("\n");
						}
						Buffer.Empty();
					}
					Buffer.Add(Lines[LineIdx]);
				}
				else if (Buffer.Num() > 0)
				{
					// Currently buffering -- waiting for the declaration line
					if (Trimmed.Contains(TEXT(";")))
					{
						// This is the actual declaration line -- extract name for dedup
						int32 EqIdx = INDEX_NONE;
						Trimmed.FindChar(TEXT('='), EqIdx);
						int32 SemiIdx = INDEX_NONE;
						Trimmed.FindChar(TEXT(';'), SemiIdx);
						int32 EndIdx = (EqIdx != INDEX_NONE) ? EqIdx : SemiIdx;

						FString DeclName;
						if (EndIdx != INDEX_NONE)
						{
							FString BeforeEnd = Trimmed.Left(EndIdx).TrimEnd();
							// Strip trailing () for function declarations
							if (BeforeEnd.EndsWith(TEXT(")")))
							{
								int32 ParenIdx = INDEX_NONE;
								BeforeEnd.FindChar(TEXT('('), ParenIdx);
								if (ParenIdx != INDEX_NONE)
								{
									BeforeEnd = BeforeEnd.Left(ParenIdx);
								}
							}
							int32 LastSpace = INDEX_NONE;
							BeforeEnd.FindLastChar(TEXT(' '), LastSpace);
							if (LastSpace != INDEX_NONE)
							{
								DeclName = BeforeEnd.Mid(LastSpace + 1);
								// Strip pointer/reference from name
								DeclName.RemoveFromStart(TEXT("*"));
								DeclName.RemoveFromStart(TEXT("&"));
							}
						}

						if (!DeclName.IsEmpty() && SeenNames.Contains(DeclName))
						{
							// Duplicate -- discard entire buffer + declaration
							UE_LOG(LogClaireon, Warning, TEXT("Duplicate declaration for '%s' -- keeping first"), *DeclName);
							Buffer.Empty();
						}
						else
						{
							if (!DeclName.IsEmpty())
							{
								SeenNames.Add(DeclName);
							}
							// Emit buffer + declaration
							for (const FString& Buffered : Buffer)
							{
								DeduplicatedDeclarations += Buffered + TEXT("\n");
							}
							DeduplicatedDeclarations += Lines[LineIdx] + TEXT("\n");
							Buffer.Empty();
						}
					}
					else
					{
						// Interleaved comment or blank line between specifier and declaration
						Buffer.Add(Lines[LineIdx]);
					}
				}
				else
				{
					// Not buffering, not a specifier -- pass through
					DeduplicatedDeclarations += Lines[LineIdx] + TEXT("\n");
				}
			}

			// Flush any remaining buffer (orphan specifier at end of input)
			if (Buffer.Num() > 0)
			{
				for (const FString& Buffered : Buffer)
				{
					DeduplicatedDeclarations += Buffered + TEXT("\n");
				}
			}

			AllMemberDeclarations = MoveTemp(DeduplicatedDeclarations);
		}

		// V7-1: Add pure nodes inlined by GetConnectedPinExpression to AllNodes
		// These were resolved during expression evaluation but not tracked by EmitScope
		for (const UEdGraphNode* InlinedNode : NodeMapper.GetInlinedPureNodes())
		{
			if (!InlinedNode) continue;
			FString InlinedGuid = InlinedNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces);
			if (!AllNodes.Contains(InlinedGuid))
			{
				FClaireonBPTranslateNodeStatus InlinedStatus;
				InlinedStatus.Status = TEXT("inlined");
				InlinedStatus.Type = InlinedNode->GetClass()->GetName();
				InlinedStatus.Name = InlinedNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
				AllNodes.Add(InlinedGuid, InlinedStatus);
			}
		}

		// V4-8: Detect and classify orphan nodes
		TSet<const UEdGraphNode*> VisitedNodes;
		for (const auto& Pair : AllNodes)
		{
			for (UEdGraph* Graph : AllGraphs)
			{
				if (!Graph) continue;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (Node && Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces) == Pair.Key)
					{
						VisitedNodes.Add(Node);
					}
				}
			}
		}

		// V6-3: Build transitive closure for orphan classification
		// Collect all orphan nodes and classify pure vs exec
		TSet<const UEdGraphNode*> AllOrphanNodes;
		TSet<const UEdGraphNode*> PureOrphans;
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node || VisitedNodes.Contains(Node)) continue;
				// Check if this node has any connections
				bool bHasConnections = false;
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && Pin->LinkedTo.Num() > 0)
					{
						bHasConnections = true;
						break;
					}
				}
				if (!bHasConnections) continue; // Fully disconnected nodes are ignored

				AllOrphanNodes.Add(Node);

				// Check if this is a pure node (no exec pins)
				bool bHasExecPin = false;
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
					{
						bHasExecPin = true;
						break;
					}
				}
				if (!bHasExecPin)
				{
					PureOrphans.Add(Node);
				}
			}
		}

		// Seed ConsumedPureNodes: pure orphans whose outputs directly connect to visited nodes
		TSet<const UEdGraphNode*> ConsumedPureNodes;
		for (const UEdGraphNode* PureNode : PureOrphans)
		{
			for (const UEdGraphPin* Pin : PureNode->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output) continue;
				for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (LinkedPin && VisitedNodes.Contains(LinkedPin->GetOwningNode()))
					{
						ConsumedPureNodes.Add(PureNode);
						goto NextSeedNode;
					}
				}
			}
			NextSeedNode:;
		}

		// Backward propagation: transitively mark pure orphans feeding into consumed nodes
		{
			TArray<const UEdGraphNode*> Worklist;
			Worklist.Append(ConsumedPureNodes.Array());
			while (Worklist.Num() > 0)
			{
				const UEdGraphNode* Current = Worklist.Pop();
				for (const UEdGraphPin* Pin : Current->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Input) continue;
					for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (!LinkedPin) continue;
						const UEdGraphNode* Predecessor = LinkedPin->GetOwningNode();
						if (PureOrphans.Contains(Predecessor) && !ConsumedPureNodes.Contains(Predecessor))
						{
							ConsumedPureNodes.Add(Predecessor);
							Worklist.Add(Predecessor);
						}
					}
				}
			}
		}

		// Emit orphan classification using transitive closure results
		FString OrphanCode;
		int32 OrphanExecCount = 0;
		int32 OrphanPureConsumedCount = 0;
		int32 OrphanPureDisconnectedCount = 0;
		for (const UEdGraphNode* Node : AllOrphanNodes)
		{
			FString GuidStr = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces);
			if (!PureOrphans.Contains(Node))
			{
				// Exec node that is unreachable
				OrphanCode += FString::Printf(TEXT("\t// [BP:ORPHAN] Unreachable exec node -- verify blueprint connections\n"));
				OrphanCode += FString::Printf(TEXT("\t// Node: %s (Class: %s, Guid: %s)\n"),
					*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
					*Node->GetClass()->GetName(), *GuidStr);
				++OrphanExecCount;
			}
			else if (ConsumedPureNodes.Contains(Node))
			{
				OrphanCode += FString::Printf(TEXT("\t// [BP:ORPHAN] Pure node consumed by visited code but not inlined -- check P3-20 pass\n"));
				OrphanCode += FString::Printf(TEXT("\t// Node: %s (Class: %s, Guid: %s)\n"),
					*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
					*Node->GetClass()->GetName(), *GuidStr);
				++OrphanPureConsumedCount;
			}
			else
			{
				OrphanCode += FString::Printf(TEXT("\t// [BP:ORPHAN] Disconnected pure node\n"));
				OrphanCode += FString::Printf(TEXT("\t// Node: %s (Class: %s, Guid: %s)\n"),
					*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
					*Node->GetClass()->GetName(), *GuidStr);
				++OrphanPureDisconnectedCount;
			}
		}

		UE_LOG(LogClaireon, Log, TEXT("Orphan summary: %d exec (unreachable), %d pure (consumed but not inlined), %d pure (disconnected)"),
			OrphanExecCount, OrphanPureConsumedCount, OrphanPureDisconnectedCount);

		// Collect properties
		FString PropertyDeclarations;
		TSet<FString> AdditionalIncludes;
		TArray<TPair<FString, FString>> ReplicatedVars; // VarName, Condition
		bool bHasReplicatedVars = false;

		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			FString CppType = NodeMapper.PinTypeToCppType(Var.VarType);
			FString Specifiers = NodeMapper.PropertyFlagsToSpecifiers(Var.PropertyFlags, Var.Category.ToString());
			FString IncludePath = NodeMapper.InferIncludePath(Var.VarType);
			if (!IncludePath.IsEmpty() && !IncludePath.StartsWith(TEXT("//")))
			{
				AdditionalIncludes.Add(IncludePath);
			}

			// P1-6: Fix RepNotify placeholder
			if (Specifiers.Contains(TEXT("OnRep_TODO")))
			{
				FString RepFunc = Var.RepNotifyFunc.ToString();
				if (RepFunc.IsEmpty() || RepFunc == TEXT("None"))
				{
					RepFunc = FString::Printf(TEXT("OnRep_%s"), *Var.VarName.ToString());
				}
				Specifiers = Specifiers.Replace(TEXT("OnRep_TODO"), *RepFunc);
			}

			// P1-5: Track replicated vars for GetLifetimeReplicatedProps
			if (Var.PropertyFlags & CPF_Net)
			{
				bHasReplicatedVars = true;
				FString Condition = TEXT("COND_None");
				if (Var.ReplicationCondition != COND_None)
				{
					Condition = StaticEnum<ELifetimeCondition>()->GetNameStringByValue((int64)Var.ReplicationCondition);
				}
				ReplicatedVars.Add(TPair<FString, FString>(Var.VarName.ToString(), Condition));
			}

			FString DefaultVal;
			if (!Var.DefaultValue.IsEmpty())
			{
				DefaultVal = FString::Printf(TEXT(" = %s"), *Var.DefaultValue);
			}

			PropertyDeclarations += FString::Printf(TEXT("\t// [BP:VAR] Name=%s\n"), *Var.VarName.ToString());
			if (!Specifiers.IsEmpty())
			{
				PropertyDeclarations += FString::Printf(TEXT("\tUPROPERTY(%s)\n"), *Specifiers);
			}
			else
			{
				PropertyDeclarations += TEXT("\tUPROPERTY()\n");
			}
			// V7-space: Sanitize variable names to valid C++ identifiers
			FString SanitizedVarName = FClaireonBPNodeMapper::SanitizeCppIdentifier(Var.VarName.ToString());
			PropertyDeclarations += FString::Printf(TEXT("\t%s %s%s;\n\n"), *CppType, *SanitizedVarName, *DefaultVal);
		}

		// P1-10: Collect includes from node mapping
		for (const FString& Inc : NodeMapper.GetAccumulatedIncludes())
		{
			AdditionalIncludes.Add(Inc);
		}

		// P1-10: Collect includes from component types
		if (BP->SimpleConstructionScript)
		{
			for (const USCS_Node* SCSNode : BP->SimpleConstructionScript->GetAllNodes())
			{
				if (SCSNode && SCSNode->ComponentClass)
				{
					FString CompInclude = SCSNode->ComponentClass->GetMetaData(TEXT("ModuleRelativePath"));
					if (!CompInclude.IsEmpty())
					{
						CompInclude = FClaireonBPNodeMapper::StripModuleRelativePrefix(CompInclude); // V4-7
					}
					else
					{
						CompInclude = InferParentIncludePath(SCSNode->ComponentClass);
					}
					if (!CompInclude.IsEmpty()) AdditionalIncludes.Add(CompInclude);
				}
			}
		}

		// P1-10: Collect includes from interface types
		for (const FBPInterfaceDescription& Interface : BP->ImplementedInterfaces)
		{
			if (Interface.Interface)
			{
				FString IntfInclude = Interface.Interface->GetMetaData(TEXT("ModuleRelativePath"));
				if (!IntfInclude.IsEmpty())
				{
					IntfInclude = FClaireonBPNodeMapper::StripModuleRelativePrefix(IntfInclude); // V4-7
					AdditionalIncludes.Add(IntfInclude);
				}
			}
		}

		// P5-24: Collect parent SCS variable names for inherited component filtering
		TSet<FName> ParentSCSNames = GetParentSCSVariableNames(BP);

		// Collect components from SCS
		FString ComponentDeclarations;
		if (BP->SimpleConstructionScript)
		{
			const TArray<USCS_Node*>& SCSNodes = BP->SimpleConstructionScript->GetAllNodes();
			for (const USCS_Node* SCSNode : SCSNodes)
			{
				if (!SCSNode || !SCSNode->ComponentClass)
				{
					continue;
				}
				if (ParentSCSNames.Contains(SCSNode->GetVariableName())) continue; // Skip inherited
				FString CompCppType = GetClassCppName(SCSNode->ComponentClass);
				FString CompVarName = SCSNode->GetVariableName().ToString();

				ComponentDeclarations += FString::Printf(
					TEXT("\tUPROPERTY(VisibleAnywhere, BlueprintReadOnly)\n\tTObjectPtr<%s> %s;\n\n"),
					*CompCppType, *CompVarName);
			}
		}

		// P5-26: Collect interfaces, filtering those already on ancestor C++ classes
		FString InterfaceFilterComments;
		TSet<UClass*> AncestorInterfaces;
		for (UClass* Ancestor = ParentClass; Ancestor; Ancestor = Ancestor->GetSuperClass())
		{
			for (const FImplementedInterface& AncIf : Ancestor->Interfaces)
			{
				AncestorInterfaces.Add(AncIf.Class);
			}
		}

		TArray<FString> InterfaceNames;
		for (const FBPInterfaceDescription& Interface : BP->ImplementedInterfaces)
		{
			if (Interface.Interface)
			{
				// Skip if already on ancestor C++ class
				if (AncestorInterfaces.Contains(Interface.Interface))
				{
					FString InterfaceName = FString::Printf(TEXT("I%s"), *Interface.Interface->GetName());
					InterfaceFilterComments += FString::Printf(TEXT("// [BP:INTERFACE_CHECK] %s already on ancestor -- skipped\n"),
						*InterfaceName);
					continue;
				}
				FString InterfaceName = FString::Printf(TEXT("I%s"), *Interface.Interface->GetName());
				InterfaceNames.Add(InterfaceName);
			}
		}

		// Build class declaration
		FString ClassDecl;
		ClassDecl += FString::Printf(TEXT("class %s %s : public %s"),
			*ApiMacro, *CppClassName, *ParentCppName);
		for (const FString& InterfaceName : InterfaceNames)
		{
			ClassDecl += FString::Printf(TEXT(", public %s"), *InterfaceName);
		}

		// Generate header file
		FString HeaderContent;
		HeaderContent += FString::Printf(TEXT("// Generated by Claireon BP Translator\n"));
		HeaderContent += FString::Printf(TEXT("// Source Blueprint: %s\n"), *AssetPath);
		HeaderContent += TEXT("#pragma once\n\n");
		HeaderContent += TEXT("#include \"CoreMinimal.h\"\n");
		HeaderContent += FString::Printf(TEXT("#include \"%s\"\n"), *ParentInclude);
		for (const FString& Inc : AdditionalIncludes)
		{
			HeaderContent += FString::Printf(TEXT("#include \"%s\"\n"), *Inc);
		}
		HeaderContent += FString::Printf(TEXT("#include \"%s.generated.h\"\n\n"), *ClassName);

		// Forward declarations for cross-references
		for (int32 OtherIdx = 0; OtherIdx < Blueprints.Num(); ++OtherIdx)
		{
			if (OtherIdx == BPIndex) continue;
			UBlueprint* OtherBP = Blueprints[OtherIdx];
			FString OtherClassName = OutputClassPrefix + OtherBP->GetName();
			FString OtherCppName = GetClassCppName(OtherBP->GeneratedClass);
			if (OtherCppName.IsEmpty())
			{
				OtherCppName = FString::Printf(TEXT("A%s"), *OtherClassName);
			}
			HeaderContent += FString::Printf(TEXT("// [BP:XREF] Blueprint=%s Class=%s\n"),
				*AssetPaths[OtherIdx], *OtherCppName);
			HeaderContent += FString::Printf(TEXT("class %s %s;\n\n"), *ApiMacro, *OtherCppName);
		}

		// P5-26: Emit interface filter comments before class declaration
		if (!InterfaceFilterComments.IsEmpty())
		{
			HeaderContent += InterfaceFilterComments;
		}
		HeaderContent += TEXT("UCLASS()\n");
		HeaderContent += ClassDecl + TEXT("\n{\n");
		HeaderContent += TEXT("\tGENERATED_BODY()\n\npublic:\n");
		HeaderContent += FString::Printf(TEXT("\t%s();\n\n"), *CppClassName);

		// P1-5: GetLifetimeReplicatedProps declaration
		if (bHasReplicatedVars)
		{
			HeaderContent += TEXT("\tvirtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;\n\n");
			AdditionalIncludes.Add(TEXT("Net/UnrealNetwork.h"));
		}

		// P1-9: OnConstruction declaration if construction script exists
		{
			bool bHasConstructionScript = false;
			for (const FRootNode& Root : RootNodes)
			{
				if (Root.bIsConstructor) { bHasConstructionScript = true; break; }
			}
			if (bHasConstructionScript)
			{
				HeaderContent += TEXT("\tvirtual void OnConstruction(const FTransform& Transform) override;\n\n");
			}
		}

		// Function declarations
		for (const FRootNode& Root : RootNodes)
		{
			if (Root.bIsConstructor) continue;

			FString ParamStr;
			for (int32 i = 0; i < Root.Parameters.Num(); ++i)
			{
				if (i > 0) ParamStr += TEXT(", ");
				ParamStr += FString::Printf(TEXT("%s %s"),
					*Root.Parameters[i].Key, *Root.Parameters[i].Value);
			}

			FString GuidStr = Root.Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces);
			HeaderContent += FString::Printf(TEXT("\t// [BP:NODE] Guid=%s Type=%s\n"),
				*GuidStr, *Root.Node->GetClass()->GetName());

			if (Root.bIsVirtualOverride)
			{
				FString NormName = NormalizeFuncName(Root.FunctionName);
				HeaderContent += FString::Printf(TEXT("\tvirtual %s %s(%s) override;\n\n"),
					*Root.ReturnType, *NormName, *ParamStr);
			}
			else
			{
				FString UFuncSpec = NodeMapper.FunctionFlagsToSpecifiers(Root.FunctionFlags, TEXT(""));
				if (UFuncSpec.IsEmpty())
				{
					UFuncSpec = TEXT("BlueprintCallable");
				}
				HeaderContent += FString::Printf(TEXT("\tUFUNCTION(%s)\n"), *UFuncSpec);
				HeaderContent += FString::Printf(TEXT("\t%s %s(%s);\n\n"),
					*Root.ReturnType, *Root.FunctionName, *ParamStr);
			}
		}

		// Properties
		if (!PropertyDeclarations.IsEmpty())
		{
			HeaderContent += PropertyDeclarations;
		}

		// Macro-generated member variables
		if (!AllMemberDeclarations.IsEmpty())
		{
			HeaderContent += TEXT("\t// Macro state variables\n");
			HeaderContent += AllMemberDeclarations;
			HeaderContent += TEXT("\n");
		}

		// V3-4: Shared subgraph helper function declarations
		if (!SharedHelperDeclarations.IsEmpty())
		{
			HeaderContent += TEXT("private:\n");
			HeaderContent += SharedHelperDeclarations;
		}

		// Components
		if (!ComponentDeclarations.IsEmpty())
		{
			HeaderContent += ComponentDeclarations;
		}

		// P1-7: BeginPlay declaration if component bindings exist but no BeginPlay root
		bool bHasBeginPlayRoot = false;
		for (const FRootNode& Root : RootNodes)
		{
			FString FuncName = Root.bIsVirtualOverride ? NormalizeFuncName(Root.FunctionName) : Root.FunctionName;
			if (FuncName == TEXT("BeginPlay")) { bHasBeginPlayRoot = true; break; }
		}
		if (!bHasBeginPlayRoot && ComponentDelegateBindings.Num() > 0)
		{
			HeaderContent += TEXT("\tvirtual void BeginPlay() override;\n\n");
		}

		HeaderContent += TEXT("};\n");

		// Generate source file
		FString SourceContent;
		SourceContent += FString::Printf(TEXT("// Generated by Claireon BP Translator\n"));
		SourceContent += FString::Printf(TEXT("// Source Blueprint: %s\n\n"), *AssetPath);
		SourceContent += FString::Printf(TEXT("#include \"%s.h\"\n\n"), *ClassName);

		// Constructor
		SourceContent += FString::Printf(TEXT("%s::%s()\n{\n"), *CppClassName, *CppClassName);
		SourceContent += TEXT("\t// [BP:CONSTRUCTION] Components and defaults\n");

		if (BP->SimpleConstructionScript)
		{
			const TArray<USCS_Node*>& SCSNodes = BP->SimpleConstructionScript->GetAllNodes();
			for (const USCS_Node* SCSNode : SCSNodes)
			{
				if (!SCSNode || !SCSNode->ComponentClass) continue;
				if (ParentSCSNames.Contains(SCSNode->GetVariableName())) continue; // P5-24: Skip inherited
				FString CompCppType = GetClassCppName(SCSNode->ComponentClass);
				FString CompVarName = SCSNode->GetVariableName().ToString();
				SourceContent += FString::Printf(
					TEXT("\t%s = CreateDefaultSubobject<%s>(TEXT(\"%s\"));\n"),
					*CompVarName, *CompCppType, *CompVarName);
			}

			// P1-8: SetupAttachment for component hierarchy
			const USCS_Node* DefaultSceneRoot = nullptr;
			for (const USCS_Node* SCSNode : SCSNodes)
			{
				if (SCSNode && SCSNode->IsRootNode() && !ParentSCSNames.Contains(SCSNode->GetVariableName()))
				{
					DefaultSceneRoot = SCSNode;
					FString VarName = SCSNode->GetVariableName().ToString();
					SourceContent += FString::Printf(TEXT("\tRootComponent = %s;\n"), *VarName);
					break;
				}
			}
			// Build child->parent map from SCS tree via ChildNodes
			TMap<FName, FName> ChildToParentVarName;
			for (const USCS_Node* ParentSCSNode : SCSNodes)
			{
				if (!ParentSCSNode) continue;
				for (const USCS_Node* ChildSCSNode : ParentSCSNode->ChildNodes)
				{
					if (ChildSCSNode)
					{
						ChildToParentVarName.Add(ChildSCSNode->GetVariableName(), ParentSCSNode->GetVariableName());
					}
				}
			}
			for (const USCS_Node* SCSNode : SCSNodes)
			{
				if (!SCSNode || SCSNode == DefaultSceneRoot) continue;
				if (ParentSCSNames.Contains(SCSNode->GetVariableName())) continue; // P5-24: Skip inherited
				FString ChildName = SCSNode->GetVariableName().ToString();
				if (const FName* ParentVarName = ChildToParentVarName.Find(SCSNode->GetVariableName()))
				{
					SourceContent += FString::Printf(TEXT("\t%s->SetupAttachment(%s);\n"), *ChildName, *ParentVarName->ToString());
				}
				else if (DefaultSceneRoot)
				{
					SourceContent += FString::Printf(TEXT("\t%s->SetupAttachment(RootComponent);\n"), *ChildName);
				}
			}

			// P5-25: Emit component template defaults for non-default values
			for (const USCS_Node* SCSNode : SCSNodes)
			{
				if (!SCSNode || !SCSNode->ComponentClass || !SCSNode->ComponentTemplate) continue;
				if (ParentSCSNames.Contains(SCSNode->GetVariableName())) continue;

				FString VarName = SCSNode->GetVariableName().ToString();
				UObject* Template = SCSNode->ComponentTemplate;
				UObject* CDO = SCSNode->ComponentClass->GetDefaultObject();
				if (!CDO) continue;

				// Check common SceneComponent properties
				if (USceneComponent* SceneTemplate = Cast<USceneComponent>(Template))
				{
					USceneComponent* SceneCDO = Cast<USceneComponent>(CDO);
					if (SceneCDO)
					{
						if (!SceneTemplate->GetRelativeLocation().Equals(SceneCDO->GetRelativeLocation(), 0.01f))
						{
							FVector Loc = SceneTemplate->GetRelativeLocation();
							SourceContent += FString::Printf(TEXT("\t%s->SetRelativeLocation(FVector(%.1ff, %.1ff, %.1ff));\n"),
								*VarName, Loc.X, Loc.Y, Loc.Z);
						}
						if (!SceneTemplate->GetRelativeRotation().Equals(SceneCDO->GetRelativeRotation(), 0.01f))
						{
							FRotator Rot = SceneTemplate->GetRelativeRotation();
							SourceContent += FString::Printf(TEXT("\t%s->SetRelativeRotation(FRotator(%.1ff, %.1ff, %.1ff));\n"),
								*VarName, Rot.Pitch, Rot.Yaw, Rot.Roll);
						}
						FVector Scale = SceneTemplate->GetRelativeScale3D();
						FVector CDOScale = SceneCDO->GetRelativeScale3D();
						if (!Scale.Equals(CDOScale, 0.01f))
						{
							SourceContent += FString::Printf(TEXT("\t%s->SetRelativeScale3D(FVector(%.2ff, %.2ff, %.2ff));\n"),
								*VarName, Scale.X, Scale.Y, Scale.Z);
						}
						if (SceneTemplate->bHiddenInGame != SceneCDO->bHiddenInGame)
						{
							SourceContent += FString::Printf(TEXT("\t%s->SetHiddenInGame(%s);\n"),
								*VarName, SceneTemplate->bHiddenInGame ? TEXT("true") : TEXT("false"));
						}
						if (SceneTemplate->GetVisibleFlag() != SceneCDO->GetVisibleFlag())
						{
							SourceContent += FString::Printf(TEXT("\t%s->SetVisibility(%s);\n"),
								*VarName, SceneTemplate->GetVisibleFlag() ? TEXT("true") : TEXT("false"));
						}
					}
				}

				// Check PrimitiveComponent collision profile
				if (UPrimitiveComponent* PrimTemplate = Cast<UPrimitiveComponent>(Template))
				{
					UPrimitiveComponent* PrimCDO = Cast<UPrimitiveComponent>(CDO);
					if (PrimCDO)
					{
						FName ProfileName = PrimTemplate->GetCollisionProfileName();
						FName CDOProfile = PrimCDO->GetCollisionProfileName();
						if (ProfileName != CDOProfile)
						{
							SourceContent += FString::Printf(TEXT("\t%s->SetCollisionProfileName(TEXT(\"%s\"));\n"),
								*VarName, *ProfileName.ToString());
						}
					}
				}
			}
		}

		SourceContent += TEXT("\t// [BP] TODO: implement constructor\n}\n\n");

		// P1-5: GetLifetimeReplicatedProps implementation
		if (bHasReplicatedVars)
		{
			SourceContent += FString::Printf(TEXT("void %s::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const\n{\n"), *CppClassName);
			SourceContent += TEXT("\tSuper::GetLifetimeReplicatedProps(OutLifetimeProps);\n");
			for (const auto& Pair : ReplicatedVars)
			{
				if (Pair.Value == TEXT("COND_None"))
				{
					SourceContent += FString::Printf(TEXT("\tDOREPLIFETIME(%s, %s);\n"), *CppClassName, *Pair.Key);
				}
				else
				{
					SourceContent += FString::Printf(TEXT("\tDOREPLIFETIME_CONDITION(%s, %s, %s);\n"), *CppClassName, *Pair.Key, *Pair.Value);
				}
			}
			SourceContent += TEXT("}\n\n");
		}

		// P1-7: If component bindings exist but no BeginPlay root, generate one
		if (!bHasBeginPlayRoot && ComponentDelegateBindings.Num() > 0)
		{
			// Inject BeginPlay declaration into header (before closing brace)
			// This is done later when we still have HeaderContent available
			SourceContent += FString::Printf(TEXT("void %s::BeginPlay()\n{\n"), *CppClassName);
			SourceContent += TEXT("\tSuper::BeginPlay();\n\n");
			for (const auto& Binding : ComponentDelegateBindings)
			{
				SourceContent += FString::Printf(TEXT("\t%s.AddDynamic(this, &ThisClass::%s);\n"),
					*Binding.Key, *Binding.Value);
			}
			SourceContent += TEXT("}\n\n");
		}

		// Function implementations
		for (const FRootNode& Root : RootNodes)
		{
			// P1-9: Construction script -> OnConstruction override
			if (Root.bIsConstructor)
			{
				FString GuidStr = Root.Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces);
				SourceContent += FString::Printf(TEXT("// [BP:NODE] Guid=%s Type=ConstructionScript\n"), *GuidStr);
				SourceContent += FString::Printf(TEXT("void %s::OnConstruction(const FTransform& Transform)\n{\n"), *CppClassName);
				SourceContent += TEXT("\tSuper::OnConstruction(Transform);\n\n");
				FEmitScopeContext ConstructScopeCtx(NodeMapper);
				ConstructScopeCtx.SharedNodeToHelper = SharedNodeToHelperMap;
				ConstructScopeCtx.EmitScope(Root.Node, 1);
				SourceContent += ConstructScopeCtx.Result.SourceCode;
				SourceContent += TEXT("}\n\n");
				continue;
			}

			FString FuncName = Root.bIsVirtualOverride ? NormalizeFuncName(Root.FunctionName) : Root.FunctionName;

			// RPC functions need _Implementation suffix in .cpp
			FString ImplFuncName = FuncName;
			if (Root.bIsRPC)
			{
				ImplFuncName = FuncName + TEXT("_Implementation");
			}

			FString ParamStr;
			for (int32 i = 0; i < Root.Parameters.Num(); ++i)
			{
				if (i > 0) ParamStr += TEXT(", ");
				ParamStr += FString::Printf(TEXT("%s %s"),
					*Root.Parameters[i].Key, *Root.Parameters[i].Value);
			}

			FString GuidStr = Root.Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces);
			SourceContent += FString::Printf(TEXT("// [BP:NODE] Guid=%s Type=%s\n"),
				*GuidStr, *Root.Node->GetClass()->GetName());
			SourceContent += FString::Printf(TEXT("%s %s::%s(%s)\n{\n"),
				*Root.ReturnType, *CppClassName, *ImplFuncName, *ParamStr);

			// Add Super:: call for virtual overrides
			if (Root.bIsVirtualOverride)
			{
				SourceContent += FString::Printf(TEXT("\tSuper::%s(%s);\n\n"), *FuncName,
					*([&]() -> FString {
						TArray<FString> Names;
						for (const auto& P : Root.Parameters) Names.Add(P.Value);
						return FString::Join(Names, TEXT(", "));
					}()));
			}

			// P1-7: Inject AddDynamic calls in BeginPlay
			if (FuncName == TEXT("BeginPlay") && ComponentDelegateBindings.Num() > 0)
			{
				for (const auto& Binding : ComponentDelegateBindings)
				{
					SourceContent += FString::Printf(TEXT("\t%s.AddDynamic(this, &ThisClass::%s);\n"),
						*Binding.Key, *Binding.Value);
				}
				SourceContent += TEXT("\n");
			}

			// Traverse and generate body
			FEmitScopeContext FuncScopeCtx(NodeMapper);
			FuncScopeCtx.SharedNodeToHelper = SharedNodeToHelperMap;
			FuncScopeCtx.EmitScope(Root.Node, 1);
			// Skip the root node's own code (it's just the event/entry marker)
			// The traversal already emits code for connected nodes
			SourceContent += FuncScopeCtx.Result.SourceCode;

			if (!OrphanCode.IsEmpty() && &Root == &RootNodes.Last())
			{
				SourceContent += TEXT("\n\t// --- Orphan nodes (not reached by exec traversal) ---\n");
				SourceContent += OrphanCode;
			}

			SourceContent += TEXT("}\n\n");
		}

		// V3-4: Append shared subgraph helper function definitions
		if (!SharedHelperSource.IsEmpty())
		{
			FString ResolvedHelpers = SharedHelperSource.Replace(TEXT("CLASSNAME"), *CppClassName);
			SourceContent += ResolvedHelpers;
		}

		// V3-5/V3-6: Emit callback function bodies for Timeline and AsyncAction nodes
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;

				// Timeline callbacks
				if (UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node))
				{
					FString TimelineName = TimelineNode->TimelineName.ToString();
					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (!Pin || Pin->Direction != EGPD_Output
							|| Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;

						FString PinName = Pin->PinName.ToString();
						if ((PinName == TEXT("Update") || PinName == TEXT("Finished"))
							&& Pin->LinkedTo.Num() > 0)
						{
							FString CallbackName = FString::Printf(TEXT("%s_%s"), *TimelineName, *PinName);
							SourceContent += FString::Printf(TEXT("void %s::%s()\n{\n"), *CppClassName, *CallbackName);

							FEmitScopeContext CallbackCtx(NodeMapper);
							for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
							{
								if (LinkedPin && LinkedPin->GetOwningNode())
								{
									CallbackCtx.EmitScope(LinkedPin->GetOwningNode(), 1, nullptr, LinkedPin);
								}
							}
							SourceContent += CallbackCtx.Result.SourceCode;
							SourceContent += TEXT("}\n\n");
						}
					}
				}

				// Async action callbacks
				if (UK2Node_BaseAsyncTask* AsyncNode = Cast<UK2Node_BaseAsyncTask>(Node))
				{
					UClass* ProxyClassPtr = nullptr;
					if (const FObjectPropertyBase* ProxyClassProp = CastField<FObjectPropertyBase>(
						AsyncNode->GetClass()->FindPropertyByName(TEXT("ProxyClass"))))
					{
						ProxyClassPtr = Cast<UClass>(ProxyClassProp->GetObjectPropertyValue_InContainer(AsyncNode));
					}
					FString ActionTypeName = ProxyClassPtr ? ProxyClassPtr->GetName() : TEXT("AsyncTask");
					ActionTypeName.RemoveFromStart(TEXT("AbilityAsync_"));
					ActionTypeName.RemoveFromStart(TEXT("AbilityTask_"));

					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (!Pin || Pin->Direction != EGPD_Output
							|| Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;

						FString PinName = Pin->PinName.ToString();
						if (PinName == TEXT("then")) continue;
						if (Pin->LinkedTo.Num() == 0) continue;

						// V7-dupdef: Include short GUID for unique callback names (matches header declarations)
						FString ShortGuid = Node->NodeGuid.ToString(EGuidFormats::Digits).Left(8);
						FString CallbackName = FString::Printf(TEXT("On%s_%s_%s"),
							*FClaireonBPNodeMapper::SanitizeCppIdentifier(ActionTypeName),
							*ShortGuid,
							*FClaireonBPNodeMapper::SanitizeCppIdentifier(PinName));

						SourceContent += FString::Printf(TEXT("void %s::%s()\n{\n"), *CppClassName, *CallbackName);

						FEmitScopeContext CallbackCtx(NodeMapper);
						for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
						{
							if (LinkedPin && LinkedPin->GetOwningNode())
							{
								CallbackCtx.EmitScope(LinkedPin->GetOwningNode(), 1, nullptr, LinkedPin);
							}
						}
						SourceContent += CallbackCtx.Result.SourceCode;
						SourceContent += TEXT("}\n\n");
					}
				}

				// V8: Latent K2Node_CallFunction callbacks
				if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
				{
					if (const UFunction* TargetFunc = CallNode->GetTargetFunction())
					{
						bool bIsLatent = false;
						for (TFieldIterator<FProperty> It(TargetFunc); It; ++It)
						{
							if (const FStructProperty* StructProp = CastField<FStructProperty>(*It))
							{
								if (StructProp->Struct == FLatentActionInfo::StaticStruct())
								{
									bIsLatent = true;
									break;
								}
							}
						}

						if (bIsLatent)
						{
							FString FuncName = CallNode->FunctionReference.GetMemberName().ToString();
							FuncName = FClaireonBPNodeMapper::SanitizeCppIdentifier(FuncName);
							FString ShortGuid = Node->NodeGuid.ToString(EGuidFormats::Digits).Left(8);
							FString CallbackName = FString::Printf(TEXT("On%s_%s_Complete"), *FuncName, *ShortGuid);

							// Find the "then" exec output pin (the only exec output on latent call functions)
							for (UEdGraphPin* Pin : Node->Pins)
							{
								if (!Pin || Pin->Direction != EGPD_Output
									|| Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;

								if (Pin->LinkedTo.Num() == 0) continue;

								SourceContent += FString::Printf(TEXT("void %s::%s()\n{\n"), *CppClassName, *CallbackName);

								FEmitScopeContext CallbackCtx(NodeMapper);
								for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
								{
									if (LinkedPin && LinkedPin->GetOwningNode())
									{
										CallbackCtx.EmitScope(LinkedPin->GetOwningNode(), 1, nullptr, LinkedPin);
									}
								}
								SourceContent += CallbackCtx.Result.SourceCode;
								SourceContent += TEXT("}\n\n");
								break; // Only one exec output on latent call functions
							}
						}
					}
				}
			}
		}

		// Write files
		FString HeaderPath = FPaths::Combine(AbsTargetDir, ClassName + TEXT(".h"));
		FString SourcePath = FPaths::Combine(AbsTargetDir, ClassName + TEXT(".cpp"));

		if (!FFileHelper::SaveStringToFile(HeaderContent, *HeaderPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			// Rollback
			for (const FString& Path : RollbackFiles)
			{
				IFileManager::Get().Delete(*Path);
			}
			return MakeErrorResult(FString::Printf(TEXT("Failed to write header file: %s"), *HeaderPath));
		}
		RollbackFiles.Add(HeaderPath);

		if (!FFileHelper::SaveStringToFile(SourceContent, *SourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			for (const FString& Path : RollbackFiles)
			{
				IFileManager::Get().Delete(*Path);
			}
			return MakeErrorResult(FString::Printf(TEXT("Failed to write source file: %s"), *SourcePath));
		}
		RollbackFiles.Add(SourcePath);

		// Compute file hashes
		FString HeaderHash = FClaireonBPTranslateSession::ComputeFileHash(HeaderPath);
		FString SourceHash = FClaireonBPTranslateSession::ComputeFileHash(SourcePath);

		// Relative paths for session
		FString RelHeaderPath = FPaths::Combine(TargetDirectory, ClassName + TEXT(".h"));
		FString RelSourcePath = FPaths::Combine(TargetDirectory, ClassName + TEXT(".cpp"));

		// Build session state for this BP
		FClaireonBPTranslateBlueprintState BPState;
		BPState.GeneratedHeader = RelHeaderPath;
		BPState.GeneratedSource = RelSourcePath;
		BPState.HeaderHash = HeaderHash;
		BPState.SourceHash = SourceHash;
		BPState.Nodes = AllNodes;
		BPState.TotalNodes = AllNodes.Num();
		BPState.ImplementedNodes = 0;
		BPState.SkippedNodes = 0;
		Session.Blueprints.Add(AssetPath, BPState);

		// Record generated files
		GeneratedFiles.Add(MakeShared<FJsonValueString>(RelHeaderPath));
		GeneratedFiles.Add(MakeShared<FJsonValueString>(RelSourcePath));

		// Per-BP summary
		TSharedPtr<FJsonObject> BPSummaryObj = MakeShared<FJsonObject>();
		BPSummaryObj->SetNumberField(TEXT("total_nodes"), AllNodes.Num());
		BPSummaryObj->SetNumberField(TEXT("function_count"), RootNodes.Num());
		BPSummaryObj->SetNumberField(TEXT("component_count"),
			BP->SimpleConstructionScript ? BP->SimpleConstructionScript->GetAllNodes().Num() : 0);
		BPSummaryObj->SetNumberField(TEXT("variable_count"), BP->NewVariables.Num());
		PerBPSummary->SetObjectField(AssetPath, BPSummaryObj);

		// Generate INI entries
		// (simplified: track cross-reference asset paths)
		if (Blueprints.Num() > 1)
		{
			IniBlock += FString::Printf(TEXT("[/Script/%s.%s]\n"), *TargetModule, *CppClassName);
			for (int32 OtherIdx = 0; OtherIdx < Blueprints.Num(); ++OtherIdx)
			{
				if (OtherIdx == BPIndex) continue;
				FString OtherClassName = OutputClassPrefix + Blueprints[OtherIdx]->GetName();
				IniBlock += FString::Printf(TEXT("Translated%s=%s.%s_C\n"),
					*OtherClassName, *AssetPaths[OtherIdx], *Blueprints[OtherIdx]->GetName());
			}
			IniBlock += TEXT("\n");
		}
	}

	// Save session
	Session.CrossReferences = CrossRefs;
	FString SessionFilePath = FPaths::Combine(AbsTargetDir,
		FString::Printf(TEXT(".bp_translate_session_%s.json"), *Session.SessionId));
	Session.SaveToFile(SessionFilePath);

	// Build result
	ResultData->SetStringField(TEXT("session_id"), Session.SessionId);
	ResultData->SetArrayField(TEXT("generated_files"), GeneratedFiles);
	ResultData->SetObjectField(TEXT("blueprints"), PerBPSummary);
	if (!IniBlock.IsEmpty())
	{
		ResultData->SetStringField(TEXT("ini_entries"), IniBlock);
	}

	FString Summary = FString::Printf(TEXT("Scaffolded %d blueprint(s) -> %d files, session %s"),
		Blueprints.Num(), GeneratedFiles.Num(), *Session.SessionId);

	return MakeSuccessResult(ResultData, Summary);
}
