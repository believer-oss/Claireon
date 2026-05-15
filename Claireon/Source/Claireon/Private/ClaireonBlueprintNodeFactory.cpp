// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonBlueprintNodeFactory.h"
#include "ClaireonNameResolver.h"
#include "ClaireonLog.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"

// K2Node includes — mirrors Operation_AddNode's include set
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_Select.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_Knot.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeMap.h"
#include "K2Node_MakeSet.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_AddPinInterface.h"
#include "K2Node_Switch.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_SwitchName.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_ForEachElementInEnum.h"
#include "K2Node_DoOnceMultiInput.h"

#include "Kismet2/BlueprintEditorUtils.h"

#include "Dom/JsonValue.h"

namespace ClaireonBlueprintNodeFactory
{
	// ------------------------------------------------------------------------
	// Helpers
	// ------------------------------------------------------------------------

	namespace
	{
		/** Apply a generic reflection-based property bag onto a newly-created K2Node. */
		void ApplyReflectionProperties(UEdGraphNode* NewNode, const TSharedPtr<FJsonObject>& Props, TArray<FString>& OutWarnings)
		{
			if (!NewNode || !Props.IsValid()) return;

			for (auto& Pair : Props->Values)
			{
				FProperty* Prop = NewNode->GetClass()->FindPropertyByName(FName(*Pair.Key));
				if (!Prop)
				{
					OutWarnings.Add(FString::Printf(TEXT("node_properties: property '%s' not found on %s"),
						*Pair.Key, *NewNode->GetClass()->GetName()));
					continue;
				}

				void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(NewNode);

				if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
				{
					bool bVal = false;
					Pair.Value->TryGetBool(bVal);
					BoolProp->SetPropertyValue(ValuePtr, bVal);
				}
				else if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
				{
					double N = 0;
					Pair.Value->TryGetNumber(N);
					IntProp->SetPropertyValue(ValuePtr, static_cast<int32>(N));
				}
				else if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
				{
					double N = 0;
					Pair.Value->TryGetNumber(N);
					FloatProp->SetPropertyValue(ValuePtr, static_cast<float>(N));
				}
				else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
				{
					double N = 0;
					Pair.Value->TryGetNumber(N);
					DoubleProp->SetPropertyValue(ValuePtr, N);
				}
				else if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
				{
					FString S;
					Pair.Value->TryGetString(S);
					StrProp->SetPropertyValue(ValuePtr, S);
				}
				else if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
				{
					FString S;
					Pair.Value->TryGetString(S);
					NameProp->SetPropertyValue(ValuePtr, FName(*S));
				}
				else if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
				{
					FString S;
					Pair.Value->TryGetString(S);
					if (ObjProp->PropertyClass && ObjProp->PropertyClass->IsChildOf(UClass::StaticClass()))
					{
						ClaireonNameResolver::FNameResolveResult R;
						UClass* Found = ClaireonNameResolver::ResolveClassName(S, nullptr, R);
						if (Found)
						{
							ObjProp->SetObjectPropertyValue(ValuePtr, Found);
							if (!R.ResolutionNote.IsEmpty()) OutWarnings.Add(R.ResolutionNote);
						}
					}
					else if (ObjProp->PropertyClass && ObjProp->PropertyClass->IsChildOf(UEnum::StaticClass()))
					{
						ClaireonNameResolver::FNameResolveResult R;
						UEnum* Found = ClaireonNameResolver::ResolveEnumName(S, R);
						if (Found)
						{
							ObjProp->SetObjectPropertyValue(ValuePtr, Found);
							if (!R.ResolutionNote.IsEmpty()) OutWarnings.Add(R.ResolutionNote);
						}
					}
					else if (ObjProp->PropertyClass && ObjProp->PropertyClass->IsChildOf(UScriptStruct::StaticClass()))
					{
						ClaireonNameResolver::FNameResolveResult R;
						UScriptStruct* Found = ClaireonNameResolver::ResolveStructName(S, R);
						if (Found)
						{
							ObjProp->SetObjectPropertyValue(ValuePtr, Found);
							if (!R.ResolutionNote.IsEmpty()) OutWarnings.Add(R.ResolutionNote);
						}
					}
					else
					{
						OutWarnings.Add(FString::Printf(TEXT("node_properties: unsupported object property type for '%s'"), *Pair.Key));
					}
				}
				else
				{
					OutWarnings.Add(FString::Printf(TEXT("node_properties: unsupported property type for '%s'"), *Pair.Key));
				}
			}
		}

