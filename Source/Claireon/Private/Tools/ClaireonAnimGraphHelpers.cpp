// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimGraphHelpers.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Animation blueprint headers
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"

// Animation graph editor headers (from AnimGraph module)
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_LinkedAnimLayer.h"
#include "AnimGraphNode_LinkedAnimGraphBase.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateEntryNode.h"
#include "AnimStateConduitNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimStateAliasNode.h"

// Runtime animation node base (from AnimGraphRuntime module)
#include "Animation/AnimNodeBase.h"

// Curve asset used for CustomBlendCurve on state transitions
#include "Curves/CurveFloat.h"

// Blueprint graph
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

// For FBPVariableDescription
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "K2Node_FunctionEntry.h"

// For binding access via reflection
#include "AnimGraphNodeBinding.h"

namespace ClaireonAnimGraphHelpers
{

// ============================================================================
// Asset Loading
// ============================================================================

UAnimBlueprint* LoadAnimBlueprint(const FString& AssetPath, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return nullptr;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	UObject* LoadedObj = FSoftObjectPath(ResolvedPath).TryLoad();
	if (!LoadedObj)
	{
		OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *ResolvedPath);
		return nullptr;
	}

	UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(LoadedObj);
	if (!AnimBP)
	{
		OutError = FString::Printf(TEXT("Asset at %s is not an Animation Blueprint (actual type: %s)"),
			*ResolvedPath, *LoadedObj->GetClass()->GetName());
		return nullptr;
	}

	return AnimBP;
}

// ============================================================================
// Graph Enumeration
// ============================================================================

// Forward declaration for mutual recursion between CollectStateMachineGraphs and CollectAnimGraphSubGraphs
void CollectAnimGraphSubGraphs(UEdGraph* AnimGraph, const FString& ParentName, TArray<FAnimGraphInfo>& OutGraphs);

/** Internal recursive helper to collect graphs from a state machine. */
void CollectStateMachineGraphs(UAnimationStateMachineGraph* SMGraph, const FString& ParentName, TArray<FAnimGraphInfo>& OutGraphs)
{
	if (!SMGraph)
	{
		return;
	}

	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		// State nodes have a bound graph for their pose
		if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
		{
			if (UEdGraph* BoundGraph = StateNode->BoundGraph)
			{
				FAnimGraphInfo Info;
				Info.Name = BoundGraph->GetName();
				Info.Type = TEXT("StateGraph");
				Info.NodeCount = BoundGraph->Nodes.Num();
				Info.Graph = BoundGraph;
				Info.ParentGraphName = ParentName;
				OutGraphs.Add(Info);

				// State pose graphs can contain nested state machines and node-embedded graphs
				CollectAnimGraphSubGraphs(BoundGraph, BoundGraph->GetName(), OutGraphs);
				for (UEdGraphNode* InnerNode : BoundGraph->Nodes)
				{
					if (UAnimGraphNode_StateMachine* NestedSM = Cast<UAnimGraphNode_StateMachine>(InnerNode))
					{
						if (UAnimationStateMachineGraph* NestedSMGraph = NestedSM->EditorStateMachineGraph)
						{
							FAnimGraphInfo SMInfo;
							SMInfo.Name = NestedSMGraph->GetName();
							SMInfo.Type = TEXT("StateMachine");
							SMInfo.NodeCount = NestedSMGraph->Nodes.Num();
							SMInfo.Graph = NestedSMGraph;
							SMInfo.ParentGraphName = BoundGraph->GetName();
							OutGraphs.Add(SMInfo);

							CollectStateMachineGraphs(NestedSMGraph, NestedSMGraph->GetName(), OutGraphs);
						}
					}
				}
			}
		}
		// Transition nodes have a bound graph for their condition
		else if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(Node))
		{
			if (UEdGraph* BoundGraph = TransNode->BoundGraph)
			{
				FAnimGraphInfo Info;
				Info.Name = BoundGraph->GetName();
				Info.Type = TEXT("TransitionGraph");
				Info.NodeCount = BoundGraph->Nodes.Num();
				Info.Graph = BoundGraph;
				Info.ParentGraphName = ParentName;
				OutGraphs.Add(Info);
			}
			// Custom blend graph if present
			if (UEdGraph* CustomBlend = TransNode->CustomTransitionGraph)
			{
				FAnimGraphInfo Info;
				Info.Name = CustomBlend->GetName();
				Info.Type = TEXT("CustomBlendGraph");
				Info.NodeCount = CustomBlend->Nodes.Num();
				Info.Graph = CustomBlend;
				Info.ParentGraphName = ParentName;
				OutGraphs.Add(Info);
			}
		}
		// Conduit nodes have a bound graph
		else if (UAnimStateConduitNode* ConduitNode = Cast<UAnimStateConduitNode>(Node))
		{
			if (UEdGraph* BoundGraph = ConduitNode->BoundGraph)
			{
				FAnimGraphInfo Info;
				Info.Name = BoundGraph->GetName();
				Info.Type = TEXT("ConduitGraph");
				Info.NodeCount = BoundGraph->Nodes.Num();
				Info.Graph = BoundGraph;
				Info.ParentGraphName = ParentName;
				OutGraphs.Add(Info);
			}
		}
	}
}

/** Internal helper to collect graphs from an animation graph (AnimGraph root or state pose graph). */
void CollectAnimGraphSubGraphs(UEdGraph* AnimGraph, const FString& ParentName, TArray<FAnimGraphInfo>& OutGraphs)
{
	if (!AnimGraph)
	{
		return;
	}

	for (UEdGraphNode* Node : AnimGraph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		// State machine nodes contain a state machine graph
		if (UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node))
		{
			if (UAnimationStateMachineGraph* SMGraph = SMNode->EditorStateMachineGraph)
			{
				FAnimGraphInfo Info;
				Info.Name = SMGraph->GetName();
				Info.Type = TEXT("StateMachine");
				Info.NodeCount = SMGraph->Nodes.Num();
				Info.Graph = SMGraph;
				Info.ParentGraphName = ParentName;
				OutGraphs.Add(Info);

				CollectStateMachineGraphs(SMGraph, SMGraph->GetName(), OutGraphs);
			}
			continue;
		}

		// For generic UAnimGraphNode_Base nodes, check for BoundGraph (e.g., Blend Stack
		// internal graphs, per-sample graphs, or other nodes with embedded logic).
		// Access BoundGraph via UProperty reflection since it's not on the base class.
		if (UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node))
		{
			// Look for a BoundGraph UProperty on the node
			FObjectProperty* BoundGraphProp = CastField<FObjectProperty>(
				AnimNode->GetClass()->FindPropertyByName(TEXT("BoundGraph")));
			if (BoundGraphProp)
			{
				UEdGraph* BoundGraph = Cast<UEdGraph>(BoundGraphProp->GetObjectPropertyValue_InContainer(AnimNode));
				if (BoundGraph && BoundGraph->Nodes.Num() > 0)
				{
					FAnimGraphInfo Info;
					Info.Name = BoundGraph->GetName();
					Info.Type = TEXT("NodeGraph");
					Info.NodeCount = BoundGraph->Nodes.Num();
					Info.Graph = BoundGraph;
					Info.ParentGraphName = ParentName;
					OutGraphs.Add(Info);

					// Recursively check for nested sub-graphs within the node graph
					CollectAnimGraphSubGraphs(BoundGraph, BoundGraph->GetName(), OutGraphs);
				}
			}

			// Also check SubGraphs array (some nodes store multiple sub-graphs)
			for (UEdGraph* SubGraph : Node->GetSubGraphs())
			{
				if (SubGraph && SubGraph != AnimGraph)
				{
					// Avoid duplicates (BoundGraph may already be added)
					bool bAlreadyAdded = false;
					for (const FAnimGraphInfo& Existing : OutGraphs)
					{
						if (Existing.Graph == SubGraph)
						{
							bAlreadyAdded = true;
							break;
						}
					}
					if (!bAlreadyAdded && SubGraph->Nodes.Num() > 0)
					{
						FAnimGraphInfo Info;
						Info.Name = SubGraph->GetName();
						Info.Type = TEXT("NodeGraph");
						Info.NodeCount = SubGraph->Nodes.Num();
						Info.Graph = SubGraph;
						Info.ParentGraphName = ParentName;
						OutGraphs.Add(Info);

						CollectAnimGraphSubGraphs(SubGraph, SubGraph->GetName(), OutGraphs);
					}
				}
			}
		}
	}
}

