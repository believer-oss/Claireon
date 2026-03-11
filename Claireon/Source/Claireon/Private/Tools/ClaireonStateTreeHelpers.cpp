// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeHelpers.h"
#include "ClaireonLog.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeEditorNode.h"
#include "StateTreeSchema.h"
#include "StateTreeNodeBase.h"
#include "StateTreePropertyBindings.h"
#include "StateTreeEditorPropertyBindings.h"
#include "InstancedStruct.h"
#include "GameplayTagContainer.h"

// ============================================================================
// Asset Loading
// ============================================================================

UStateTree* ClaireonStateTreeHelpers::LoadStateTreeAsset(const FString& AssetPath, FString& OutError)
{
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("Asset path is empty");
		return nullptr;
	}

	FSoftObjectPath SoftPath(AssetPath);
	UObject* LoadedObj = SoftPath.TryLoad();
	if (!LoadedObj)
	{
		OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *AssetPath);
		return nullptr;
	}

	UStateTree* StateTree = Cast<UStateTree>(LoadedObj);
	if (!StateTree)
	{
		OutError = FString::Printf(TEXT("Asset at %s is not a State Tree (actual type: %s)"), *AssetPath, *LoadedObj->GetClass()->GetName());
		return nullptr;
	}

	return StateTree;
}

UStateTreeEditorData* ClaireonStateTreeHelpers::GetEditorData(UStateTree* StateTree, FString& OutError)
{
	if (!StateTree)
	{
		OutError = TEXT("StateTree is null");
		return nullptr;
	}

#if WITH_EDITORONLY_DATA
	UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(StateTree->EditorData);
	if (!EditorData)
	{
		OutError = TEXT("State Tree has no editor data (EditorData is null or wrong type)");
		return nullptr;
	}
	return EditorData;
#else
	OutError = TEXT("Editor data is not available in non-editor builds");
	return nullptr;
#endif
}

// ============================================================================
// Node/State Lookup
// ============================================================================

namespace
{
	UStateTreeState* FindStateByIdRecursive(const TArray<TObjectPtr<UStateTreeState>>& States, const FGuid& StateId)
	{
		for (UStateTreeState* State : States)
		{
			if (!State)
			{
				continue;
			}
			if (State->ID == StateId)
			{
				return State;
			}
			UStateTreeState* Found = FindStateByIdRecursive(State->Children, StateId);
			if (Found)
			{
				return Found;
			}
		}
		return nullptr;
	}

	FStateTreeEditorNode* FindNodeInArray(TArray<FStateTreeEditorNode>& Nodes, const FGuid& NodeId)
	{
		for (FStateTreeEditorNode& Node : Nodes)
		{
			if (Node.ID == NodeId)
			{
				return &Node;
			}
		}
		return nullptr;
	}

	FStateTreeEditorNode* FindNodeInStateRecursive(UStateTreeState* State, const FGuid& NodeId)
	{
		if (!State)
		{
			return nullptr;
		}

		// Search tasks
		FStateTreeEditorNode* Found = FindNodeInArray(State->Tasks, NodeId);
		if (Found)
			return Found;

		// Search single task
		if (State->SingleTask.ID == NodeId)
		{
			return &State->SingleTask;
		}

		// Search enter conditions
		Found = FindNodeInArray(State->EnterConditions, NodeId);
		if (Found)
			return Found;

		// Search considerations
		Found = FindNodeInArray(State->Considerations, NodeId);
		if (Found)
			return Found;

		// Search transition conditions
		for (FStateTreeTransition& Transition : State->Transitions)
		{
			Found = FindNodeInArray(Transition.Conditions, NodeId);
			if (Found)
				return Found;
		}

		// Recurse into children
		for (UStateTreeState* Child : State->Children)
		{
			Found = FindNodeInStateRecursive(Child, NodeId);
			if (Found)
				return Found;
		}

		return nullptr;
	}
} // namespace

UStateTreeState* ClaireonStateTreeHelpers::FindStateById(UStateTreeEditorData* EditorData, const FGuid& StateId)
{
	if (!EditorData || !StateId.IsValid())
	{
		return nullptr;
	}
	return FindStateByIdRecursive(EditorData->SubTrees, StateId);
}