		/** Apply num_extra_pins to a dynamic-pin node (Sequence, MakeArray, Switch, etc.). */
		void ApplyExtraPins(UEdGraphNode* NewNode, const TSharedPtr<FJsonObject>& Params)
		{
			int32 NumExtraPins = 0;
			if (!Params.IsValid() || !Params->TryGetNumberField(TEXT("num_extra_pins"), NumExtraPins) || NumExtraPins <= 0)
			{
				return;
			}
			NumExtraPins = FMath::Clamp(NumExtraPins, 0, 50);

			IK2Node_AddPinInterface* AddPinIface = Cast<IK2Node_AddPinInterface>(NewNode);
			UK2Node_Switch* SwitchNode = Cast<UK2Node_Switch>(NewNode);

			if (AddPinIface)
			{
				for (int32 i = 0; i < NumExtraPins && AddPinIface->CanAddPin(); ++i)
				{
					AddPinIface->AddInputPin();
				}
			}
			else if (SwitchNode && !SwitchNode->IsA<UK2Node_SwitchEnum>())
			{
				for (int32 i = 0; i < NumExtraPins; ++i)
				{
					SwitchNode->AddPinToSwitchNode();
				}
			}
		}
	}

	// ------------------------------------------------------------------------
	// CreateNode — typed dispatch for K2 node creation.
	// Mirrors the node_type surface of Operation_AddNode (see that file for the
	// canonical schema documentation). The two are intentionally kept in sync;
	// unifying them into a single dispatcher is tracked as a follow-up refactor.
	// ------------------------------------------------------------------------