TArray<FAnimGraphInfo> CollectAllGraphs(UAnimBlueprint* AnimBP)
{
	TArray<FAnimGraphInfo> Result;
	if (!AnimBP)
	{
		return Result;
	}

	// Collect AnimGraph roots from FunctionGraphs.
	// For child AnimBPs, FunctionGraphs may be empty — traverse the parent chain.
	UAnimBlueprint* Current = AnimBP;
	while (Current)
	{
		UE_LOG(LogClaireon, Log, TEXT("[CollectAllGraphs] Checking '%s' — FunctionGraphs: %d"),
			*Current->GetName(), Current->FunctionGraphs.Num());
		for (UEdGraph* Graph : Current->FunctionGraphs)
		{
			if (!Graph)
			{
				continue;
			}

			// Check if this is an animation graph (not a regular function graph)
			if (Cast<UAnimationGraph>(Graph))
			{
				FAnimGraphInfo Info;
				Info.Name = Graph->GetName();
				Info.Type = TEXT("AnimGraph");
				Info.NodeCount = Graph->Nodes.Num();
				Info.Graph = Graph;
				// Root graph has no parent
				Result.Add(Info);

				// Collect sub-graphs within this AnimGraph
				CollectAnimGraphSubGraphs(Graph, Graph->GetName(), Result);
			}
		}

		// If we found graphs, stop traversing parents
		if (Result.Num() > 0)
		{
			break;
		}

		// Walk up to parent AnimBP
		UAnimBlueprint* Parent = UAnimBlueprint::GetParentAnimBlueprint(Current);
		UE_LOG(LogClaireon, Log, TEXT("[CollectAllGraphs] Parent of '%s' = '%s'"),
			*Current->GetName(), Parent ? *Parent->GetName() : TEXT("null"));
		Current = Parent;
	}

	return Result;
}

UEdGraph* FindAnimGraphByName(UAnimBlueprint* AnimBP, const FString& GraphName, FString& OutError)
{
	TArray<FAnimGraphInfo> AllGraphs = CollectAllGraphs(AnimBP);

	for (const FAnimGraphInfo& Info : AllGraphs)
	{
		if (Info.Name.Equals(GraphName, ESearchCase::IgnoreCase))
		{
			return Info.Graph;
		}
	}

	// Build error with available names
	FString Available;
	for (const FAnimGraphInfo& Info : AllGraphs)
	{
		if (!Available.IsEmpty())
		{
			Available += TEXT(", ");
		}
		Available += FString::Printf(TEXT("'%s' (%s)"), *Info.Name, *Info.Type);
	}

	OutError = FString::Printf(TEXT("Graph '%s' not found in animation blueprint. Available graphs: %s"),
		*GraphName, Available.IsEmpty() ? TEXT("(none)") : *Available);
	return nullptr;
}

// ============================================================================
// Node Classification
// ============================================================================

FString GetAnimNodeCategory(const UEdGraphNode* Node)
{
	if (!Node)
	{
		return TEXT("unknown");
	}

	// Use IsA for types where we need typed member access
	if (Node->IsA<UAnimGraphNode_StateMachine>())
	{
		return TEXT("state_machine");
	}
	if (Node->IsA<UAnimStateNode>())
	{
		return TEXT("state");
	}
	if (Node->IsA<UAnimStateEntryNode>())
	{
		return TEXT("state_entry");
	}
	if (Node->IsA<UAnimStateTransitionNode>())
	{
		return TEXT("transition");
	}
	if (Node->IsA<UAnimStateConduitNode>())
	{
		return TEXT("conduit");
	}
	// State alias nodes (shared transition sources/targets)
	if (Node->GetClass()->GetName().Contains(TEXT("AnimStateAliasNode")))
	{
		return TEXT("state_alias");
	}
	if (Node->IsA<UAnimGraphNode_LinkedAnimLayer>())
	{
		return TEXT("linked_anim_layer");
	}

	// For remaining types, use class name string matching
	const FString ClassName = Node->GetClass()->GetName();

	if (ClassName.Contains(TEXT("LinkedAnimGraph")))
	{
		return TEXT("linked_anim_graph");
	}
	if (ClassName.Contains(TEXT("SequencePlayer")))
	{
		return TEXT("sequence_player");
	}
	if (ClassName.Contains(TEXT("BlendList")) || ClassName.Contains(TEXT("BlendListBy")))
	{
		return TEXT("blend_list");
	}
	if (ClassName.Contains(TEXT("LayeredBoneBlend")) || ClassName.Contains(TEXT("NamedLayeredBoneBlend")))
	{
		return TEXT("layered_bone_blend");
	}
	if (ClassName.Contains(TEXT("BlendSpace")))
	{
		return TEXT("blend_space");
	}
	if (ClassName.Contains(TEXT("AimOffset")))
	{
		return TEXT("aim_offset");
	}
	if (ClassName.Contains(TEXT("SaveCachedPose")))
	{
		return TEXT("cached_pose_save");
	}
	if (ClassName.Contains(TEXT("UseCachedPose")))
	{
		return TEXT("cached_pose_use");
	}
	if (ClassName.Contains(TEXT("Slot")) && !ClassName.Contains(TEXT("BlendSlot")))
	{
		return TEXT("montage_slot");
	}
	if (ClassName.Contains(TEXT("TransitionResult")))
	{
		return TEXT("transition_result");
	}
	if (ClassName.Contains(TEXT("OffsetRootBone")) || ClassName.Contains(TEXT("RotateRootBone")) || ClassName.Contains(TEXT("ResetRoot")) || ClassName.Contains(TEXT("OverrideRootMotion")))
	{
		return TEXT("anim_node");
	}
	if (ClassName == TEXT("AnimGraphNode_Root"))
	{
		return TEXT("output_pose");
	}
	if (ClassName.Contains(TEXT("PoseSearch")))
	{
		return TEXT("pose_search");
	}
	if (ClassName.Contains(TEXT("BlendStack")))
	{
		return TEXT("blend_stack");
	}
	if (ClassName.Contains(TEXT("ControlRig")))
	{
		return TEXT("control_rig");
	}
	if (ClassName.Contains(TEXT("Inertialization")))
	{
		return TEXT("inertialization");
	}
	if (ClassName.Contains(TEXT("TwoWayBlend")))
	{
		return TEXT("two_way_blend");
	}
	if (ClassName.Contains(TEXT("ApplyAdditive")))
	{
		return TEXT("apply_additive");
	}
	if (ClassName.Contains(TEXT("ModifyCurve")))
	{
		return TEXT("modify_curve");
	}

	// Generic animation node
	if (Node->IsA<UAnimGraphNode_Base>())
	{
		return TEXT("anim_node");
	}

	return TEXT("unknown");
}

// ============================================================================
// Pin Serialization
// ============================================================================

/** Get a human-readable pin type string. */
FString GetPinTypeString(const UEdGraphPin* Pin)
{
	if (!Pin)
	{
		return TEXT("unknown");
	}

	const FEdGraphPinType& PinType = Pin->PinType;

	// Check for pose pins
	if (PinType.PinCategory == UAnimationGraphSchema::PC_Struct)
	{
		if (UScriptStruct* Struct = Cast<UScriptStruct>(PinType.PinSubCategoryObject.Get()))
		{
			const FString StructName = Struct->GetName();
			if (StructName == TEXT("PoseLink"))
			{
				return TEXT("pose");
			}
			if (StructName == TEXT("ComponentSpacePoseLink"))
			{
				return TEXT("component_space_pose");
			}
			return StructName;
		}
	}

	// Standard pin categories
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
	{
		return TEXT("exec");
	}
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
	{
		return TEXT("bool");
	}
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Float || PinType.PinCategory == UEdGraphSchema_K2::PC_Real || PinType.PinCategory == UEdGraphSchema_K2::PC_Double)
	{
		return TEXT("float");
	}
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
	{
		return TEXT("int");
	}
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Name)
	{
		return TEXT("name");
	}
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_String)
	{
		return TEXT("string");
	}
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Object)
	{
		if (UClass* ObjClass = Cast<UClass>(PinType.PinSubCategoryObject.Get()))
		{
			return FString::Printf(TEXT("object<%s>"), *ObjClass->GetName());
		}
		return TEXT("object");
	}
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
	{
		if (UScriptStruct* Struct = Cast<UScriptStruct>(PinType.PinSubCategoryObject.Get()))
		{
			return FString::Printf(TEXT("struct<%s>"), *Struct->GetName());
		}
		return TEXT("struct");
	}
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Enum || PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
	{
		if (UEnum* Enum = Cast<UEnum>(PinType.PinSubCategoryObject.Get()))
		{
			return FString::Printf(TEXT("enum<%s>"), *Enum->GetName());
		}
		return PinType.PinCategory == UEdGraphSchema_K2::PC_Enum ? TEXT("enum") : TEXT("byte");
	}
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
	{
		return TEXT("wildcard");
	}

	return PinType.PinCategory.ToString();
}