FStateTreeEditorNode* ClaireonStateTreeHelpers::FindNodeById(UStateTreeEditorData* EditorData, const FGuid& NodeId)
{
	if (!EditorData || !NodeId.IsValid())
	{
		return nullptr;
	}

	// Search global evaluators
	FStateTreeEditorNode* Found = FindNodeInArray(EditorData->Evaluators, NodeId);
	if (Found)
		return Found;

	// Search global tasks
	Found = FindNodeInArray(EditorData->GlobalTasks, NodeId);
	if (Found)
		return Found;

	// Search all states recursively
	for (UStateTreeState* SubTree : EditorData->SubTrees)
	{
		Found = FindNodeInStateRecursive(SubTree, NodeId);
		if (Found)
			return Found;
	}

	return nullptr;
}

FStateTreeTransition* ClaireonStateTreeHelpers::FindTransitionById(UStateTreeState* State, const FGuid& TransitionId)
{
	if (!State || !TransitionId.IsValid())
	{
		return nullptr;
	}
	for (FStateTreeTransition& Transition : State->Transitions)
	{
		if (Transition.ID == TransitionId)
		{
			return &Transition;
		}
	}
	return nullptr;
}

// ============================================================================
// Formatting Helpers
// ============================================================================

namespace
{
	FString FormatPropertyValue(const FProperty* Prop, const void* ValuePtr)
	{
		if (!Prop || !ValuePtr)
		{
			return TEXT("?");
		}

		FString ValueStr;
		Prop->ExportText_Direct(ValueStr, ValuePtr, ValuePtr, nullptr, PPF_None);

		// Truncate long values
		if (ValueStr.Len() > 80)
		{
			ValueStr = ValueStr.Left(77) + TEXT("...");
		}

		return ValueStr;
	}

	FString FormatStructProperties(const UScriptStruct* Struct, const void* StructData, int32 MaxProps = 10)
	{
		if (!Struct || !StructData)
		{
			return TEXT("");
		}

		FString Result;
		int32 Count = 0;

		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			if (Count >= MaxProps)
			{
				Result += TEXT(", ...");
				break;
			}

			const FProperty* Prop = *It;
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(StructData);
			FString ValueStr = FormatPropertyValue(Prop, ValuePtr);

			if (!Result.IsEmpty())
			{
				Result += TEXT(", ");
			}
			Result += FString::Printf(TEXT("%s=%s"), *Prop->GetName(), *ValueStr);
			Count++;
		}