	FCreateResult CreateNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TSharedPtr<FJsonObject>& Params,
		const FVector2D& Position)
	{
		FCreateResult Out;

		if (!Blueprint || !Graph || !Params.IsValid())
		{
			Out.Error = TEXT("CreateNode: Blueprint, Graph, and Params all required");
			return Out;
		}

		FString NodeType;
		if (!Params->TryGetStringField(TEXT("node_type"), NodeType))
		{
			Out.Error = TEXT("Missing required field: node_type");
			return Out;
		}

		UEdGraphNode* NewNode = nullptr;
		FString Desc;

		// -------- Typed dispatch --------
		if (NodeType == TEXT("CallFunction"))
		{
			FString FunctionName, FunctionClass;
			if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
			{
				Out.Error = TEXT("CallFunction: missing required field 'function_name'");
				return Out;
			}
			Params->TryGetStringField(TEXT("function_class"), FunctionClass);

			UK2Node_CallFunction* N = NewObject<UK2Node_CallFunction>(Graph);
			if (!FunctionClass.IsEmpty())
			{
				ClaireonNameResolver::FNameResolveResult R;
				if (UClass* Owner = ClaireonNameResolver::ResolveClassName(FunctionClass, nullptr, R))
				{
					N->FunctionReference.SetExternalMember(FName(*FunctionName), Owner);
					if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);
				}
				else
				{
					N->FunctionReference.SetSelfMember(FName(*FunctionName));
				}
			}
			else
			{
				N->FunctionReference.SetSelfMember(FName(*FunctionName));
			}
			NewNode = N;
			Desc = FString::Printf(TEXT("CallFunction: %s"), *FunctionName);
		}
		else if (NodeType == TEXT("CallParentFunction"))
		{
			FString FunctionName;
			if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
			{
				Out.Error = TEXT("CallParentFunction: missing required field 'function_name'");
				return Out;
			}
			UClass* ParentClass = Blueprint->ParentClass;
			ClaireonNameResolver::FNameResolveResult R;
			UFunction* Target = ParentClass ? ClaireonNameResolver::ResolveFunctionName(ParentClass, FunctionName, R) : nullptr;
			if (!Target)
			{
				Out.Error = R.Error.IsEmpty() ? FString::Printf(TEXT("Function '%s' not found on parent class"), *FunctionName) : R.Error;
				return Out;
			}
			if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);

			UK2Node_CallParentFunction* N = NewObject<UK2Node_CallParentFunction>(Graph);
			N->SetFromFunction(Target);
			NewNode = N;
			Desc = FString::Printf(TEXT("Call Parent: %s"), *Target->GetName());
		}
		else if (NodeType == TEXT("VariableGet"))
		{
			FString VariableName;
			if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
			{
				Out.Error = TEXT("VariableGet: missing required field 'variable_name'");
				return Out;
			}
			UK2Node_VariableGet* N = NewObject<UK2Node_VariableGet>(Graph);
			N->VariableReference.SetSelfMember(FName(*VariableName));
			NewNode = N;
			Desc = FString::Printf(TEXT("Get %s"), *VariableName);
		}
		else if (NodeType == TEXT("VariableSet"))
		{
			FString VariableName;
			if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
			{
				Out.Error = TEXT("VariableSet: missing required field 'variable_name'");
				return Out;
			}
			UK2Node_VariableSet* N = NewObject<UK2Node_VariableSet>(Graph);
			N->VariableReference.SetSelfMember(FName(*VariableName));
			NewNode = N;
			Desc = FString::Printf(TEXT("Set %s"), *VariableName);
		}
		else if (NodeType == TEXT("Branch"))
		{
			NewNode = NewObject<UK2Node_IfThenElse>(Graph);
			Desc = TEXT("Branch");
		}
		else if (NodeType == TEXT("Sequence"))
		{
			NewNode = NewObject<UK2Node_ExecutionSequence>(Graph);
			Desc = TEXT("Sequence");
		}
		else if (NodeType == TEXT("Cast"))
		{
			FString TargetClass;
			if (!Params->TryGetStringField(TEXT("target_class"), TargetClass))
			{
				Out.Error = TEXT("Cast: missing required field 'target_class'");
				return Out;
			}
			ClaireonNameResolver::FNameResolveResult R;
			UClass* C = ClaireonNameResolver::ResolveClassName(TargetClass, nullptr, R);
			if (!C) { Out.Error = R.Error; return Out; }
			if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);

			UK2Node_DynamicCast* N = NewObject<UK2Node_DynamicCast>(Graph);
			N->TargetType = C;
			NewNode = N;
			Desc = FString::Printf(TEXT("Cast to %s"), *TargetClass);
		}
		else if (NodeType == TEXT("SpawnActor"))
		{
			FString ActorClass;
			if (!Params->TryGetStringField(TEXT("actor_class"), ActorClass))
			{
				Out.Error = TEXT("SpawnActor: missing required field 'actor_class'");
				return Out;
			}
			ClaireonNameResolver::FNameResolveResult R;
			UClass* C = ClaireonNameResolver::ResolveClassName(ActorClass, AActor::StaticClass(), R);
			if (!C) { Out.Error = R.Error; return Out; }
			if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);

			NewNode = NewObject<UK2Node_SpawnActorFromClass>(Graph);
			Desc = FString::Printf(TEXT("Spawn %s"), *ActorClass);
		}
		else if (NodeType == TEXT("CustomEvent"))
		{
			FString EventName;
			if (!Params->TryGetStringField(TEXT("event_name"), EventName))
			{
				Out.Error = TEXT("CustomEvent: missing required field 'event_name'");
				return Out;
			}
			UK2Node_CustomEvent* N = NewObject<UK2Node_CustomEvent>(Graph);
			N->CustomFunctionName = FName(*EventName);
			NewNode = N;
			Desc = FString::Printf(TEXT("Custom Event: %s"), *EventName);
		}
		else if (NodeType == TEXT("Knot"))
		{
			NewNode = NewObject<UK2Node_Knot>(Graph);
			Desc = TEXT("Reroute Node");
		}
		else if (NodeType == TEXT("Comment"))
		{
			FString CommentText;
			if (!Params->TryGetStringField(TEXT("comment_text"), CommentText))
			{
				CommentText = TEXT("Comment");
			}
			UEdGraphNode_Comment* N = NewObject<UEdGraphNode_Comment>(Graph);
			N->NodeComment = CommentText;
			NewNode = N;
			Desc = FString::Printf(TEXT("Comment: %s"), *CommentText);
		}
		else if (NodeType == TEXT("Select"))
		{
			NewNode = NewObject<UK2Node_Select>(Graph);
			Desc = TEXT("Select");
		}
		else if (NodeType == TEXT("MakeArray"))
		{
			NewNode = NewObject<UK2Node_MakeArray>(Graph);
			Desc = TEXT("Make Array");
		}
		else if (NodeType == TEXT("MakeSet"))
		{
			NewNode = NewObject<UK2Node_MakeSet>(Graph);
			Desc = TEXT("Make Set");
		}
		else if (NodeType == TEXT("MakeMap"))
		{
			NewNode = NewObject<UK2Node_MakeMap>(Graph);
			Desc = TEXT("Make Map");
		}
		else if (NodeType == TEXT("GetArrayItem"))
		{
			NewNode = NewObject<UK2Node_GetArrayItem>(Graph);
			Desc = TEXT("Get Array Item");
		}
		else if (NodeType == TEXT("MakeStruct"))
		{
			FString StructType;
			if (!Params->TryGetStringField(TEXT("struct_type"), StructType))
			{
				Out.Error = TEXT("MakeStruct: missing required field 'struct_type'");
				return Out;
			}
			ClaireonNameResolver::FNameResolveResult R;
			UScriptStruct* S = ClaireonNameResolver::ResolveStructName(StructType, R);
			if (!S) { Out.Error = R.Error; return Out; }
			if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);

			UK2Node_MakeStruct* N = NewObject<UK2Node_MakeStruct>(Graph);
			N->StructType = S;
			NewNode = N;
			Desc = FString::Printf(TEXT("Make %s"), *StructType);
		}
		else if (NodeType == TEXT("BreakStruct"))
		{
			FString StructType;
			if (!Params->TryGetStringField(TEXT("struct_type"), StructType))
			{
				Out.Error = TEXT("BreakStruct: missing required field 'struct_type'");
				return Out;
			}
			ClaireonNameResolver::FNameResolveResult R;
			UScriptStruct* S = ClaireonNameResolver::ResolveStructName(StructType, R);
			if (!S) { Out.Error = R.Error; return Out; }
			if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);

			UK2Node_BreakStruct* N = NewObject<UK2Node_BreakStruct>(Graph);
			N->StructType = S;
			NewNode = N;
			Desc = FString::Printf(TEXT("Break %s"), *StructType);
		}
		else if (NodeType == TEXT("SwitchInteger"))
		{
			NewNode = NewObject<UK2Node_SwitchInteger>(Graph);
			Desc = TEXT("Switch on Int");
		}
		else if (NodeType == TEXT("SwitchString"))
		{
			NewNode = NewObject<UK2Node_SwitchString>(Graph);
			Desc = TEXT("Switch on String");
		}
		else if (NodeType == TEXT("SwitchName"))
		{
			NewNode = NewObject<UK2Node_SwitchName>(Graph);
			Desc = TEXT("Switch on Name");
		}
		else if (NodeType == TEXT("SwitchEnum"))
		{
			FString EnumType;
			if (!Params->TryGetStringField(TEXT("enum_type"), EnumType))
			{
				Out.Error = TEXT("SwitchEnum: missing required field 'enum_type'");
				return Out;
			}
			ClaireonNameResolver::FNameResolveResult R;
			UEnum* E = ClaireonNameResolver::ResolveEnumName(EnumType, R);
			if (!E) { Out.Error = R.Error; return Out; }
			if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);

			UK2Node_SwitchEnum* N = NewObject<UK2Node_SwitchEnum>(Graph);
			N->Enum = E;
			N->EnumEntries.Empty();
			N->EnumFriendlyNames.Empty();
			for (int32 Idx = 0; Idx < E->NumEnums() - 1; ++Idx)
			{
				const bool bHidden = E->HasMetaData(TEXT("Hidden"), Idx) || E->HasMetaData(TEXT("Spacer"), Idx);
				if (!bHidden)
				{
					N->EnumEntries.Add(FName(*E->GetNameStringByIndex(Idx)));
					N->EnumFriendlyNames.Add(E->GetDisplayNameTextByIndex(Idx));
				}
			}
			NewNode = N;
			Desc = FString::Printf(TEXT("Switch on %s"), *EnumType);
		}
		else if (NodeType == TEXT("ForEachElementInEnum"))
		{
			FString EnumType;
			if (!Params->TryGetStringField(TEXT("enum_type"), EnumType))
			{
				Out.Error = TEXT("ForEachElementInEnum: missing required field 'enum_type'");
				return Out;
			}
			ClaireonNameResolver::FNameResolveResult R;
			UEnum* E = ClaireonNameResolver::ResolveEnumName(EnumType, R);
			if (!E) { Out.Error = R.Error; return Out; }
			if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);

			UK2Node_ForEachElementInEnum* N = NewObject<UK2Node_ForEachElementInEnum>(Graph);
			N->Enum = E;
			NewNode = N;
			Desc = FString::Printf(TEXT("For Each %s"), *EnumType);
		}
		else if (NodeType == TEXT("DoOnceMultiInput"))
		{
			NewNode = NewObject<UK2Node_DoOnceMultiInput>(Graph);
			Desc = TEXT("Do Once (Multi Input)");
		}
		else if (NodeType == TEXT("Macro") || NodeType == TEXT("ForEachLoop") || NodeType == TEXT("ForEachLoopWithBreak")
			|| NodeType == TEXT("ForLoop") || NodeType == TEXT("ForLoopWithBreak") || NodeType == TEXT("WhileLoop")
			|| NodeType == TEXT("DoOnce") || NodeType == TEXT("DoN") || NodeType == TEXT("FlipFlop")
			|| NodeType == TEXT("Gate") || NodeType == TEXT("MultiGate") || NodeType == TEXT("IsValid"))
		{
			FString MacroName;
			FString MacroLibraryPath = TEXT("/Engine/EditorBlueprintResources/StandardMacros");

			if (NodeType == TEXT("Macro"))
			{
				if (!Params->TryGetStringField(TEXT("macro_name"), MacroName))
				{
					Out.Error = TEXT("Macro: missing required field 'macro_name'");
					return Out;
				}
				FString Custom;
				if (Params->TryGetStringField(TEXT("macro_library_path"), Custom)) MacroLibraryPath = Custom;
			}
			else
			{
				MacroName = NodeType;
			}

			UBlueprint* Lib = LoadObject<UBlueprint>(nullptr, *MacroLibraryPath);
			if (!Lib)
			{
				Out.Error = FString::Printf(TEXT("Failed to load macro library: %s"), *MacroLibraryPath);
				return Out;
			}

			UEdGraph* MacroGraph = nullptr;
			for (UEdGraph* G : Lib->MacroGraphs)
			{
				if (G && G->GetName() == MacroName) { MacroGraph = G; break; }
			}
			if (!MacroGraph)
			{
				TArray<FString> Avail;
				for (UEdGraph* G : Lib->MacroGraphs) { if (G) Avail.Add(G->GetName()); }
				Out.Error = FString::Printf(TEXT("Macro '%s' not found in %s. Available: %s"),
					*MacroName, *MacroLibraryPath, *FString::Join(Avail, TEXT(", ")));
				return Out;
			}

			UK2Node_MacroInstance* N = NewObject<UK2Node_MacroInstance>(Graph);
			N->SetMacroGraph(MacroGraph);
			NewNode = N;
			Desc = FString::Printf(TEXT("Macro: %s"), *MacroName);
		}
		else if (NodeType == TEXT("Generic"))
		{
			FString ClassName;
			if (!Params->TryGetStringField(TEXT("class_name"), ClassName))
			{
				Out.Error = TEXT("Generic: missing required field 'class_name'");
				return Out;
			}
			ClaireonNameResolver::FNameResolveResult R;
			UClass* C = ClaireonNameResolver::ResolveClassName(ClassName, UK2Node::StaticClass(), R);
			if (!C) { Out.Error = R.Error; return Out; }
			if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);

			if (!C->IsChildOf(UEdGraphNode::StaticClass()))
			{
				Out.Error = FString::Printf(TEXT("Class '%s' is not a graph-node class"), *ClassName);
				return Out;
			}

			NewNode = NewObject<UEdGraphNode>(Graph, C);
			Desc = FString::Printf(TEXT("Generic: %s"), *ClassName);
		}
		else
		{
			Out.Error = FString::Printf(
				TEXT("Unsupported node_type '%s' in factory. Use 'Generic' with 'class_name' for custom types, "
				     "or Operation_AddNode (claireon.blueprint_edit_graph) for typed support not yet in the factory "
				     "(SpawnActor variants, Delegate nodes, Timeline, EventOverride)."), *NodeType);
			return Out;
		}

		if (!NewNode)
		{
			Out.Error = FString::Printf(TEXT("Factory produced null node for type '%s'"), *NodeType);
			return Out;
		}

		// -------- Apply node_properties (Generic path and — harmlessly — any type that passes them) --------
		bool bWroteProperties = false;
		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		if (Params->TryGetObjectField(TEXT("node_properties"), PropsObj) && PropsObj && (*PropsObj).IsValid())
		{
			ApplyReflectionProperties(NewNode, *PropsObj, Out.Warnings);
			bWroteProperties = (*PropsObj)->Values.Num() > 0;
		}

		// -------- Add to graph + AllocateDefaultPins --------
		NewNode->NodePosX = static_cast<int32>(Position.X);
		NewNode->NodePosY = static_cast<int32>(Position.Y);
		NewNode->CreateNewGuid();
		Graph->AddNode(NewNode, false, false);
		NewNode->AllocateDefaultPins();

		// H2: Dynamic-pin nodes (SwitchEnum, Sequence, MakeArray, Select, etc.) regenerate
		// their real pin set in ReconstructNode from written UPROPERTY state. AllocateDefaultPins
		// alone is not enough when properties were set pre-allocate.
		if (bWroteProperties)
		{
			NewNode->ReconstructNode();
		}

		// -------- Dynamic pins --------
		ApplyExtraPins(NewNode, Params);

		Out.Node = NewNode;
		Out.Description = Desc;
		Out.bAlreadyAdded = true; // factory already added to graph
		return Out;
	}
}