TArray<TSharedPtr<FJsonValue>> SerializeAllPins(const UEdGraphNode* Node, bool bIncludeDefaults)
{
	TArray<TSharedPtr<FJsonValue>> PinsArray;
	if (!Node)
	{
		return PinsArray;
	}

	for (const UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->bHidden)
		{
			continue;
		}

		TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
		PinObj->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
		PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
		PinObj->SetStringField(TEXT("pin_type"), GetPinTypeString(Pin));
		PinObj->SetNumberField(TEXT("connection_count"), Pin->LinkedTo.Num());

		// Connected-to details
		if (Pin->LinkedTo.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ConnectedArray;
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin || !LinkedPin->GetOwningNode())
				{
					continue;
				}
				TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
				ConnObj->SetStringField(TEXT("node_id"), LinkedPin->GetOwningNode()->NodeGuid.ToString());
				ConnObj->SetStringField(TEXT("node_title"), LinkedPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString());
				ConnObj->SetStringField(TEXT("pin_name"), LinkedPin->PinName.ToString());
				ConnectedArray.Add(MakeShared<FJsonValueObject>(ConnObj));
			}
			PinObj->SetArrayField(TEXT("connected_to"), ConnectedArray);
		}

		// Default value
		if (bIncludeDefaults && Pin->LinkedTo.Num() == 0 && !Pin->DefaultValue.IsEmpty())
		{
			PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
		}
		if (bIncludeDefaults && !Pin->DefaultObject)
		{
			// DefaultObject is used for object-type defaults
		}
		else if (bIncludeDefaults && Pin->DefaultObject)
		{
			PinObj->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
		}

		PinsArray.Add(MakeShared<FJsonValueObject>(PinObj));
	}

	return PinsArray;
}

// ============================================================================
// Runtime FAnimNode Property Reflection
// ============================================================================

TSharedPtr<FJsonObject> SerializeAnimNodeProperties(UAnimGraphNode_Base* AnimNode)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!AnimNode)
	{
		return Result;
	}

	// Find the FAnimNode_Base-derived struct property on the node
	const UClass* NodeClass = AnimNode->GetClass();
	const FStructProperty* AnimNodeStructProp = nullptr;
	const UScriptStruct* AnimNodeStruct = nullptr;

	for (TFieldIterator<FStructProperty> It(NodeClass); It; ++It)
	{
		FStructProperty* StructProp = *It;
		if (StructProp && StructProp->Struct && StructProp->Struct->IsChildOf(FAnimNode_Base::StaticStruct()))
		{
			AnimNodeStructProp = StructProp;
			AnimNodeStruct = StructProp->Struct;
			break;
		}
	}

	if (!AnimNodeStructProp || !AnimNodeStruct)
	{
		Result->SetStringField(TEXT("_error"), TEXT("Could not find FAnimNode_Base struct property on node"));
		return Result;
	}

	Result->SetStringField(TEXT("_struct_type"), AnimNodeStruct->GetName());
	Result->SetStringField(TEXT("_struct_member_name"), AnimNodeStructProp->GetName());

	// Get pointer to the struct data
	const uint8* StructData = AnimNodeStructProp->ContainerPtrToValuePtr<uint8>(AnimNode);
	if (!StructData)
	{
		Result->SetStringField(TEXT("_error"), TEXT("Could not get struct data pointer"));
		return Result;
	}

	// Iterate all properties on the FAnimNode struct
	for (TFieldIterator<FProperty> PropIt(AnimNodeStruct); PropIt; ++PropIt)
	{
		const FProperty* Prop = *PropIt;
		if (!Prop)
		{
			continue;
		}

		// Skip transient and deprecated properties
		if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
		{
			continue;
		}

		// Skip pose link properties (they're covered by pin connections)
		if (const FStructProperty* InnerStructProp = CastField<FStructProperty>(Prop))
		{
			if (InnerStructProp->Struct)
			{
				const FString StructName = InnerStructProp->Struct->GetName();
				if (StructName == TEXT("PoseLink") || StructName == TEXT("ComponentSpacePoseLink"))
				{
					continue;
				}
			}
		}

		FString ValueStr;
		Prop->ExportText_Direct(ValueStr,
			StructData + Prop->GetOffset_ForInternal(),
			StructData + Prop->GetOffset_ForInternal(),
			nullptr,
			PPF_None);

		Result->SetStringField(Prop->GetName(), ValueStr);
	}

	return Result;
}

// ============================================================================
// Property Bindings & Fast Path
// ============================================================================

/**
 * Helper: Access the PropertyBindings TMap from the binding object via UProperty reflection.
 * The binding object (UAnimGraphNodeBinding_Base) stores PropertyBindings as a private UPROPERTY.
 * We access it via reflection since the old direct access was deprecated.
 */
const TMap<FName, FAnimGraphNodePropertyBinding>* GetPropertyBindingsMap(UAnimGraphNode_Base* AnimNode)
{
	if (!AnimNode)
	{
		return nullptr;
	}

	const UAnimGraphNodeBinding* BindingObj = AnimNode->GetBinding();
	if (!BindingObj)
	{
		return nullptr;
	}

	// Find the PropertyBindings UPROPERTY on the binding object via reflection
	const FMapProperty* MapProp = CastField<FMapProperty>(
		BindingObj->GetClass()->FindPropertyByName(TEXT("PropertyBindings")));
	if (!MapProp)
	{
		return nullptr;
	}

	return MapProp->ContainerPtrToValuePtr<TMap<FName, FAnimGraphNodePropertyBinding>>(BindingObj);
}

TSharedPtr<FJsonObject> SerializePropertyBindings(UAnimGraphNode_Base* AnimNode)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!AnimNode)
	{
		return Result;
	}

	const TMap<FName, FAnimGraphNodePropertyBinding>* BindingsPtr = GetPropertyBindingsMap(AnimNode);

	TArray<TSharedPtr<FJsonValue>> BindingsArray;
	int32 FastPathCount = 0;
	int32 NonFastPathCount = 0;

	if (BindingsPtr)
	{
		for (const auto& Pair : *BindingsPtr)
		{
			TSharedPtr<FJsonObject> BindingObj = MakeShared<FJsonObject>();
			BindingObj->SetStringField(TEXT("property_name"), Pair.Key.ToString());

			const FAnimGraphNodePropertyBinding& Binding = Pair.Value;
			BindingObj->SetStringField(TEXT("binding_source_path"), FString::Join(Binding.PropertyPath, TEXT(".")));

			// Check fast path compliance
			const bool bIsFastPath = (Binding.Type == EAnimGraphNodePropertyBindingType::Property);
			BindingObj->SetBoolField(TEXT("is_fast_path"), bIsFastPath);

			if (bIsFastPath)
			{
				FastPathCount++;
			}
			else
			{
				NonFastPathCount++;
			}

			// Binding type
			FString TypeStr;
			switch (Binding.Type)
			{
			case EAnimGraphNodePropertyBindingType::Property:
				TypeStr = TEXT("Property");
				break;
			case EAnimGraphNodePropertyBindingType::Function:
				TypeStr = TEXT("Function");
				break;
			default:
				TypeStr = TEXT("Unknown");
				break;
			}
			BindingObj->SetStringField(TEXT("binding_type"), TypeStr);

			BindingsArray.Add(MakeShared<FJsonValueObject>(BindingObj));
		}
	}

	Result->SetArrayField(TEXT("bindings"), BindingsArray);
	Result->SetNumberField(TEXT("total_bindings"), BindingsArray.Num());
	Result->SetNumberField(TEXT("fast_path_count"), FastPathCount);
	Result->SetNumberField(TEXT("non_fast_path_count"), NonFastPathCount);

	return Result;
}

bool AnalyzeFastPath(UAnimGraphNode_Base* AnimNode, TArray<FString>& OutWarnings)
{
	if (!AnimNode)
	{
		return true;
	}

	// Use the engine's compiler message — it's authoritative for all cases:
	// property bindings, pin-connected BP logic, transitions, everything.
	if (AnimNode->bHasCompilerMessage && !AnimNode->ErrorMsg.IsEmpty() &&
		AnimNode->ErrorMsg.Contains(TEXT("uses Blueprint to update its values")))
	{
		OutWarnings.Add(AnimNode->ErrorMsg);
		return false;
	}

	return true;
}

// ============================================================================
// Linked Layer Inspection
// ============================================================================

TSharedPtr<FJsonObject> SerializeLinkedLayerInfo(UAnimGraphNode_Base* AnimNode)
{
	UAnimGraphNode_LinkedAnimLayer* LayerNode = Cast<UAnimGraphNode_LinkedAnimLayer>(AnimNode);
	if (!LayerNode)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Use reflection to read interface and guid since the getters are not public
	// Read Interface property via ClaireonPropertyUtils
	TSharedPtr<FJsonObject> LayerProps = ClaireonPropertyUtils::GetAllProperties(LayerNode, TEXT(""), 1);
	if (LayerProps)
	{
		// Extract interface-related properties
		FString InterfaceStr;
		if (LayerProps->TryGetStringField(TEXT("Interface"), InterfaceStr))
		{
			Result->SetStringField(TEXT("interface"), InterfaceStr);
		}

		// Extract layer guid
		FString GuidStr;
		if (LayerProps->TryGetStringField(TEXT("LayerGuid"), GuidStr))
		{
			Result->SetStringField(TEXT("layer_guid"), GuidStr);
		}
	}

	// Check pin connections to determine if the layer is connected
	bool bIsConnected = false;
	for (const UEdGraphPin* Pin : LayerNode->Pins)
	{
		if (Pin && Pin->LinkedTo.Num() > 0)
		{
			bIsConnected = true;
			break;
		}
	}
	Result->SetBoolField(TEXT("has_connections"), bIsConnected);

	return Result;
}