		return Result;
	}

	FString GetStateTypeName(EStateTreeStateType Type)
	{
		switch (Type)
		{
			case EStateTreeStateType::State:
				return TEXT("State");
			case EStateTreeStateType::Group:
				return TEXT("Group");
			case EStateTreeStateType::Linked:
				return TEXT("Linked");
			case EStateTreeStateType::LinkedAsset:
				return TEXT("LinkedAsset");
			case EStateTreeStateType::Subtree:
				return TEXT("Subtree");
			default:
				return TEXT("Unknown");
		}
	}

	FString GetSelectionBehaviorName(EStateTreeStateSelectionBehavior Behavior)
	{
		switch (Behavior)
		{
			case EStateTreeStateSelectionBehavior::None:
				return TEXT("None");
			case EStateTreeStateSelectionBehavior::TryEnterState:
				return TEXT("TryEnterState");
			case EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder:
				return TEXT("InOrder");
			case EStateTreeStateSelectionBehavior::TrySelectChildrenAtRandom:
				return TEXT("Random");
			case EStateTreeStateSelectionBehavior::TrySelectChildrenWithHighestUtility:
				return TEXT("HighestUtility");
			case EStateTreeStateSelectionBehavior::TrySelectChildrenAtRandomWeightedByUtility:
				return TEXT("WeightedUtility");
			case EStateTreeStateSelectionBehavior::TryFollowTransitions:
				return TEXT("FollowTransitions");
			default:
				return TEXT("Unknown");
		}
	}

	FString GetTriggerName(EStateTreeTransitionTrigger Trigger)
	{
		if (EnumHasAllFlags(Trigger, EStateTreeTransitionTrigger::OnStateCompleted))
		{
			return TEXT("OnStateCompleted");
		}
		if (EnumHasAllFlags(Trigger, EStateTreeTransitionTrigger::OnStateSucceeded))
		{
			return TEXT("OnStateSucceeded");
		}
		if (EnumHasAllFlags(Trigger, EStateTreeTransitionTrigger::OnStateFailed))
		{
			return TEXT("OnStateFailed");
		}
		if (EnumHasAllFlags(Trigger, EStateTreeTransitionTrigger::OnTick))
		{
			return TEXT("OnTick");
		}
		if (EnumHasAllFlags(Trigger, EStateTreeTransitionTrigger::OnEvent))
		{
			return TEXT("OnEvent");
		}
		return TEXT("None");
	}

	FString GetTransitionTypeName(EStateTreeTransitionType Type)
	{
		switch (Type)
		{
			case EStateTreeTransitionType::GotoState:
				return TEXT("GotoState");
			case EStateTreeTransitionType::NextState:
				return TEXT("NextState");
			case EStateTreeTransitionType::NextSelectableState:
				return TEXT("NextSelectableState");
			case EStateTreeTransitionType::Succeeded:
				return TEXT("Succeeded");
			case EStateTreeTransitionType::Failed:
				return TEXT("Failed");
			case EStateTreeTransitionType::None:
				return TEXT("None");
			default:
				return TEXT("Unknown");
		}
	}

	FString GetPriorityName(EStateTreeTransitionPriority Priority)
	{
		switch (Priority)
		{
			case EStateTreeTransitionPriority::None:
				return TEXT("None");
			case EStateTreeTransitionPriority::Low:
				return TEXT("Low");
			case EStateTreeTransitionPriority::Normal:
				return TEXT("Normal");
			case EStateTreeTransitionPriority::Medium:
				return TEXT("Medium");
			case EStateTreeTransitionPriority::High:
				return TEXT("High");
			case EStateTreeTransitionPriority::Critical:
				return TEXT("Critical");
			default:
				return TEXT("Normal");
		}
	}

	void FormatStateRecursive(UStateTreeState* State, FString& Output, const FString& Indent, bool bIsLast, const FGuid* FocusStateId)
	{
		if (!State)
		{
			return;
		}

		FString Connector = bIsLast ? TEXT("\u2514\u2500 ") : TEXT("\u251C\u2500 ");
		FString ChildIndent = Indent + (bIsLast ? TEXT("   ") : TEXT("\u2502  "));

		// State header
		FString FocusMarker;
		if (FocusStateId && State->ID == *FocusStateId)
		{
			FocusMarker = TEXT(" <<<CURSOR>>>");
		}

		FString EnabledStr = State->bEnabled ? TEXT("") : TEXT(" [DISABLED]");

		Output += FString::Printf(TEXT("%s%s[%s] %s (%s, %s)%s%s\n"),
			*Indent, *Connector,
			*State->ID.ToString(EGuidFormats::DigitsWithHyphensLower),
			*State->Name.ToString(),
			*GetStateTypeName(State->Type),
			*GetSelectionBehaviorName(State->SelectionBehavior),
			*EnabledStr, *FocusMarker);

		// Enter conditions
		if (State->EnterConditions.Num() > 0)
		{
			Output += FString::Printf(TEXT("%sEnterConditions:\n"), *ChildIndent);
			for (const FStateTreeEditorNode& CondNode : State->EnterConditions)
			{
				Output += FString::Printf(TEXT("%s  %s\n"), *ChildIndent, *ClaireonStateTreeHelpers::FormatEditorNode(CondNode));
			}
		}

		// Tasks
		if (State->Tasks.Num() > 0)
		{
			Output += FString::Printf(TEXT("%sTasks:\n"), *ChildIndent);
			for (const FStateTreeEditorNode& TaskNode : State->Tasks)
			{
				Output += FString::Printf(TEXT("%s  %s\n"), *ChildIndent, *ClaireonStateTreeHelpers::FormatEditorNode(TaskNode));
			}
		}
		else if (State->SingleTask.Node.IsValid())
		{
			Output += FString::Printf(TEXT("%sTask: %s\n"), *ChildIndent, *ClaireonStateTreeHelpers::FormatEditorNode(State->SingleTask));
		}

		// Considerations
		if (State->Considerations.Num() > 0)
		{
			Output += FString::Printf(TEXT("%sConsiderations:\n"), *ChildIndent);
			for (const FStateTreeEditorNode& ConsNode : State->Considerations)
			{
				Output += FString::Printf(TEXT("%s  %s\n"), *ChildIndent, *ClaireonStateTreeHelpers::FormatEditorNode(ConsNode));
			}
		}

		// Transitions
		if (State->Transitions.Num() > 0)
		{
			Output += FString::Printf(TEXT("%sTransitions:\n"), *ChildIndent);
			for (const FStateTreeTransition& Trans : State->Transitions)
			{
				FString TriggerStr = GetTriggerName(Trans.Trigger);

#if WITH_EDITORONLY_DATA
				// Add event tag info if it's an event trigger
				if (EnumHasAllFlags(Trans.Trigger, EStateTreeTransitionTrigger::OnEvent) && Trans.RequiredEvent.Tag.IsValid())
				{
					TriggerStr += FString::Printf(TEXT("[%s]"), *Trans.RequiredEvent.Tag.ToString());
				}

				FString TargetStr;
				if (Trans.State.LinkType == EStateTreeTransitionType::GotoState)
				{
					TargetStr = Trans.State.Name.IsNone() ? Trans.State.ID.ToString(EGuidFormats::DigitsWithHyphensLower) : Trans.State.Name.ToString();
				}
				else
				{
					TargetStr = GetTransitionTypeName(Trans.State.LinkType);
				}
#else
				FString TargetStr = TEXT("(runtime only)");
#endif

				FString PriorityStr;
				if (Trans.Priority != EStateTreeTransitionPriority::Normal)
				{
					PriorityStr = FString::Printf(TEXT(" (Priority: %s)"), *GetPriorityName(Trans.Priority));
				}

				FString EnabledTransStr = Trans.bTransitionEnabled ? TEXT("") : TEXT(" [DISABLED]");

				Output += FString::Printf(TEXT("%s  [%s] \u2192 %s \u2192 %s%s%s\n"),
					*ChildIndent,
					*Trans.ID.ToString(EGuidFormats::DigitsWithHyphensLower),
					*TriggerStr, *TargetStr, *PriorityStr, *EnabledTransStr);

				// Transition conditions
				for (const FStateTreeEditorNode& CondNode : Trans.Conditions)
				{
					Output += FString::Printf(TEXT("%s    Condition: %s\n"), *ChildIndent, *ClaireonStateTreeHelpers::FormatEditorNode(CondNode));
				}
			}
		}

		// Children
		for (int32 i = 0; i < State->Children.Num(); i++)
		{
			bool bChildIsLast = (i == State->Children.Num() - 1);
			FormatStateRecursive(State->Children[i], Output, ChildIndent, bChildIsLast, FocusStateId);
		}
	}
} // namespace

FString ClaireonStateTreeHelpers::FormatEditorNode(const FStateTreeEditorNode& Node)
{
	if (!Node.Node.IsValid())
	{
		return TEXT("[invalid node]");
	}

	const UScriptStruct* NodeStruct = Node.Node.GetScriptStruct();
	FString StructName = NodeStruct ? NodeStruct->GetName() : TEXT("Unknown");
	FString GuidStr = Node.ID.ToString(EGuidFormats::DigitsWithHyphensLower);

	// Format node struct properties
	FString PropsStr;
	if (NodeStruct)
	{
		PropsStr = FormatStructProperties(NodeStruct, Node.Node.GetMemory());
	}

	// Format instance data properties
	FString InstanceStr;
	if (Node.Instance.IsValid())
	{
		const UScriptStruct* InstanceStruct = Node.Instance.GetScriptStruct();
		if (InstanceStruct)
		{
			FString InstanceProps = FormatStructProperties(InstanceStruct, Node.Instance.GetMemory());
			if (!InstanceProps.IsEmpty())
			{
				InstanceStr = FString::Printf(TEXT(" Instance(%s)"), *InstanceProps);
			}
		}
	}
	else if (Node.InstanceObject)
	{
		FString ObjProps;
		int32 Count = 0;
		for (TFieldIterator<FProperty> It(Node.InstanceObject->GetClass()); It; ++It)
		{
			if (Count >= 10)
			{
				ObjProps += TEXT(", ...");
				break;
			}
			const FProperty* Prop = *It;
			// Skip internal UObject properties
			if (Prop->GetOwnerClass() == UObject::StaticClass())
				continue;
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Node.InstanceObject);
			FString ValueStr = FormatPropertyValue(Prop, ValuePtr);
			if (!ObjProps.IsEmpty())
				ObjProps += TEXT(", ");
			ObjProps += FString::Printf(TEXT("%s=%s"), *Prop->GetName(), *ValueStr);
			Count++;
		}
		if (!ObjProps.IsEmpty())
		{
			InstanceStr = FString::Printf(TEXT(" Instance(%s)"), *ObjProps);
		}
	}

	FString Result = FString::Printf(TEXT("[%s] %s"), *GuidStr, *StructName);
	if (!PropsStr.IsEmpty())
	{
		Result += FString::Printf(TEXT(" (%s)"), *PropsStr);
	}
	if (!InstanceStr.IsEmpty())
	{
		Result += InstanceStr;
	}

	return Result;
}