// ============================================================================
// Node-Bound Event Functions
// ============================================================================

TArray<TSharedPtr<FJsonValue>> SerializeNodeBoundEvents(UEdGraphNode* Node, UAnimBlueprint* AnimBP)
{
	TArray<TSharedPtr<FJsonValue>> EventsArray;
	if (!Node || !AnimBP)
	{
		return EventsArray;
	}

	// State nodes have specific bound events
	if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
	{
		// Check for custom event graphs bound to this state
		// State nodes can have OnStateEntered, OnStateLeft, OnStateFullyBlended events

		auto AddEventInfo = [&](const FString& EventName, bool bIsBound, const FString& GraphName = TEXT(""))
		{
			TSharedPtr<FJsonObject> EventObj = MakeShared<FJsonObject>();
			EventObj->SetStringField(TEXT("event_name"), EventName);
			EventObj->SetBoolField(TEXT("is_bound"), bIsBound);
			if (bIsBound && !GraphName.IsEmpty())
			{
				EventObj->SetStringField(TEXT("bound_graph_name"), GraphName);
			}
			EventsArray.Add(MakeShared<FJsonValueObject>(EventObj));
		};

		// Check state notification types through reflection
		// UAnimStateNode stores event overrides - we can check using ClaireonPropertyUtils
		FString Error;
		FString AutoRuleBasedOnSequence = ClaireonPropertyUtils::ReadPropertyByPath(StateNode, TEXT("bAlwaysResetOnEntry"), Error);
		if (!AutoRuleBasedOnSequence.IsEmpty())
		{
			// This state has the property accessible
		}

		// Examine bound graph for event-like nodes
		if (UEdGraph* BoundGraph = StateNode->BoundGraph)
		{
			for (UEdGraphNode* InnerNode : BoundGraph->Nodes)
			{
				if (!InnerNode)
				{
					continue;
				}
				const FString InnerClassName = InnerNode->GetClass()->GetName();
				if (InnerClassName.Contains(TEXT("OnBecomeRelevant")) || InnerClassName.Contains(TEXT("OnUpdate")) ||
					InnerClassName.Contains(TEXT("OnInitialize")))
				{
					AddEventInfo(InnerClassName, true, BoundGraph->GetName());
				}
			}
		}
	}

	// Transition nodes can have custom event bindings
	if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(Node))
	{
		// Check for custom transition events
		if (UEdGraph* BoundGraph = TransNode->BoundGraph)
		{
			TSharedPtr<FJsonObject> EventObj = MakeShared<FJsonObject>();
			EventObj->SetStringField(TEXT("event_name"), TEXT("TransitionCondition"));
			EventObj->SetBoolField(TEXT("is_bound"), BoundGraph->Nodes.Num() > 0);
			EventObj->SetStringField(TEXT("bound_graph_name"), BoundGraph->GetName());
			EventObj->SetNumberField(TEXT("condition_node_count"), BoundGraph->Nodes.Num());
			EventsArray.Add(MakeShared<FJsonValueObject>(EventObj));
		}
		if (UEdGraph* CustomBlend = TransNode->CustomTransitionGraph)
		{
			TSharedPtr<FJsonObject> EventObj = MakeShared<FJsonObject>();
			EventObj->SetStringField(TEXT("event_name"), TEXT("CustomBlend"));
			EventObj->SetBoolField(TEXT("is_bound"), CustomBlend->Nodes.Num() > 0);
			EventObj->SetStringField(TEXT("bound_graph_name"), CustomBlend->GetName());
			EventsArray.Add(MakeShared<FJsonValueObject>(EventObj));
		}
	}

	// For generic anim graph nodes, check the three FMemberReference function bindings
	if (UAnimGraphNode_Base* AnimGraphNode = Cast<UAnimGraphNode_Base>(Node))
	{
		auto AddFunctionEvent = [&](const FString& EventName, const FMemberReference& FuncRef)
		{
			bool bIsBound = UAnimGraphNode_Base::IsPotentiallyBoundFunction(FuncRef);
			TSharedPtr<FJsonObject> EventObj = MakeShared<FJsonObject>();
			EventObj->SetStringField(TEXT("event_name"), EventName);
			EventObj->SetBoolField(TEXT("is_bound"), bIsBound);
			if (bIsBound)
			{
				EventObj->SetStringField(TEXT("function_name"), FuncRef.GetMemberName().ToString());
			}
			EventsArray.Add(MakeShared<FJsonValueObject>(EventObj));
		};

		AddFunctionEvent(TEXT("OnInitialUpdate"), AnimGraphNode->InitialUpdateFunction);
		AddFunctionEvent(TEXT("OnBecomeRelevant"), AnimGraphNode->BecomeRelevantFunction);
		AddFunctionEvent(TEXT("OnUpdate"), AnimGraphNode->UpdateFunction);
	}

	return EventsArray;
}

// ============================================================================
// Node Serialization
// ============================================================================