FString ClaireonStateTreeHelpers::FormatStateArea(UStateTreeState* State)
{
	if (!State)
	{
		return TEXT("(null state)");
	}

	FString Output;
	FormatStateRecursive(State, Output, TEXT(""), true, nullptr);
	return Output;
}

FString ClaireonStateTreeHelpers::FormatStateTreeStructure(UStateTreeEditorData* EditorData, const FGuid* FocusStateId)
{
	if (!EditorData)
	{
		return TEXT("(null editor data)");
	}

	UStateTree* StateTree = Cast<UStateTree>(EditorData->GetOuter());
	FString Output;

	// Header
	FString AssetName = StateTree ? StateTree->GetName() : TEXT("Unknown");
	Output += FString::Printf(TEXT("=== State Tree: %s ===\n"), *AssetName);

	if (StateTree)
	{
		const UStateTreeSchema* Schema = StateTree->GetSchema();
		if (Schema)
		{
			Output += FString::Printf(TEXT("Schema: %s\n"), *Schema->GetClass()->GetName());
		}
	}

	Output += TEXT("\n");

	// Global Evaluators
	Output += TEXT("=== Global Evaluators ===\n");
	if (EditorData->Evaluators.Num() > 0)
	{
		for (const FStateTreeEditorNode& EvalNode : EditorData->Evaluators)
		{
			Output += FString::Printf(TEXT("%s\n"), *FormatEditorNode(EvalNode));
		}
	}
	else
	{
		Output += TEXT("(none)\n");
	}
	Output += TEXT("\n");

	// Global Tasks
	Output += TEXT("=== Global Tasks ===\n");
	if (EditorData->GlobalTasks.Num() > 0)
	{
		for (const FStateTreeEditorNode& TaskNode : EditorData->GlobalTasks)
		{
			Output += FString::Printf(TEXT("%s\n"), *FormatEditorNode(TaskNode));
		}
	}
	else
	{
		Output += TEXT("(none)\n");
	}
	Output += TEXT("\n");

	// State Hierarchy
	Output += TEXT("=== State Hierarchy ===\n");
	for (int32 i = 0; i < EditorData->SubTrees.Num(); i++)
	{
		bool bIsLast = (i == EditorData->SubTrees.Num() - 1);
		FormatStateRecursive(EditorData->SubTrees[i], Output, TEXT(""), bIsLast, FocusStateId);
	}
	Output += TEXT("\n");

	// Property Bindings
	Output += TEXT("=== Property Bindings ===\n");
	const auto& Bindings = EditorData->EditorBindings.GetBindings();
	if (Bindings.Num() > 0)
	{
		for (const FStateTreePropertyPathBinding& Binding : Bindings)
		{
			FString SourceStr;
			FString TargetStr;

#if WITH_EDITORONLY_DATA
			SourceStr = Binding.GetSourcePath().GetStructID().ToString(EGuidFormats::DigitsWithHyphensLower);
			for (const FStateTreePropertyPathSegment& Seg : Binding.GetSourcePath().GetSegments())
			{
				SourceStr += FString::Printf(TEXT(".%s"), *Seg.GetName().ToString());
			}

			TargetStr = Binding.GetTargetPath().GetStructID().ToString(EGuidFormats::DigitsWithHyphensLower);
			for (const FStateTreePropertyPathSegment& Seg : Binding.GetTargetPath().GetSegments())
			{
				TargetStr += FString::Printf(TEXT(".%s"), *Seg.GetName().ToString());
			}
#endif

			Output += FString::Printf(TEXT("%s \u2192 %s\n"), *SourceStr, *TargetStr);
		}
	}
	else
	{
		Output += TEXT("(none)\n");
	}

	return Output;
}