TSharedPtr<FJsonObject> SerializeAnimGraphNode(UEdGraphNode* Node, const FString& DetailLevel, UAnimBlueprint* AnimBP)
{
	TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
	if (!Node)
	{
		return NodeObj;
	}

	// Basic info (always included)
	NodeObj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
	NodeObj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
	NodeObj->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
	NodeObj->SetStringField(TEXT("category"), GetAnimNodeCategory(Node));

	TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
	PosObj->SetNumberField(TEXT("x"), Node->NodePosX);
	PosObj->SetNumberField(TEXT("y"), Node->NodePosY);
	NodeObj->SetObjectField(TEXT("position"), PosObj);

	// Node comment
	if (!Node->NodeComment.IsEmpty())
	{
		NodeObj->SetStringField(TEXT("comment"), Node->NodeComment);
	}

	// Compiler messages (the engine's authoritative fast-path and other warnings)
	if (Node->bHasCompilerMessage)
	{
		NodeObj->SetBoolField(TEXT("has_compiler_message"), true);
		NodeObj->SetNumberField(TEXT("compiler_error_type"), Node->ErrorType);
		NodeObj->SetStringField(TEXT("compiler_message"), Node->ErrorMsg);
	}

	// Summary: add connection counts
	if (DetailLevel == TEXT("summary"))
	{
		int32 InputCount = 0;
		int32 OutputCount = 0;
		int32 ConnectedInputs = 0;
		int32 ConnectedOutputs = 0;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->bHidden)
			{
				continue;
			}
			if (Pin->Direction == EGPD_Input)
			{
				InputCount++;
				if (Pin->LinkedTo.Num() > 0) ConnectedInputs++;
			}
			else
			{
				OutputCount++;
				if (Pin->LinkedTo.Num() > 0) ConnectedOutputs++;
			}
		}
		NodeObj->SetNumberField(TEXT("input_pin_count"), InputCount);
		NodeObj->SetNumberField(TEXT("output_pin_count"), OutputCount);
		NodeObj->SetNumberField(TEXT("connected_inputs"), ConnectedInputs);
		NodeObj->SetNumberField(TEXT("connected_outputs"), ConnectedOutputs);
	}

	// Nodes and Full detail: include all pins
	if (DetailLevel == TEXT("nodes") || DetailLevel == TEXT("full"))
	{
		NodeObj->SetArrayField(TEXT("pins"), SerializeAllPins(Node, DetailLevel == TEXT("full")));
	}

	// Full detail: include bindings and fast path
	if (DetailLevel == TEXT("full"))
	{
		if (UAnimGraphNode_Base* AnimGraphNode = Cast<UAnimGraphNode_Base>(Node))
		{
			// Property bindings (informational — shows what bindings exist)
			TSharedPtr<FJsonObject> Bindings = SerializePropertyBindings(AnimGraphNode);
			if (Bindings)
			{
				NodeObj->SetObjectField(TEXT("property_bindings"), Bindings);
			}

			// Fast path: use engine's compiler message (authoritative for all cases)
			const bool bFastPath = !(Node->bHasCompilerMessage && !Node->ErrorMsg.IsEmpty() &&
				Node->ErrorMsg.Contains(TEXT("uses Blueprint to update its values")));
			NodeObj->SetBoolField(TEXT("is_fast_path"), bFastPath);

			// Linked layer info
			TSharedPtr<FJsonObject> LayerInfo = SerializeLinkedLayerInfo(AnimGraphNode);
			if (LayerInfo)
			{
				NodeObj->SetObjectField(TEXT("linked_layer_info"), LayerInfo);
			}
		}

		// Node-bound events
		if (AnimBP)
		{
			TArray<TSharedPtr<FJsonValue>> Events = SerializeNodeBoundEvents(Node, AnimBP);
			if (Events.Num() > 0)
			{
				NodeObj->SetArrayField(TEXT("bound_events"), Events);
			}
		}
	}

	// Sub-graph references for state machine nodes
	if (UAnimGraphNode_StateMachine* SMNode = Cast<UAnimGraphNode_StateMachine>(Node))
	{
		if (SMNode->EditorStateMachineGraph)
		{
			TSharedPtr<FJsonObject> SubGraph = MakeShared<FJsonObject>();
			SubGraph->SetStringField(TEXT("name"), SMNode->EditorStateMachineGraph->GetName());
			SubGraph->SetStringField(TEXT("type"), TEXT("StateMachine"));
			SubGraph->SetNumberField(TEXT("node_count"), SMNode->EditorStateMachineGraph->Nodes.Num());
			NodeObj->SetObjectField(TEXT("sub_graph"), SubGraph);
		}
	}
	else if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
	{
		if (StateNode->BoundGraph)
		{
			TSharedPtr<FJsonObject> SubGraph = MakeShared<FJsonObject>();
			SubGraph->SetStringField(TEXT("name"), StateNode->BoundGraph->GetName());
			SubGraph->SetStringField(TEXT("type"), TEXT("StateGraph"));
			SubGraph->SetNumberField(TEXT("node_count"), StateNode->BoundGraph->Nodes.Num());
			NodeObj->SetObjectField(TEXT("sub_graph"), SubGraph);
		}
	}
	else if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(Node))
	{
		if (TransNode->BoundGraph)
		{
			TSharedPtr<FJsonObject> SubGraph = MakeShared<FJsonObject>();
			SubGraph->SetStringField(TEXT("name"), TransNode->BoundGraph->GetName());
			SubGraph->SetStringField(TEXT("type"), TEXT("TransitionGraph"));
			SubGraph->SetNumberField(TEXT("node_count"), TransNode->BoundGraph->Nodes.Num());
			NodeObj->SetObjectField(TEXT("sub_graph"), SubGraph);
		}

		// Add transition summary info
		UAnimStateNodeBase* PrevState = TransNode->GetPreviousState();
		UAnimStateNodeBase* NextState = TransNode->GetNextState();
		NodeObj->SetStringField(TEXT("from_state"), PrevState ? PrevState->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("Unknown"));
		NodeObj->SetStringField(TEXT("to_state"), NextState ? NextState->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("Unknown"));
		NodeObj->SetNumberField(TEXT("crossfade_duration"), TransNode->CrossfadeDuration);

		// Shared transition rules
		if (TransNode->bSharedRules)
		{
			TSharedPtr<FJsonObject> SharedObj = MakeShared<FJsonObject>();
			SharedObj->SetStringField(TEXT("name"), TransNode->SharedRulesName);
			SharedObj->SetStringField(TEXT("guid"), TransNode->SharedRulesGuid.ToString());
			NodeObj->SetObjectField(TEXT("shared_rules"), SharedObj);
		}
		if (TransNode->bSharedCrossfade)
		{
			TSharedPtr<FJsonObject> SharedObj = MakeShared<FJsonObject>();
			SharedObj->SetStringField(TEXT("name"), TransNode->SharedCrossfadeName);
			SharedObj->SetStringField(TEXT("guid"), TransNode->SharedCrossfadeGuid.ToString());
			NodeObj->SetObjectField(TEXT("shared_crossfade"), SharedObj);
		}
	}
	else if (UAnimStateConduitNode* ConduitNode = Cast<UAnimStateConduitNode>(Node))
	{
		if (ConduitNode->BoundGraph)
		{
			TSharedPtr<FJsonObject> SubGraph = MakeShared<FJsonObject>();
			SubGraph->SetStringField(TEXT("name"), ConduitNode->BoundGraph->GetName());
			SubGraph->SetStringField(TEXT("type"), TEXT("ConduitGraph"));
			SubGraph->SetNumberField(TEXT("node_count"), ConduitNode->BoundGraph->Nodes.Num());
			NodeObj->SetObjectField(TEXT("sub_graph"), SubGraph);
		}
	}
	else if (UAnimStateAliasNode* AliasNode = Cast<UAnimStateAliasNode>(Node))
	{
		NodeObj->SetBoolField(TEXT("is_global_alias"), AliasNode->bGlobalAlias);

		// Serialize which states this alias represents
		TArray<TSharedPtr<FJsonValue>> AliasedArray;
		for (const TWeakObjectPtr<UAnimStateNodeBase>& StatePtr : AliasNode->GetAliasedStates())
		{
			if (UAnimStateNodeBase* AliasedState = StatePtr.Get())
			{
				TSharedPtr<FJsonObject> AliasedObj = MakeShared<FJsonObject>();
				AliasedObj->SetStringField(TEXT("name"), AliasedState->GetNodeTitle(ENodeTitleType::ListView).ToString());
				AliasedObj->SetStringField(TEXT("guid"), AliasedState->NodeGuid.ToString());
				AliasedArray.Add(MakeShared<FJsonValueObject>(AliasedObj));
			}
		}
		NodeObj->SetArrayField(TEXT("aliased_states"), AliasedArray);
	}

	return NodeObj;
}

// ============================================================================
// State Machine
// ============================================================================

TSharedPtr<FJsonObject> SerializeStateMachine(UAnimationStateMachineGraph* SMGraph)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!SMGraph)
	{
		Result->SetStringField(TEXT("error"), TEXT("Invalid state machine graph"));
		return Result;
	}

	Result->SetStringField(TEXT("graph_name"), SMGraph->GetName());
	Result->SetNumberField(TEXT("total_nodes"), SMGraph->Nodes.Num());

	// Find entry state
	FString EntryStateName;
	TArray<TSharedPtr<FJsonValue>> StatesArray;
	TArray<TSharedPtr<FJsonValue>> TransitionsArray;
	TArray<TSharedPtr<FJsonValue>> ConduitsArray;

	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		if (UAnimStateEntryNode* EntryNode = Cast<UAnimStateEntryNode>(Node))
		{
			// Follow the entry node's output to find the default state
			for (UEdGraphPin* Pin : EntryNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0)
				{
					UEdGraphNode* DefaultState = Pin->LinkedTo[0]->GetOwningNode();
					if (DefaultState)
					{
						EntryStateName = DefaultState->GetNodeTitle(ENodeTitleType::ListView).ToString();
					}
				}
			}
		}
		else if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
		{
			TSharedPtr<FJsonObject> StateObj = MakeShared<FJsonObject>();
			StateObj->SetStringField(TEXT("name"), StateNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
			StateObj->SetStringField(TEXT("guid"), StateNode->NodeGuid.ToString());

			if (StateNode->BoundGraph)
			{
				StateObj->SetStringField(TEXT("bound_graph_name"), StateNode->BoundGraph->GetName());
				StateObj->SetNumberField(TEXT("bound_graph_node_count"), StateNode->BoundGraph->Nodes.Num());
			}

			StatesArray.Add(MakeShared<FJsonValueObject>(StateObj));
		}
		else if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(Node))
		{
			TSharedPtr<FJsonObject> TransObj = MakeShared<FJsonObject>();

			UAnimStateNodeBase* PrevState = TransNode->GetPreviousState();
			UAnimStateNodeBase* NextState = TransNode->GetNextState();

			TransObj->SetStringField(TEXT("from_state"), PrevState ? PrevState->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("Unknown"));
			TransObj->SetStringField(TEXT("to_state"), NextState ? NextState->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("Unknown"));
			TransObj->SetStringField(TEXT("guid"), TransNode->NodeGuid.ToString());
			TransObj->SetNumberField(TEXT("crossfade_duration"), TransNode->CrossfadeDuration);
			TransObj->SetNumberField(TEXT("priority_order"), TransNode->PriorityOrder);
			TransObj->SetBoolField(TEXT("bidirectional"), TransNode->Bidirectional);

			// Crossfade mode
			FString CrossfadeStr;
			switch (TransNode->BlendMode)
			{
			case EAlphaBlendOption::Linear: CrossfadeStr = TEXT("Linear"); break;
			case EAlphaBlendOption::Cubic: CrossfadeStr = TEXT("Cubic"); break;
			case EAlphaBlendOption::HermiteCubic: CrossfadeStr = TEXT("HermiteCubic"); break;
			case EAlphaBlendOption::Sinusoidal: CrossfadeStr = TEXT("Sinusoidal"); break;
			case EAlphaBlendOption::QuadraticInOut: CrossfadeStr = TEXT("QuadraticInOut"); break;
			case EAlphaBlendOption::CubicInOut: CrossfadeStr = TEXT("CubicInOut"); break;
			case EAlphaBlendOption::QuarticInOut: CrossfadeStr = TEXT("QuarticInOut"); break;
			case EAlphaBlendOption::QuinticInOut: CrossfadeStr = TEXT("QuinticInOut"); break;
			case EAlphaBlendOption::CircularIn: CrossfadeStr = TEXT("CircularIn"); break;
			case EAlphaBlendOption::CircularOut: CrossfadeStr = TEXT("CircularOut"); break;
			case EAlphaBlendOption::CircularInOut: CrossfadeStr = TEXT("CircularInOut"); break;
			case EAlphaBlendOption::ExpIn: CrossfadeStr = TEXT("ExpIn"); break;
			case EAlphaBlendOption::ExpOut: CrossfadeStr = TEXT("ExpOut"); break;
			case EAlphaBlendOption::ExpInOut: CrossfadeStr = TEXT("ExpInOut"); break;
			case EAlphaBlendOption::Custom: CrossfadeStr = TEXT("Custom"); break;
			default: CrossfadeStr = TEXT("Unknown"); break;
			}
			TransObj->SetStringField(TEXT("crossfade_mode"), CrossfadeStr);

			// Blend logic type
			FString LogicTypeStr;
			switch (TransNode->LogicType)
			{
			case ETransitionLogicType::TLT_StandardBlend: LogicTypeStr = TEXT("StandardBlend"); break;
			case ETransitionLogicType::TLT_Inertialization: LogicTypeStr = TEXT("Inertialization"); break;
			case ETransitionLogicType::TLT_Custom: LogicTypeStr = TEXT("Custom"); break;
			default: LogicTypeStr = TEXT("Unknown"); break;
			}
			TransObj->SetStringField(TEXT("logic_type"), LogicTypeStr);

			// Shared transition rules
			if (TransNode->bSharedRules)
			{
				TSharedPtr<FJsonObject> SharedObj = MakeShared<FJsonObject>();
				SharedObj->SetStringField(TEXT("name"), TransNode->SharedRulesName);
				SharedObj->SetStringField(TEXT("guid"), TransNode->SharedRulesGuid.ToString());
				TransObj->SetObjectField(TEXT("shared_rules"), SharedObj);
			}
			if (TransNode->bSharedCrossfade)
			{
				TSharedPtr<FJsonObject> SharedObj = MakeShared<FJsonObject>();
				SharedObj->SetStringField(TEXT("name"), TransNode->SharedCrossfadeName);
				SharedObj->SetStringField(TEXT("guid"), TransNode->SharedCrossfadeGuid.ToString());
				TransObj->SetObjectField(TEXT("shared_crossfade"), SharedObj);
			}

			TransitionsArray.Add(MakeShared<FJsonValueObject>(TransObj));
		}
		else if (UAnimStateConduitNode* ConduitNode = Cast<UAnimStateConduitNode>(Node))
		{
			TSharedPtr<FJsonObject> ConduitObj = MakeShared<FJsonObject>();
			ConduitObj->SetStringField(TEXT("name"), ConduitNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
			ConduitObj->SetStringField(TEXT("guid"), ConduitNode->NodeGuid.ToString());
			if (ConduitNode->BoundGraph)
			{
				ConduitObj->SetStringField(TEXT("bound_graph_name"), ConduitNode->BoundGraph->GetName());
				ConduitObj->SetNumberField(TEXT("bound_graph_node_count"), ConduitNode->BoundGraph->Nodes.Num());
			}
			ConduitsArray.Add(MakeShared<FJsonValueObject>(ConduitObj));
		}
	}

	Result->SetStringField(TEXT("entry_state"), EntryStateName.IsEmpty() ? TEXT("None") : *EntryStateName);
	Result->SetArrayField(TEXT("states"), StatesArray);
	Result->SetNumberField(TEXT("state_count"), StatesArray.Num());
	Result->SetArrayField(TEXT("transitions"), TransitionsArray);
	Result->SetNumberField(TEXT("transition_count"), TransitionsArray.Num());
	Result->SetArrayField(TEXT("conduits"), ConduitsArray);
	Result->SetNumberField(TEXT("conduit_count"), ConduitsArray.Num());

	return Result;
}

TSharedPtr<FJsonObject> SerializeTransition(UAnimStateTransitionNode* TransNode)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!TransNode)
	{
		Result->SetStringField(TEXT("error"), TEXT("Invalid transition node"));
		return Result;
	}

	UAnimStateNodeBase* PrevState = TransNode->GetPreviousState();
	UAnimStateNodeBase* NextState = TransNode->GetNextState();

	Result->SetStringField(TEXT("from_state"), PrevState ? PrevState->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("Unknown"));
	Result->SetStringField(TEXT("to_state"), NextState ? NextState->GetNodeTitle(ENodeTitleType::ListView).ToString() : TEXT("Unknown"));
	Result->SetStringField(TEXT("guid"), TransNode->NodeGuid.ToString());
	Result->SetNumberField(TEXT("crossfade_duration"), TransNode->CrossfadeDuration);
	Result->SetNumberField(TEXT("priority_order"), TransNode->PriorityOrder);
	Result->SetBoolField(TEXT("bidirectional"), TransNode->Bidirectional);

	// Crossfade mode
	FString CrossfadeStr;
	switch (TransNode->BlendMode)
	{
	case EAlphaBlendOption::Linear: CrossfadeStr = TEXT("Linear"); break;
	case EAlphaBlendOption::Cubic: CrossfadeStr = TEXT("Cubic"); break;
	case EAlphaBlendOption::HermiteCubic: CrossfadeStr = TEXT("HermiteCubic"); break;
	case EAlphaBlendOption::Sinusoidal: CrossfadeStr = TEXT("Sinusoidal"); break;
	case EAlphaBlendOption::QuadraticInOut: CrossfadeStr = TEXT("QuadraticInOut"); break;
	case EAlphaBlendOption::CubicInOut: CrossfadeStr = TEXT("CubicInOut"); break;
	case EAlphaBlendOption::QuarticInOut: CrossfadeStr = TEXT("QuarticInOut"); break;
	case EAlphaBlendOption::QuinticInOut: CrossfadeStr = TEXT("QuinticInOut"); break;
	case EAlphaBlendOption::CircularIn: CrossfadeStr = TEXT("CircularIn"); break;
	case EAlphaBlendOption::CircularOut: CrossfadeStr = TEXT("CircularOut"); break;
	case EAlphaBlendOption::CircularInOut: CrossfadeStr = TEXT("CircularInOut"); break;
	case EAlphaBlendOption::ExpIn: CrossfadeStr = TEXT("ExpIn"); break;
	case EAlphaBlendOption::ExpOut: CrossfadeStr = TEXT("ExpOut"); break;
	case EAlphaBlendOption::ExpInOut: CrossfadeStr = TEXT("ExpInOut"); break;
	case EAlphaBlendOption::Custom: CrossfadeStr = TEXT("Custom"); break;
	default: CrossfadeStr = TEXT("Unknown"); break;
	}
	Result->SetStringField(TEXT("crossfade_mode"), CrossfadeStr);

	// Blend logic type
	FString LogicTypeStr;
	switch (TransNode->LogicType)
	{
	case ETransitionLogicType::TLT_StandardBlend: LogicTypeStr = TEXT("StandardBlend"); break;
	case ETransitionLogicType::TLT_Inertialization: LogicTypeStr = TEXT("Inertialization"); break;
	case ETransitionLogicType::TLT_Custom: LogicTypeStr = TEXT("Custom"); break;
	default: LogicTypeStr = TEXT("Unknown"); break;
	}
	Result->SetStringField(TEXT("logic_type"), LogicTypeStr);

	// Blend mode
	FString BlendModeStr;
	switch (TransNode->BlendMode)
	{
	case EAlphaBlendOption::Linear: BlendModeStr = TEXT("Linear"); break;
	case EAlphaBlendOption::Cubic: BlendModeStr = TEXT("Cubic"); break;
	case EAlphaBlendOption::Custom: BlendModeStr = TEXT("Custom"); break;
	default: BlendModeStr = TEXT("Other"); break;
	}
	Result->SetStringField(TEXT("blend_mode"), BlendModeStr);

	// Blend profile
	if (TransNode->BlendProfile)
	{
		Result->SetStringField(TEXT("blend_profile"), TransNode->BlendProfile->GetName());
	}

	// Custom blend curve
	if (TransNode->CustomBlendCurve)
	{
		Result->SetStringField(TEXT("custom_blend_curve"), TransNode->CustomBlendCurve->GetPathName());
	}

	// Shared transition rules
	if (TransNode->bSharedRules)
	{
		TSharedPtr<FJsonObject> SharedObj = MakeShared<FJsonObject>();
		SharedObj->SetStringField(TEXT("name"), TransNode->SharedRulesName);
		SharedObj->SetStringField(TEXT("guid"), TransNode->SharedRulesGuid.ToString());
		Result->SetObjectField(TEXT("shared_rules"), SharedObj);
	}
	if (TransNode->bSharedCrossfade)
	{
		TSharedPtr<FJsonObject> SharedObj = MakeShared<FJsonObject>();
		SharedObj->SetStringField(TEXT("name"), TransNode->SharedCrossfadeName);
		SharedObj->SetStringField(TEXT("guid"), TransNode->SharedCrossfadeGuid.ToString());
		Result->SetObjectField(TEXT("shared_crossfade"), SharedObj);
	}

	// Condition graph
	if (UEdGraph* CondGraph = TransNode->BoundGraph)
	{
		TSharedPtr<FJsonObject> CondGraphObj = MakeShared<FJsonObject>();
		CondGraphObj->SetStringField(TEXT("graph_name"), CondGraph->GetName());
		CondGraphObj->SetNumberField(TEXT("node_count"), CondGraph->Nodes.Num());

		TArray<TSharedPtr<FJsonValue>> CondNodesArray;
		for (UEdGraphNode* CondNode : CondGraph->Nodes)
		{
			if (CondNode)
			{
				CondNodesArray.Add(MakeShared<FJsonValueObject>(SerializeAnimGraphNode(CondNode, TEXT("full"))));
			}
		}
		CondGraphObj->SetArrayField(TEXT("nodes"), CondNodesArray);
		Result->SetObjectField(TEXT("condition_graph"), CondGraphObj);
	}

	// Custom transition graph
	if (UEdGraph* CustomGraph = TransNode->CustomTransitionGraph)
	{
		TSharedPtr<FJsonObject> CustomGraphObj = MakeShared<FJsonObject>();
		CustomGraphObj->SetStringField(TEXT("graph_name"), CustomGraph->GetName());
		CustomGraphObj->SetNumberField(TEXT("node_count"), CustomGraph->Nodes.Num());

		TArray<TSharedPtr<FJsonValue>> CustomNodesArray;
		for (UEdGraphNode* CustomNode : CustomGraph->Nodes)
		{
			if (CustomNode)
			{
				CustomNodesArray.Add(MakeShared<FJsonValueObject>(SerializeAnimGraphNode(CustomNode, TEXT("full"))));
			}
		}
		CustomGraphObj->SetArrayField(TEXT("nodes"), CustomNodesArray);
		Result->SetObjectField(TEXT("custom_blend_graph"), CustomGraphObj);
	}

	return Result;
}