// ============================================================================
// Node Creation and Property Setting
// ============================================================================

UScriptStruct* ClaireonStateTreeHelpers::ResolveNodeStruct(const FString& StructName, FString& OutError)
{
	if (StructName.IsEmpty())
	{
		OutError = TEXT("Struct name is empty");
		return nullptr;
	}

	UScriptStruct* FoundStruct = FindFirstObject<UScriptStruct>(*StructName, EFindFirstObjectOptions::ExactClass | EFindFirstObjectOptions::NativeFirst);
	if (!FoundStruct)
	{
		OutError = FString::Printf(TEXT("Could not find struct: %s"), *StructName);
		return nullptr;
	}

	return FoundStruct;
}

bool ClaireonStateTreeHelpers::CreateEditorNode(FStateTreeEditorNode& OutNode, UScriptStruct* NodeStruct, UObject* Outer, FString& OutError)
{
	if (!NodeStruct)
	{
		OutError = TEXT("NodeStruct is null");
		return false;
	}

	OutNode.ID = FGuid::NewGuid();
	OutNode.Node.InitializeAs(NodeStruct);

	// Initialize instance data based on GetInstanceDataType()
	const FStateTreeNodeBase& NodeBase = OutNode.Node.Get<const FStateTreeNodeBase>();
	const UStruct* InstanceType = NodeBase.GetInstanceDataType();

	if (InstanceType)
	{
		if (const UScriptStruct* InstanceStruct = Cast<const UScriptStruct>(InstanceType))
		{
			// Struct-based instance data
			OutNode.Instance.InitializeAs(InstanceStruct);
		}
		else if (const UClass* InstanceClass = Cast<const UClass>(InstanceType))
		{
			// UObject-based instance data
			if (Outer)
			{
				OutNode.InstanceObject = NewObject<UObject>(Outer, const_cast<UClass*>(InstanceClass));
			}
			else
			{
				OutNode.InstanceObject = NewObject<UObject>(static_cast<UObject*>(GetTransientPackage()), const_cast<UClass*>(InstanceClass));
			}
		}
	}

	return true;
}

bool ClaireonStateTreeHelpers::SetNodeProperty(FStateTreeEditorNode& Node, const FString& PropertyName, const FString& PropertyValue, bool bOnInstanceData, FString& OutError)
{
	if (PropertyName.IsEmpty())
	{
		OutError = TEXT("Property name is empty");
		return false;
	}

	const UStruct* TargetStruct = nullptr;
	void* TargetData = nullptr;

	if (bOnInstanceData)
	{
		if (Node.Instance.IsValid())
		{
			TargetStruct = Node.Instance.GetScriptStruct();
			TargetData = Node.Instance.GetMutableMemory();
		}
		else if (Node.InstanceObject)
		{
			TargetStruct = Node.InstanceObject->GetClass();
			TargetData = Node.InstanceObject;
		}
		else
		{
			OutError = TEXT("Node has no instance data");
			return false;
		}
	}
	else
	{
		if (!Node.Node.IsValid())
		{
			OutError = TEXT("Node struct is invalid");
			return false;
		}
		TargetStruct = Node.Node.GetScriptStruct();
		TargetData = Node.Node.GetMutableMemory();
	}

	if (!TargetStruct || !TargetData)
	{
		OutError = TEXT("Could not resolve target struct or data");
		return false;
	}

	const FProperty* Prop = TargetStruct->FindPropertyByName(FName(*PropertyName));
	if (!Prop)
	{
		OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *TargetStruct->GetName());
		return false;
	}

	void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(TargetData);
	const TCHAR* Result = Prop->ImportText_Direct(*PropertyValue, ValuePtr, nullptr, PPF_None);
	if (!Result)
	{
		OutError = FString::Printf(TEXT("Failed to set property '%s' to '%s' on %s"), *PropertyName, *PropertyValue, *TargetStruct->GetName());
		return false;
	}

	return true;
}