// ============================================================================
// Class Settings & Blueprint Metadata
// ============================================================================

TSharedPtr<FJsonObject> SerializeClassSettings(UAnimBlueprint* AnimBP)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!AnimBP)
	{
		return Result;
	}

	// Parent class
	if (AnimBP->ParentClass)
	{
		TSharedPtr<FJsonObject> ParentObj = MakeShared<FJsonObject>();
		ParentObj->SetStringField(TEXT("name"), AnimBP->ParentClass->GetName());
		ParentObj->SetStringField(TEXT("path"), AnimBP->ParentClass->GetPathName());
		Result->SetObjectField(TEXT("parent_class"), ParentObj);
	}

	// Target skeleton
	if (AnimBP->TargetSkeleton)
	{
		Result->SetStringField(TEXT("skeleton_path"), AnimBP->TargetSkeleton->GetPathName());
		Result->SetStringField(TEXT("skeleton_name"), AnimBP->TargetSkeleton->GetName());
	}

	// Preview skeletal mesh
	USkeletalMesh* PreviewMesh = AnimBP->GetPreviewMesh();
	if (PreviewMesh)
	{
		Result->SetStringField(TEXT("preview_mesh_path"), PreviewMesh->GetPathName());
	}

	// Implemented interfaces
	TArray<TSharedPtr<FJsonValue>> InterfacesArray;
	for (const FBPInterfaceDescription& Interface : AnimBP->ImplementedInterfaces)
	{
		if (Interface.Interface)
		{
			TSharedPtr<FJsonObject> IntObj = MakeShared<FJsonObject>();
			IntObj->SetStringField(TEXT("name"), Interface.Interface->GetName());
			IntObj->SetStringField(TEXT("path"), Interface.Interface->GetPathName());

			// Count function graphs from this interface
			int32 FunctionCount = Interface.Graphs.Num();
			IntObj->SetNumberField(TEXT("function_count"), FunctionCount);

			InterfacesArray.Add(MakeShared<FJsonValueObject>(IntObj));
		}
	}
	Result->SetArrayField(TEXT("interfaces"), InterfacesArray);

	// Blueprint flags
	Result->SetBoolField(TEXT("use_multi_threaded_animation_update"), AnimBP->bUseMultiThreadedAnimationUpdate);

	return Result;
}

TArray<TSharedPtr<FJsonValue>> SerializeVariables(UAnimBlueprint* AnimBP)
{
	TArray<TSharedPtr<FJsonValue>> VarsArray;
	if (!AnimBP)
	{
		return VarsArray;
	}

	for (const FBPVariableDescription& Var : AnimBP->NewVariables)
	{
		TSharedPtr<FJsonObject> VarObj = MakeShared<FJsonObject>();
		VarObj->SetStringField(TEXT("name"), Var.VarName.ToString());

		// Type
		FString TypeStr;
		if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
		{
			TypeStr = TEXT("bool");
		}
		else if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_Float || Var.VarType.PinCategory == UEdGraphSchema_K2::PC_Real || Var.VarType.PinCategory == UEdGraphSchema_K2::PC_Double)
		{
			TypeStr = TEXT("float");
		}
		else if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_Int)
		{
			TypeStr = TEXT("int");
		}
		else if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_Name)
		{
			TypeStr = TEXT("FName");
		}
		else if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_String)
		{
			TypeStr = TEXT("FString");
		}
		else if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_Text)
		{
			TypeStr = TEXT("FText");
		}
		else if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_Struct)
		{
			UScriptStruct* Struct = Cast<UScriptStruct>(Var.VarType.PinSubCategoryObject.Get());
			TypeStr = Struct ? FString::Printf(TEXT("struct<%s>"), *Struct->GetName()) : TEXT("struct");
		}
		else if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_Object || Var.VarType.PinCategory == UEdGraphSchema_K2::PC_SoftObject)
		{
			UClass* ObjClass = Cast<UClass>(Var.VarType.PinSubCategoryObject.Get());
			TypeStr = ObjClass ? FString::Printf(TEXT("object<%s>"), *ObjClass->GetName()) : TEXT("object");
		}
		else if (Var.VarType.PinCategory == UEdGraphSchema_K2::PC_Enum || Var.VarType.PinCategory == UEdGraphSchema_K2::PC_Byte)
		{
			UEnum* Enum = Cast<UEnum>(Var.VarType.PinSubCategoryObject.Get());
			TypeStr = Enum ? FString::Printf(TEXT("enum<%s>"), *Enum->GetName()) : Var.VarType.PinCategory.ToString();
		}
		else
		{
			TypeStr = Var.VarType.PinCategory.ToString();
		}

		// Container type
		if (Var.VarType.IsArray())
		{
			TypeStr = FString::Printf(TEXT("TArray<%s>"), *TypeStr);
		}
		else if (Var.VarType.IsSet())
		{
			TypeStr = FString::Printf(TEXT("TSet<%s>"), *TypeStr);
		}
		else if (Var.VarType.IsMap())
		{
			TypeStr = FString::Printf(TEXT("TMap<%s, ...>"), *TypeStr);
		}

		VarObj->SetStringField(TEXT("type"), TypeStr);
		VarObj->SetStringField(TEXT("category"), Var.Category.ToString());

		// Flags
		VarObj->SetBoolField(TEXT("is_instance_editable"),
			Var.PropertyFlags & CPF_Edit ? true : false);
		VarObj->SetBoolField(TEXT("is_blueprint_read_only"),
			(Var.PropertyFlags & CPF_DisableEditOnInstance) != 0);

		// Default value
		if (!Var.DefaultValue.IsEmpty())
		{
			VarObj->SetStringField(TEXT("default_value"), Var.DefaultValue);
		}

		VarsArray.Add(MakeShared<FJsonValueObject>(VarObj));
	}

	return VarsArray;
}

TArray<TSharedPtr<FJsonValue>> SerializeFunctions(UAnimBlueprint* AnimBP)
{
	TArray<TSharedPtr<FJsonValue>> FuncsArray;
	if (!AnimBP)
	{
		return FuncsArray;
	}

	// Get the generated class for checking compiled function metadata
	UAnimBlueprintGeneratedClass* GeneratedClass = Cast<UAnimBlueprintGeneratedClass>(AnimBP->GeneratedClass);

	for (UEdGraph* FuncGraph : AnimBP->FunctionGraphs)
	{
		if (!FuncGraph)
		{
			continue;
		}

		// Skip animation graphs (they're listed separately as graphs)
		if (Cast<UAnimationGraph>(FuncGraph))
		{
			continue;
		}

		TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
		FuncObj->SetStringField(TEXT("name"), FuncGraph->GetName());

		// Try to find the corresponding UFunction for flag inspection
		UFunction* CompiledFunc = nullptr;
		if (GeneratedClass)
		{
			CompiledFunc = GeneratedClass->FindFunctionByName(FName(*FuncGraph->GetName()));
		}

		if (CompiledFunc)
		{
			FuncObj->SetBoolField(TEXT("is_pure"), CompiledFunc->HasAnyFunctionFlags(FUNC_BlueprintPure));
			FuncObj->SetBoolField(TEXT("is_const"), CompiledFunc->HasAnyFunctionFlags(FUNC_Const));
			FuncObj->SetBoolField(TEXT("is_static"), CompiledFunc->HasAnyFunctionFlags(FUNC_Static));
			FuncObj->SetBoolField(TEXT("is_thread_safe"),
				CompiledFunc->HasMetaData(TEXT("BlueprintThreadSafe")) ||
				CompiledFunc->HasMetaData(TEXT("NotBlueprintThreadSafe")) == false);

			// Check for override
			UFunction* SuperFunc = CompiledFunc->GetSuperFunction();
			FuncObj->SetBoolField(TEXT("is_override"), SuperFunc != nullptr);

			// Parameters
			TArray<TSharedPtr<FJsonValue>> ParamsArray;
			FString ReturnType;
			for (TFieldIterator<FProperty> PropIt(CompiledFunc); PropIt; ++PropIt)
			{
				FProperty* Prop = *PropIt;
				if (Prop->HasAnyPropertyFlags(CPF_ReturnParm))
				{
					ReturnType = Prop->GetCPPType();
				}
				else if (Prop->HasAnyPropertyFlags(CPF_Parm))
				{
					TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
					ParamObj->SetStringField(TEXT("name"), Prop->GetName());
					ParamObj->SetStringField(TEXT("type"), Prop->GetCPPType());
					ParamObj->SetBoolField(TEXT("is_output"), Prop->HasAnyPropertyFlags(CPF_OutParm) && !Prop->HasAnyPropertyFlags(CPF_ReferenceParm));
					ParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
				}
			}
			FuncObj->SetArrayField(TEXT("parameters"), ParamsArray);
			if (!ReturnType.IsEmpty())
			{
				FuncObj->SetStringField(TEXT("return_type"), ReturnType);
			}
		}
		else
		{
			// Fall back to graph inspection for function entry node
			for (UEdGraphNode* Node : FuncGraph->Nodes)
			{
				if (UK2Node_FunctionEntry* EntryNode = Cast<UK2Node_FunctionEntry>(Node))
				{
					// Get basic info from entry node pins
					TArray<TSharedPtr<FJsonValue>> ParamsArray;
					for (UEdGraphPin* Pin : EntryNode->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Output &&
							Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
						{
							TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
							ParamObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
							ParamObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
							ParamsArray.Add(MakeShared<FJsonValueObject>(ParamObj));
						}
					}
					FuncObj->SetArrayField(TEXT("parameters"), ParamsArray);
					break;
				}
			}

			FuncObj->SetBoolField(TEXT("is_compiled"), false);
		}

		FuncsArray.Add(MakeShared<FJsonValueObject>(FuncObj));
	}

	return FuncsArray;
}

// ============================================================================
// Analysis & Warnings
// ============================================================================

TSharedPtr<FJsonObject> AnalyzeThreadSafety(UAnimBlueprint* AnimBP)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!AnimBP)
	{
		return Result;
	}

	UAnimBlueprintGeneratedClass* GeneratedClass = Cast<UAnimBlueprintGeneratedClass>(AnimBP->GeneratedClass);
	if (!GeneratedClass)
	{
		Result->SetStringField(TEXT("warning"), TEXT("Blueprint has no generated class - may need compilation"));
		return Result;
	}

	TArray<TSharedPtr<FJsonValue>> SafeFuncs;
	TArray<TSharedPtr<FJsonValue>> UnsafeFuncs;
	TArray<TSharedPtr<FJsonValue>> TSWarnings;

	for (UEdGraph* FuncGraph : AnimBP->FunctionGraphs)
	{
		if (!FuncGraph || Cast<UAnimationGraph>(FuncGraph))
		{
			continue;
		}

		UFunction* CompiledFunc = GeneratedClass->FindFunctionByName(FName(*FuncGraph->GetName()));
		if (!CompiledFunc)
		{
			continue;
		}

		const bool bIsThreadSafe = CompiledFunc->HasMetaData(TEXT("BlueprintThreadSafe"));
		const bool bIsNotThreadSafe = CompiledFunc->HasMetaData(TEXT("NotBlueprintThreadSafe"));

		TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
		FuncObj->SetStringField(TEXT("name"), FuncGraph->GetName());
		FuncObj->SetBoolField(TEXT("is_thread_safe"), bIsThreadSafe);
		FuncObj->SetBoolField(TEXT("is_explicitly_not_thread_safe"), bIsNotThreadSafe);

		if (bIsThreadSafe)
		{
			SafeFuncs.Add(MakeShared<FJsonValueObject>(FuncObj));
		}
		else
		{
			UnsafeFuncs.Add(MakeShared<FJsonValueObject>(FuncObj));

			// Generate warning if this function is likely called from anim graph
			if (!bIsNotThreadSafe)
			{
				TSWarnings.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("Function '%s' is not marked BlueprintThreadSafe - may cause issues if called from animation graph"), *FuncGraph->GetName())));
			}
		}
	}

	Result->SetArrayField(TEXT("thread_safe_functions"), SafeFuncs);
	Result->SetArrayField(TEXT("non_thread_safe_functions"), UnsafeFuncs);
	Result->SetArrayField(TEXT("warnings"), TSWarnings);
	Result->SetNumberField(TEXT("thread_safe_count"), SafeFuncs.Num());
	Result->SetNumberField(TEXT("non_thread_safe_count"), UnsafeFuncs.Num());

	return Result;
}

TSharedPtr<FJsonObject> CollectWarnings(UAnimBlueprint* AnimBP)
{
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	if (!AnimBP)
	{
		return Result;
	}

	TArray<TSharedPtr<FJsonValue>> FastPathWarnings;
	TArray<TSharedPtr<FJsonValue>> ThreadSafetyWarnings;
	TArray<TSharedPtr<FJsonValue>> CompilerWarnings;

	// Primary: Collect compiler messages from ALL nodes across ALL graphs.
	// The engine's blueprint compiler already computes fast-path compliance for
	// all cases (property bindings, pin-connected BP logic, transitions, etc.)
	// and stores results as bHasCompilerMessage + ErrorType + ErrorMsg on each node.
	TArray<FAnimGraphInfo> AllGraphs = CollectAllGraphs(AnimBP);
	for (const FAnimGraphInfo& GraphInfo : AllGraphs)
	{
		if (!GraphInfo.Graph)
		{
			continue;
		}

		for (UEdGraphNode* Node : GraphInfo.Graph->Nodes)
		{
			if (!Node || !Node->bHasCompilerMessage || Node->ErrorMsg.IsEmpty())
			{
				continue;
			}

			TSharedPtr<FJsonObject> WarningObj = MakeShared<FJsonObject>();
			WarningObj->SetStringField(TEXT("graph"), GraphInfo.Name);
			WarningObj->SetStringField(TEXT("node"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			WarningObj->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
			WarningObj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
			WarningObj->SetNumberField(TEXT("error_type"), Node->ErrorType);

			// Deduplicate repeated lines in ErrorMsg (engine sometimes repeats per-pin)
			TArray<FString> UniqueMessages;
			TArray<FString> Lines;
			Node->ErrorMsg.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				FString Trimmed = Line.TrimStartAndEnd();
				if (!Trimmed.IsEmpty())
				{
					UniqueMessages.AddUnique(Trimmed);
				}
			}

			FString DeduplicatedMsg = FString::Join(UniqueMessages, TEXT("\n"));
			WarningObj->SetStringField(TEXT("message"), DeduplicatedMsg);

			// Categorize: fast-path warnings vs other compiler messages
			if (DeduplicatedMsg.Contains(TEXT("uses Blueprint to update its values")))
			{
				FastPathWarnings.Add(MakeShared<FJsonValueObject>(WarningObj));
			}
			else
			{
				CompilerWarnings.Add(MakeShared<FJsonValueObject>(WarningObj));
			}
		}
	}

	// Thread safety warnings
	TSharedPtr<FJsonObject> TSAnalysis = AnalyzeThreadSafety(AnimBP);
	if (TSAnalysis)
	{
		const TArray<TSharedPtr<FJsonValue>>* TSWarningsPtr = nullptr;
		if (TSAnalysis->TryGetArrayField(TEXT("warnings"), TSWarningsPtr) && TSWarningsPtr)
		{
			ThreadSafetyWarnings = *TSWarningsPtr;
		}
	}

	// Blueprint compile status
	if (AnimBP->Status == BS_Error || AnimBP->Status == BS_UpToDateWithWarnings)
	{
		FString StatusStr = AnimBP->Status == BS_Error ? TEXT("Error") : TEXT("UpToDateWithWarnings");
		CompilerWarnings.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("Blueprint compile status: %s"), *StatusStr)));
	}

	Result->SetArrayField(TEXT("fast_path_warnings"), FastPathWarnings);
	Result->SetNumberField(TEXT("fast_path_warning_count"), FastPathWarnings.Num());
	Result->SetArrayField(TEXT("thread_safety_warnings"), ThreadSafetyWarnings);
	Result->SetNumberField(TEXT("thread_safety_warning_count"), ThreadSafetyWarnings.Num());
	Result->SetArrayField(TEXT("compiler_warnings"), CompilerWarnings);
	Result->SetNumberField(TEXT("compiler_warning_count"), CompilerWarnings.Num());
	Result->SetNumberField(TEXT("total_warning_count"), FastPathWarnings.Num() + ThreadSafetyWarnings.Num() + CompilerWarnings.Num());

	return Result;
}

} // namespace ClaireonAnimGraphHelpers
