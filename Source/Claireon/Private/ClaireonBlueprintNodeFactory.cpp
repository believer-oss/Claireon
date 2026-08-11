// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonBlueprintNodeFactory.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonNameResolver.h"
#include "GameplayTask.h"
#include "Abilities/Tasks/AbilityTask.h"
#include "ClaireonLog.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

// K2Node includes — mirrors Operation_AddNode's include set
#include "K2Node.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallDataTableFunction.h"
#include "K2Node_CallMaterialParameterCollectionFunction.h"
#include "K2Node_CommutativeAssociativeBinaryOperator.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "Misc/EngineVersionComparison.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_Select.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_SetFieldsInStruct.h"
#include "K2Node_StructOperation.h"
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
#include "K2Node_MultiGate.h"
#include "K2Node_FormatText.h"
#include "K2Node_Composite.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Tunnel.h"
#include "GameplayTagsK2Node_SwitchGameplayTag.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "AnimGraphNode_Base.h"
#include "Animation/AnimBlueprint.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Misc/PackageName.h"
#include "UObject/PropertyPortFlags.h"

#include "Dom/JsonValue.h"

namespace ClaireonBlueprintNodeFactory
{
	// ------------------------------------------------------------------------
	// Helpers
	// ------------------------------------------------------------------------

	namespace ClaireonBlueprintNodeFactory_Private
	{
		/** Apply a generic reflection-based property bag onto a newly-created K2Node. */
		void ApplyReflectionProperties(UEdGraphNode* NewNode, const TSharedPtr<FJsonObject>& Props, TArray<FString>& OutWarnings)
		{
			if (!IsValid(NewNode) || !Props.IsValid()) return;

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
						if (IsValid(Found))
						{
							ObjProp->SetObjectPropertyValue(ValuePtr, Found);
							if (!R.ResolutionNote.IsEmpty()) OutWarnings.Add(R.ResolutionNote);
						}
					}
					else if (ObjProp->PropertyClass && ObjProp->PropertyClass->IsChildOf(UEnum::StaticClass()))
					{
						ClaireonNameResolver::FNameResolveResult R;
						UEnum* Found = ClaireonNameResolver::ResolveEnumName(S, R);
						if (IsValid(Found))
						{
							ObjProp->SetObjectPropertyValue(ValuePtr, Found);
							if (!R.ResolutionNote.IsEmpty()) OutWarnings.Add(R.ResolutionNote);
						}
					}
					else if (ObjProp->PropertyClass && ObjProp->PropertyClass->IsChildOf(UScriptStruct::StaticClass()))
					{
						ClaireonNameResolver::FNameResolveResult R;
						UScriptStruct* Found = ClaireonNameResolver::ResolveStructName(S, R);
						if (IsValid(Found))
						{
							ObjProp->SetObjectPropertyValue(ValuePtr, Found);
							if (!R.ResolutionNote.IsEmpty()) OutWarnings.Add(R.ResolutionNote);
						}
					}
					else
					{
						// General asset/object reference (e.g. UInputAction on
						// K2Node_EnhancedInputAction): load by object path.
						UObject* Loaded = LoadObject<UObject>(nullptr, *S);
						if (IsValid(Loaded) && ObjProp->PropertyClass && Loaded->IsA(ObjProp->PropertyClass))
						{
							ObjProp->SetObjectPropertyValue(ValuePtr, Loaded);
						}
						else
						{
							OutWarnings.Add(FString::Printf(
								TEXT("node_properties: could not resolve object '%s' for property '%s' (expected %s; pass a full object path like /Game/Dir/Asset.Asset)"),
								*S, *Pair.Key,
								ObjProp->PropertyClass ? *ObjProp->PropertyClass->GetName() : TEXT("UObject")));
						}
					}
				}
				else if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
				{
					// Struct value from its text form (e.g. FKey InputKey = "F" or "Gamepad_FaceButton_Bottom").
					FString S;
					Pair.Value->TryGetString(S);
					if (StructProp->ImportText_Direct(*S, ValuePtr, NewNode, PPF_None) == nullptr)
					{
						OutWarnings.Add(FString::Printf(
							TEXT("node_properties: failed to import struct value '%s' for property '%s' (%s)"),
							*S, *Pair.Key, *StructProp->Struct->GetName()));
					}
				}
				else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
				{
					double N = 0;
					FString S;
					if (Pair.Value->TryGetNumber(N))
					{
						ByteProp->SetPropertyValue(ValuePtr, static_cast<uint8>(N));
					}
					else if (Pair.Value->TryGetString(S) && ByteProp->Enum)
					{
						const int64 EnumVal = ByteProp->Enum->GetValueByNameString(S);
						if (EnumVal != INDEX_NONE)
						{
							ByteProp->SetPropertyValue(ValuePtr, static_cast<uint8>(EnumVal));
						}
						else
						{
							OutWarnings.Add(FString::Printf(TEXT("node_properties: unknown enum entry '%s' for property '%s'"), *S, *Pair.Key));
						}
					}
				}
				else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
				{
					FString S;
					double N = 0;
					int64 EnumVal = INDEX_NONE;
					if (Pair.Value->TryGetString(S))
					{
						EnumVal = IsValid(EnumProp->GetEnum()) ? EnumProp->GetEnum()->GetValueByNameString(S) : INDEX_NONE;
					}
					else if (Pair.Value->TryGetNumber(N))
					{
						EnumVal = static_cast<int64>(N);
					}
					if (EnumVal != INDEX_NONE)
					{
						EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, EnumVal);
					}
					else
					{
						OutWarnings.Add(FString::Printf(TEXT("node_properties: unknown enum value for property '%s'"), *Pair.Key));
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
			else if (IsValid(SwitchNode) && !SwitchNode->IsA<UK2Node_SwitchEnum>())
			{
				for (int32 i = 0; i < NumExtraPins; ++i)
				{
					SwitchNode->AddPinToSwitchNode();
				}
			}
		}
	}
	using namespace ClaireonBlueprintNodeFactory_Private;

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

		if (!IsValid(Blueprint) || !IsValid(Graph) || !Params.IsValid())
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
		// Branches that must control their own graph insertion (e.g. Composite, whose
		// PostPlacedNewNode needs GetGraph() live) set this to skip the shared tail insert.
		bool bSelfInserted = false;
		// Deferred per-branch fixup that must run AFTER the shared tail's
		// AllocateDefaultPins (e.g. SpawnActor's Class pin default, which only
		// exists once pins allocate).
		TFunction<void()> PostAllocate;

		// VariableGet/VariableSet binding bookkeeping for the post-AllocateDefaultPins
		// validation in the shared tail: set when the reference was bound against a
		// function INPUT PARAMETER (rather than a member or declared local) so a
		// binding failure can emit the parameter-specific guidance, plus the scope
		// name the binding targeted (for the error text).
		bool bVariableBoundAsParameter = false;
		FString VariableBindScopeName;

		// -------- Typed dispatch --------
		// CallArrayFunction is an accepted spelling of CallFunction: the specialized
		// node class is picked from the resolved UFunction's MD_ArrayParam metadata,
		// so both spellings produce the correct node.
		if (NodeType == TEXT("CallFunction") || NodeType == TEXT("CallArrayFunction"))
		{
			FString FunctionName, FunctionClass;
			if (!Params->TryGetStringField(TEXT("function_name"), FunctionName) || FunctionName.IsEmpty()
				|| FunctionName == TEXT("None"))
			{
				// An empty/None name would silently author an unbound 'None' call node.
				Out.Error = TEXT("CallFunction: missing or empty required field 'function_name'");
				return Out;
			}
			Params->TryGetStringField(TEXT("function_class"), FunctionClass);

			// Resolve owner class up-front. When function_class is supplied but
			// cannot be resolved, surface a warning instead of silently falling
			// through to SetSelfMember.
			UClass* ResolvedOwnerClass = nullptr;
			if (!FunctionClass.IsEmpty())
			{
				ClaireonNameResolver::FNameResolveResult R;
				ResolvedOwnerClass = ClaireonNameResolver::ResolveClassName(FunctionClass, nullptr, R);
				if (IsValid(ResolvedOwnerClass))
				{
					if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);
				}
				else
				{
					Out.Warnings.Add(FString::Printf(
						TEXT("CallFunction: function_class '%s' could not be resolved; falling back to Self. (%s)"),
						*FunctionClass, *R.Error));
				}
			}

			// Look up the UFunction so we can (a) pick the specialized subclass
			// and (b) give ReconstructNode real data to work with in the shared
			// tail. Self-bound functions live on SkeletonGeneratedClass.
			UFunction* ResolvedFunction = nullptr;
			if (IsValid(ResolvedOwnerClass))
			{
				ResolvedFunction = ResolvedOwnerClass->FindFunctionByName(FName(*FunctionName));
			}
			else if (IsValid(Blueprint->SkeletonGeneratedClass))
			{
				ResolvedFunction = Blueprint->SkeletonGeneratedClass->FindFunctionByName(FName(*FunctionName));
			}
			if (!IsValid(ResolvedFunction))
			{
				Out.Warnings.Add(FString::Printf(
					TEXT("CallFunction: function '%s' not found on %s; node will be created with empty pins."),
					*FunctionName,
					IsValid(ResolvedOwnerClass) ? *ResolvedOwnerClass->GetName()
					                   : (IsValid(Blueprint->SkeletonGeneratedClass) ? *Blueprint->SkeletonGeneratedClass->GetName()
					                                                        : TEXT("<no owner>"))));
			}

			// Optional exact node class override (e.g. K2Node_PromotableOperator,
			// K2Node_CallArrayFunction). Replay callers pass the source's node
			// class so the copy carries the same class instead of the inferred
			// one -- the editor spawns PromotableOperator for operator entries,
			// which no metadata on the UFunction distinguishes after the fact.
			UClass* NodeClass = nullptr;
			FString NodeClassName;
			if (Params->TryGetStringField(TEXT("node_class"), NodeClassName) && !NodeClassName.IsEmpty())
			{
				ClaireonNameResolver::FNameResolveResult NodeClassResolve;
				UClass* Requested = ClaireonNameResolver::ResolveClassName(NodeClassName, nullptr, NodeClassResolve);
				if (IsValid(Requested) && Requested->IsChildOf(UK2Node_CallFunction::StaticClass()))
				{
					NodeClass = Requested;
				}
				else
				{
					Out.Warnings.Add(FString::Printf(
						TEXT("CallFunction: node_class '%s' %s; using the class inferred from the function."),
						*NodeClassName,
						IsValid(Requested) ? TEXT("is not a UK2Node_CallFunction subclass") : TEXT("could not be resolved")));
				}
			}
			if (!IsValid(NodeClass))
			{
				NodeClass = ClaireonBlueprintHelpers::PickK2NodeClassForFunction(ResolvedFunction);
			}

			if (IsValid(NodeClass) && NodeClass->IsChildOf(UK2Node_BaseAsyncTask::StaticClass()))
			{
				// Async/latent branch: helper guarantees ResolvedFunction is a valid
				// proxy factory. No FunctionReference set; InitializeProxyFromFunction
				// populates the proxy fields directly.
				//
				// Construct NodeClass itself rather than UK2Node_AsyncAction: the helper
				// also promotes GameplayTask factories to UK2Node_LatentGameplayTaskCall /
				// UK2Node_LatentAbilityCall, which are BaseAsyncTask subclasses too, and
				// hardcoding the base would silently build the wrong node class.
				UEdGraphNode* AsyncNode = NewObject<UEdGraphNode>(Graph, NodeClass);

				if (UK2Node_AsyncAction* AsAsyncAction = Cast<UK2Node_AsyncAction>(AsyncNode); IsValid(AsAsyncAction))
				{
					AsAsyncAction->InitializeProxyFromFunction(ResolvedFunction);
				}
				else
				{
					// The latent node classes do not derive from UK2Node_AsyncAction, so
					// InitializeProxyFromFunction is not available. Write the same three
					// proxy fields UK2Node_BaseAsyncTask reads, by reflection -- the same
					// mechanism the Generic route already uses for these classes.
					UClass* ProxyReturnClass = nullptr;
					if (const FObjectProperty* RetProp =
							CastField<FObjectProperty>(ResolvedFunction->GetReturnProperty()))
					{
						ProxyReturnClass = RetProp->PropertyClass;
					}

					if (FNameProperty* FnNameProp = CastField<FNameProperty>(
							NodeClass->FindPropertyByName(TEXT("ProxyFactoryFunctionName"))))
					{
						FnNameProp->SetPropertyValue_InContainer(AsyncNode, FName(*FunctionName));
					}
					if (FClassProperty* FactoryClassProp = CastField<FClassProperty>(
							NodeClass->FindPropertyByName(TEXT("ProxyFactoryClass"))))
					{
						FactoryClassProp->SetPropertyValue_InContainer(
							AsyncNode, ResolvedFunction->GetOwnerClass());
					}
					if (FClassProperty* ProxyClassProp = CastField<FClassProperty>(
							NodeClass->FindPropertyByName(TEXT("ProxyClass"))))
					{
						ProxyClassProp->SetPropertyValue_InContainer(AsyncNode, ProxyReturnClass);
					}
				}

				NewNode = AsyncNode;
				Desc = FString::Printf(TEXT("AsyncAction: %s (%s)"), *FunctionName, *NodeClass->GetName());
			}
			else
			{
				UK2Node_CallFunction* N = NewObject<UK2Node_CallFunction>(Graph, NodeClass);

				if (IsValid(ResolvedOwnerClass))
				{
					N->FunctionReference.SetExternalMember(FName(*FunctionName), ResolvedOwnerClass);
				}
				else
				{
					// Either function_class was empty or it failed to resolve. Either
					// way, route through SetSelfMember so functions on the blueprint's
					// skeleton class (card repro: SetHiddenInGame self-bound on
					// SceneComponent-owning actor) get their UFunction reference.
					N->FunctionReference.SetSelfMember(FName(*FunctionName));
				}
				NewNode = N;
				Desc = FString::Printf(TEXT("CallFunction: %s (%s)"), *FunctionName, *NodeClass->GetName());
			}
		}
		else if (NodeType == TEXT("AsyncAction"))
		{
			// Explicit AsyncAction surface: callers state intent directly
			// instead of relying on the CallFunction-helper-detection conjunction
			// in PickK2NodeClassForFunction. Both paths construct the same
			// node; this one skips the four-conjunct guard for callers who
			// already know they want an AsyncAction node.
			FString FunctionName, FunctionClass;
			if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
			{
				Out.Error = TEXT("AsyncAction: missing required field 'function_name'");
				return Out;
			}
			if (!Params->TryGetStringField(TEXT("function_class"), FunctionClass))
			{
				Out.Error = TEXT("AsyncAction: missing required field 'function_class' (the UClass that hosts the async factory UFUNCTION, e.g. '/Script/MyModule.MyAsyncAction')");
				return Out;
			}

			ClaireonNameResolver::FNameResolveResult R;
			UClass* OwnerClass = ClaireonNameResolver::ResolveClassName(FunctionClass, UBlueprintAsyncActionBase::StaticClass(), R);
			if (!IsValid(OwnerClass))
			{
				// A GameplayTask factory is the common reason to land here: it is a
				// latent node, not an AsyncAction, so name the route that works
				// instead of just reporting the base-class mismatch.
				ClaireonNameResolver::FNameResolveResult UnfilteredResult;
				UClass* UnfilteredOwner =
					ClaireonNameResolver::ResolveClassName(FunctionClass, nullptr, UnfilteredResult);
				if (IsValid(UnfilteredOwner) && UnfilteredOwner->IsChildOf(UGameplayTask::StaticClass()))
				{
					const bool bIsAbilityTask = UnfilteredOwner->IsChildOf(UAbilityTask::StaticClass());
					Out.Error = FString::Printf(
						TEXT("AsyncAction: function_class '%s' is a %s factory, not a UBlueprintAsyncActionBase. ")
						TEXT("Use node_type='%s' with the same function_name/function_class, or ")
						TEXT("node_type='Generic' with class_name='%s'."),
						*FunctionClass,
						bIsAbilityTask ? TEXT("UAbilityTask") : TEXT("UGameplayTask"),
						bIsAbilityTask ? TEXT("LatentAbilityCall") : TEXT("LatentGameplayTaskCall"),
						bIsAbilityTask ? TEXT("K2Node_LatentAbilityCall") : TEXT("K2Node_LatentGameplayTaskCall"));
					return Out;
				}

				Out.Error = FString::Printf(
					TEXT("AsyncAction: function_class '%s' could not be resolved to a UBlueprintAsyncActionBase-derived class. (%s)"),
					*FunctionClass, *R.Error);
				return Out;
			}
			if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);

			UFunction* ResolvedFunction = OwnerClass->FindFunctionByName(FName(*FunctionName));
			if (!IsValid(ResolvedFunction))
			{
				Out.Error = FString::Printf(
					TEXT("AsyncAction: factory function '%s' not found on class '%s'."),
					*FunctionName, *OwnerClass->GetName());
				return Out;
			}

			UK2Node_AsyncAction* AsyncNode = NewObject<UK2Node_AsyncAction>(Graph);
			AsyncNode->InitializeProxyFromFunction(ResolvedFunction);
			NewNode = AsyncNode;
			Desc = FString::Printf(TEXT("AsyncAction: %s (%s)"), *FunctionName, *OwnerClass->GetName());
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
			UFunction* Target = IsValid(ParentClass) ? ClaireonNameResolver::ResolveFunctionName(ParentClass, FunctionName, R) : nullptr;
			if (!IsValid(Target))
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
		else if (NodeType == TEXT("VariableGet") || NodeType == TEXT("VariableSet"))
		{
			const bool bIsGet = (NodeType == TEXT("VariableGet"));
			FString VariableName;
			if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
			{
				Out.Error = FString::Printf(TEXT("%s: missing required field 'variable_name'"), *NodeType);
				return Out;
			}
			const FName VarFName(*VariableName);

			// Member binding forms (mirrors FMemberReference's serialized shapes):
			//   member_scope   -> function-local variable (VariableReference=(MemberScope=...,MemberName=...))
			//   member_parent  -> external class member    (VariableReference=(MemberParent=...,MemberName=...))
			//   target_class   -> accepted alias of member_parent
			//   neither        -> resolved in order: Blueprint member (self-context) ->
			//                     function-local variable of the session graph ->
			//                     function input parameter of the session graph
			FString MemberScope, MemberParent;
			Params->TryGetStringField(TEXT("member_scope"), MemberScope);
			if (!Params->TryGetStringField(TEXT("member_parent"), MemberParent))
			{
				Params->TryGetStringField(TEXT("target_class"), MemberParent);
			}

			UK2Node_Variable* N = bIsGet
				? static_cast<UK2Node_Variable*>(NewObject<UK2Node_VariableGet>(Graph))
				: static_cast<UK2Node_Variable*>(NewObject<UK2Node_VariableSet>(Graph));

			if (!MemberScope.IsEmpty())
			{
				// Function-local variable: resolve the declaring function graph by name
				// and validate the local exists on its entry node. Failing loud here is
				// the fix for the silent-unbound-node FAILED_OP.
				UEdGraph* ScopeGraph = nullptr;
				for (UEdGraph* G : Blueprint->FunctionGraphs)
				{
					if (IsValid(G) && (G->GetName() == MemberScope || FName(*G->GetName()) == FName(*MemberScope)))
					{
						ScopeGraph = G;
						break;
					}
				}
				if (!IsValid(ScopeGraph))
				{
					TArray<FString> Avail;
					for (UEdGraph* G : Blueprint->FunctionGraphs) { if (IsValid(G)) Avail.Add(G->GetName()); }
					Out.Error = FString::Printf(
						TEXT("%s: member_scope '%s' does not match any function graph. Available: %s"),
						*NodeType, *MemberScope, *FString::Join(Avail, TEXT(", ")));
					return Out;
				}

				UK2Node_FunctionEntry* Entry = nullptr;
				for (UEdGraphNode* GraphNode : ScopeGraph->Nodes)
				{
					if (UK2Node_FunctionEntry* AsEntry = Cast<UK2Node_FunctionEntry>(GraphNode); IsValid(AsEntry))
					{
						Entry = AsEntry;
						break;
					}
				}
				const FBPVariableDescription* LocalDesc = nullptr;
				bool bIsParameter = false;
				if (IsValid(Entry))
				{
					for (const FBPVariableDescription& Local : Entry->LocalVariables)
					{
						if (Local.VarName == VarFName)
						{
							LocalDesc = &Local;
							break;
						}
					}
					// Function PARAMETERS are also referenced as local-scope members
					// (VariableReference=(MemberScope=<fn>,MemberName=<param>), no guid).
					if (!LocalDesc)
					{
						for (UEdGraphPin* EntryPin : Entry->Pins)
						{
							if (EntryPin && EntryPin->Direction == EGPD_Output
								&& EntryPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
								&& EntryPin->PinName == VarFName)
							{
								bIsParameter = true;
								break;
							}
						}
					}
				}
				if (!LocalDesc && !bIsParameter)
				{
					TArray<FString> Avail;
					if (IsValid(Entry))
					{
						for (const FBPVariableDescription& Local : Entry->LocalVariables) { Avail.Add(Local.VarName.ToString()); }
						for (UEdGraphPin* EntryPin : Entry->Pins)
						{
							if (EntryPin && EntryPin->Direction == EGPD_Output
								&& EntryPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
							{
								Avail.Add(EntryPin->PinName.ToString() + TEXT(" (param)"));
							}
						}
					}
					Out.Error = FString::Printf(
						TEXT("%s: local variable '%s' not found in function '%s'. Available locals/params: %s. Use bp_add_local_variable to declare a new local."),
						*NodeType, *VariableName, *MemberScope, *FString::Join(Avail, TEXT(", ")));
					return Out;
				}
				N->VariableReference.SetLocalMember(VarFName, ScopeGraph->GetName(), LocalDesc ? LocalDesc->VarGuid : FGuid());
				bVariableBoundAsParameter = bIsParameter;
				VariableBindScopeName = ScopeGraph->GetName();
				Desc = FString::Printf(TEXT("%s %s (local in %s)"), bIsGet ? TEXT("Get") : TEXT("Set"), *VariableName, *MemberScope);
			}
			else if (!MemberParent.IsEmpty())
			{
				// External member: resolve the owning class and validate the property
				// exists on it before binding.
				ClaireonNameResolver::FNameResolveResult R;
				UClass* OwnerClass = ClaireonNameResolver::ResolveClassName(MemberParent, nullptr, R);
				if (!IsValid(OwnerClass))
				{
					Out.Error = FString::Printf(
						TEXT("%s: member_parent '%s' could not be resolved to a class. (%s)"),
						*NodeType, *MemberParent, *R.Error);
					return Out;
				}
				if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);

				FProperty* Prop = FindFProperty<FProperty>(OwnerClass, VarFName);
				if (!Prop)
				{
					Out.Error = FString::Printf(
						TEXT("%s: property '%s' not found on class '%s'"),
						*NodeType, *VariableName, *OwnerClass->GetName());
					return Out;
				}
				N->VariableReference.SetExternalMember(VarFName, OwnerClass);
				Desc = FString::Printf(TEXT("%s %s.%s"), bIsGet ? TEXT("Get") : TEXT("Set"), *OwnerClass->GetName(), *VariableName);
			}
			else
			{
				// No explicit scope given. Resolution order (WI-2): member variable on
				// the Blueprint -> function-local variable of the session graph ->
				// function input parameter of the session graph. Previously this branch
				// called SetSelfMember unconditionally, so a bare variable_name naming a
				// local or parameter produced a pinless unbound node with a success
				// message ("Added node: Get X") that only failed at connect/compile time.
				bool bResolvesAsMember = false;
				if (IsValid(Blueprint->SkeletonGeneratedClass) && FindFProperty<FProperty>(Blueprint->SkeletonGeneratedClass, VarFName))
				{
					bResolvesAsMember = true;
				}
				if (!bResolvesAsMember && IsValid(Blueprint->GeneratedClass) && FindFProperty<FProperty>(Blueprint->GeneratedClass, VarFName))
				{
					bResolvesAsMember = true;
				}
				if (!bResolvesAsMember)
				{
					for (const FBPVariableDescription& Var : Blueprint->NewVariables)
					{
						if (Var.VarName == VarFName) { bResolvesAsMember = true; break; }
					}
				}

				if (bResolvesAsMember)
				{
					N->VariableReference.SetSelfMember(VarFName);
					Desc = FString::Printf(TEXT("%s %s"), bIsGet ? TEXT("Get") : TEXT("Set"), *VariableName);
				}
				else
				{
					// Not a member: fall back to the SESSION graph's local scope when it
					// is a function graph (has a K2Node_FunctionEntry).
					// TODO(WI-1): document this implicit member/local/parameter fallback
					// in bp_add_node's input schema (ClaireonBlueprintGraphTool_AddNode.cpp
					// is owned by WI-1 and cannot be edited from this work item).
					UK2Node_FunctionEntry* SessionEntry = nullptr;
					for (UEdGraphNode* GraphNode : Graph->Nodes)
					{
						if (UK2Node_FunctionEntry* AsEntry = Cast<UK2Node_FunctionEntry>(GraphNode); IsValid(AsEntry))
						{
							SessionEntry = AsEntry;
							break;
						}
					}

					const FBPVariableDescription* SessionLocalDesc = nullptr;
					bool bSessionParam = false;
					if (IsValid(SessionEntry))
					{
						for (const FBPVariableDescription& Local : SessionEntry->LocalVariables)
						{
							if (Local.VarName == VarFName)
							{
								SessionLocalDesc = &Local;
								break;
							}
						}
						if (!SessionLocalDesc)
						{
							for (UEdGraphPin* EntryPin : SessionEntry->Pins)
							{
								if (EntryPin && EntryPin->Direction == EGPD_Output
									&& EntryPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec
									&& EntryPin->PinName == VarFName)
								{
									bSessionParam = true;
									break;
								}
							}
						}
					}

					if (SessionLocalDesc)
					{
						N->VariableReference.SetLocalMember(VarFName, Graph->GetName(), SessionLocalDesc->VarGuid);
						VariableBindScopeName = Graph->GetName();
						Desc = FString::Printf(TEXT("%s %s (local in %s)"), bIsGet ? TEXT("Get") : TEXT("Set"), *VariableName, *Graph->GetName());
					}
					else if (bSessionParam)
					{
						// Function input parameters are referenced as local-scope members
						// with no guid. Whether this yields a usable node is verified by
						// the post-AllocateDefaultPins validation in the shared tail; on
						// failure the node is removed and the parameter-specific error is
						// returned instead of a false success.
						N->VariableReference.SetLocalMember(VarFName, Graph->GetName(), FGuid());
						bVariableBoundAsParameter = true;
						VariableBindScopeName = Graph->GetName();
						Desc = FString::Printf(TEXT("%s %s (parameter of %s)"), bIsGet ? TEXT("Get") : TEXT("Set"), *VariableName, *Graph->GetName());
					}
					else
					{
						Out.Error = FString::Printf(
							TEXT("%s: variable '%s' not found in member/local/parameter scope of '%s'. ")
							TEXT("Checked Blueprint '%s' members (skeleton class, generated class, pending NewVariables)%s. ")
							TEXT("For a member on another class pass member_parent; for a local in a different function pass member_scope; ")
							TEXT("use bp_add_local_variable to declare a new local."),
							*NodeType, *VariableName, *Graph->GetName(), *Blueprint->GetName(),
							IsValid(SessionEntry)
								? TEXT(", the graph's local variables, and its input parameters")
								: TEXT("; the graph has no function entry node, so it has no local/parameter scope"));
						return Out;
					}
				}
			}
			// Validated (impure) get: execute/then/else exec pins instead of a pure
			// value tap. Set BEFORE AllocateDefaultPins so the exec pins allocate.
			bool bValidatedGet = false;
			if (bIsGet && Params->TryGetBoolField(TEXT("validated"), bValidatedGet) && bValidatedGet)
			{
				if (UK2Node_VariableGet* GetNode = Cast<UK2Node_VariableGet>(N); IsValid(GetNode))
				{
#if UE_VERSION_OLDER_THAN(5, 7, 0)
					GetNode->SetPurity(false);
#else
					// UE 5.7 switched UK2Node_VariableGet to UCLASS(MinimalAPI) with per-symbol
					// UE_API exports and left SetPurity unexported (TogglePurity and
					// CurrentVariation are private). CurrentVariation is still a UPROPERTY, so set
					// it reflectively to the validated (impure) variation before
					// AllocateDefaultPins reads it to allocate exec pins. CreateImpurePins resets to
					// Pure if the graph doesn't support impure gets, so this is self-correcting.
					if (FProperty* VarProp = GetNode->GetClass()->FindPropertyByName(TEXT("CurrentVariation")))
					{
						if (FEnumProperty* EnumProp = CastField<FEnumProperty>(VarProp))
						{
							void* ValuePtr = EnumProp->ContainerPtrToValuePtr<void>(GetNode);
							EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(
								ValuePtr, static_cast<int64>(EGetNodeVariation::ValidatedObject));
						}
					}
#endif
					Desc += TEXT(" [validated]");
				}
			}

			// Typed pin created directly from FProperty lookup during AllocateDefaultPins; ReconstructNode not required.
			NewNode = N;
		}
		else if (NodeType == TEXT("Branch"))
		{
			NewNode = NewObject<UK2Node_IfThenElse>(Graph);
			Desc = TEXT("Branch");
		}
		else if (NodeType == TEXT("Sequence") || NodeType == TEXT("ExecutionSequence"))
		{
			// "ExecutionSequence" accepted as an alias: it is the canonical TypeTag
			// emitted by ClaireonTool_BlueprintDiff, so round-tripping diff output
			// through apply_delta must accept either spelling.
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
			if (!IsValid(C)) { Out.Error = R.Error; return Out; }
			if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);

			// TargetType drives the As* output pin; ReconstructNode finalizes via PostReconstructNode.
			UK2Node_DynamicCast* N = NewObject<UK2Node_DynamicCast>(Graph);
			N->TargetType = C;
			// Pure cast: no exec pins / bSuccess output. Must be set BEFORE
			// AllocateDefaultPins; flipping bIsPureCast on a placed node leaves
			// a hybrid pin set behind.
			bool bPureCast = false;
			if (Params->TryGetBoolField(TEXT("pure"), bPureCast) && bPureCast)
			{
				N->SetPurity(true);
			}
			NewNode = N;
			Desc = FString::Printf(TEXT("Cast to %s%s"), *TargetClass, bPureCast ? TEXT(" [pure]") : TEXT(""));
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
			if (!IsValid(C)) { Out.Error = R.Error; return Out; }
			if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);

			UK2Node_SpawnActorFromClass* SpawnNode = NewObject<UK2Node_SpawnActorFromClass>(Graph);
			NewNode = SpawnNode;
			// The resolved class was previously dropped on the floor: the node
			// authored with an empty Class pin ('Spawn node must have a Class
			// specified') unless a later pin-default write happened to set it.
			PostAllocate = [SpawnNode, C]()
			{
				if (UEdGraphPin* ClassPin = SpawnNode->FindPin(TEXT("Class")))
				{
					// TrySetDefaultObject routes through PinDefaultValueChanged so
					// the node rebuilds its exposed/result pins for the class.
					GetDefault<UEdGraphSchema_K2>()->TrySetDefaultObject(*ClassPin, C);
				}
			};
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
			if (!IsValid(S)) { Out.Error = R.Error; return Out; }
			if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);

			// Pins derive from the UScriptStruct member list; ReconstructNode required.
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
			if (!IsValid(S)) { Out.Error = R.Error; return Out; }
			if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);

			// Pins derive from the UScriptStruct member list; ReconstructNode required.
			UK2Node_BreakStruct* N = NewObject<UK2Node_BreakStruct>(Graph);
			N->StructType = S;
			NewNode = N;
			Desc = FString::Printf(TEXT("Break %s"), *StructType);
		}
		else if (NodeType == TEXT("SetFieldsInStruct"))
		{
			FString StructType;
			if (!Params->TryGetStringField(TEXT("struct_type"), StructType))
			{
				Out.Error = TEXT("SetFieldsInStruct: missing required field 'struct_type'");
				return Out;
			}
			ClaireonNameResolver::FNameResolveResult R;
			UScriptStruct* S = ClaireonNameResolver::ResolveStructName(StructType, R);
			if (!IsValid(S)) { Out.Error = R.Error; return Out; }
			if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);

			// Pins derive from the UScriptStruct member list; ReconstructNode required
			// (covered by the IsA<UK2Node_MakeStruct> reconstruct check below).
			UK2Node_SetFieldsInStruct* N = NewObject<UK2Node_SetFieldsInStruct>(Graph);
			N->StructType = S;
			NewNode = N;
			Desc = FString::Printf(TEXT("Set members in %s"), *StructType);
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
			if (!IsValid(E)) { Out.Error = R.Error; return Out; }
			if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);

			// Pins regenerate from EnumEntries; ReconstructNode required after Enum/EnumEntries assignment.
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
			if (!IsValid(E)) { Out.Error = R.Error; return Out; }
			if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);

			// Loop pin shape derives from Enum member count; ReconstructNode required.
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
		else if (NodeType == TEXT("MultiGate"))
		{
			// MultiGate is a native node (UK2Node_MultiGate), not a StandardMacros macro.
			NewNode = NewObject<UK2Node_MultiGate>(Graph);
			Desc = TEXT("MultiGate");
		}
		else if (NodeType == TEXT("FormatText"))
		{
			// Argument pins regenerate from the Format pin's default text; the optional
			// 'format_text' param is applied post-insert (see the post-insert hook below)
			// so PinDefaultValueChanged can synthesize the argument pins.
			NewNode = NewObject<UK2Node_FormatText>(Graph);
			Desc = TEXT("Format Text");
		}
		else if (NodeType == TEXT("SwitchGameplayTag"))
		{
			FString TagError;
			TArray<FGameplayTag> ParsedTags;
			const TArray<TSharedPtr<FJsonValue>>* TagsArray = nullptr;
			if (Params->TryGetArrayField(TEXT("tags"), TagsArray) && TagsArray)
			{
				for (const TSharedPtr<FJsonValue>& TagVal : *TagsArray)
				{
					const FString TagString = TagVal->AsString();
					const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*TagString), /*ErrorIfNotFound=*/false);
					if (!Tag.IsValid())
					{
						Out.Error = FString::Printf(TEXT("SwitchGameplayTag: tag '%s' is not a registered gameplay tag"), *TagString);
						return Out;
					}
					ParsedTags.Add(Tag);
				}
			}
			// Case pins derive from PinTags inside AllocateDefaultPins (CreateCasePins);
			// set them before the shared insert tail runs.
			UGameplayTagsK2Node_SwitchGameplayTag* N = NewObject<UGameplayTagsK2Node_SwitchGameplayTag>(Graph);
			N->PinTags = ParsedTags;
			NewNode = N;
			Desc = FString::Printf(TEXT("Switch on Gameplay Tag (%d cases)"), ParsedTags.Num());
		}
		else if (NodeType == TEXT("Composite") || NodeType == TEXT("CollapsedGraph"))
		{
			// Collapsed-graph node. PostPlacedNewNode creates the bound subgraph with
			// entry/exit tunnels and registers it on the parent graph's SubGraphs, so the
			// node must be inserted here rather than by the shared tail.
			UK2Node_Composite* N = NewObject<UK2Node_Composite>(Graph);
			N->NodePosX = static_cast<int32>(Position.X);
			N->NodePosY = static_cast<int32>(Position.Y);
			N->CreateNewGuid();
			N->SetFlags(RF_Transactional);
			Graph->AddNode(N, false, false);
			N->PostPlacedNewNode();
			FString SubgraphName;
			if (Params->TryGetStringField(TEXT("graph_name"), SubgraphName) && !SubgraphName.IsEmpty() && N->BoundGraph)
			{
				FBlueprintEditorUtils::RenameGraph(N->BoundGraph, SubgraphName);
			}
			N->AllocateDefaultPins();
			bSelfInserted = true;
			NewNode = N;
			Desc = FString::Printf(TEXT("Collapsed Graph: %s"), N->BoundGraph ? *N->BoundGraph->GetName() : TEXT("<unnamed>"));
		}
		else if (NodeType == TEXT("Macro") || NodeType == TEXT("MacroInstance")
			|| NodeType == TEXT("ForEachLoop") || NodeType == TEXT("ForEachLoopWithBreak")
			|| NodeType == TEXT("ForLoop") || NodeType == TEXT("ForLoopWithBreak") || NodeType == TEXT("WhileLoop")
			|| NodeType == TEXT("DoOnce") || NodeType == TEXT("DoN") || NodeType == TEXT("FlipFlop")
			|| NodeType == TEXT("Gate") || NodeType == TEXT("IsValid") || NodeType == TEXT("SwitchHasAuthority"))
		{
			FString MacroName;
			FString MacroLibraryPath = TEXT("/Engine/EditorBlueprintResources/StandardMacros");

			if (NodeType == TEXT("Macro") || NodeType == TEXT("MacroInstance"))
			{
				if (!Params->TryGetStringField(TEXT("macro_name"), MacroName))
				{
					Out.Error = FString::Printf(TEXT("%s: missing required field 'macro_name'"), *NodeType);
					return Out;
				}
				// Both spellings accepted: the input schema advertises 'macro_library',
				// the original factory read 'macro_library_path'.
				FString Custom;
				if (Params->TryGetStringField(TEXT("macro_library_path"), Custom)
					|| Params->TryGetStringField(TEXT("macro_library"), Custom))
				{
					MacroLibraryPath = Custom;
				}
			}
			else if (NodeType == TEXT("SwitchHasAuthority"))
			{
				// 'Switch Has Authority' lives in ActorMacros, not StandardMacros.
				MacroName = TEXT("Switch Has Authority");
				MacroLibraryPath = TEXT("/Engine/EditorBlueprintResources/ActorMacros");
			}
			else
			{
				// Shorthand aliases whose graph name differs from the alias are mapped in
				// ClaireonMacroShorthand::ResolveIfShorthand, which rewrites node_type to
				// MacroInstance and supplies macro_name before this factory runs. Do not
				// add per-alias cases here -- they would be unreachable for anything the
				// shorthand resolver already handles.
				MacroName = NodeType;
			}

			UBlueprint* Lib = LoadObject<UBlueprint>(nullptr, *MacroLibraryPath);
			if (!IsValid(Lib))
			{
				Out.Error = FString::Printf(TEXT("Failed to load macro library: %s"), *MacroLibraryPath);
				return Out;
			}

			UEdGraph* MacroGraph = nullptr;
			for (UEdGraph* G : Lib->MacroGraphs)
			{
				if (IsValid(G) && G->GetName() == MacroName) { MacroGraph = G; break; }
			}
			if (!IsValid(MacroGraph))
			{
				TArray<FString> Avail;
				for (UEdGraph* G : Lib->MacroGraphs) { if (IsValid(G)) Avail.Add(G->GetName()); }
				Out.Error = FString::Printf(TEXT("Macro '%s' not found in %s. Available: %s"),
					*MacroName, *MacroLibraryPath, *FString::Join(Avail, TEXT(", ")));
				return Out;
			}

			// Macro-graph pin set is resolved from SetMacroGraph; ReconstructNode required.
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
			if (!IsValid(C)) { Out.Error = R.Error; return Out; }
			if (!R.ResolutionNote.IsEmpty()) Out.Warnings.Add(R.ResolutionNote);

			if (!C->IsChildOf(UEdGraphNode::StaticClass()))
			{
				Out.Error = FString::Printf(TEXT("Class '%s' is not a graph-node class"), *ClassName);
				return Out;
			}

			// Anim graph nodes require a UAnimBlueprint host: UAnimGraphNode_Base::
			// GetAnimBlueprint() CastChecked-fatals when the owning Blueprint is any
			// other class, taking the editor down with it.
			if (C->IsChildOf(UAnimGraphNode_Base::StaticClass()) && !Blueprint->IsA<UAnimBlueprint>())
			{
				Out.Error = FString::Printf(
					TEXT("Generic '%s': anim graph nodes can only be authored into an Anim Blueprint; '%s' is a %s. Create the target via animbp_create and author with the anim_graph_* session tools."),
					*ClassName, *Blueprint->GetName(), *Blueprint->GetClass()->GetName());
				return Out;
			}

			NewNode = NewObject<UEdGraphNode>(Graph, C);
			Desc = FString::Printf(TEXT("Generic: %s"), *ClassName);

			// Async-task subclasses (e.g. project K2Node_FSFlowprintAwait_*) accept
			// function_name + function_class and get their proxy fields initialized
			// from the factory UFUNCTION -- the same data InitializeProxyFromFunction
			// derives. Without this, only the raw node_properties fallback exists.
			FString AsyncFunctionName, AsyncFunctionClass;
			if (NewNode->IsA<UK2Node_BaseAsyncTask>()
				&& Params->TryGetStringField(TEXT("function_name"), AsyncFunctionName)
				&& Params->TryGetStringField(TEXT("function_class"), AsyncFunctionClass))
			{
				ClaireonNameResolver::FNameResolveResult AsyncR;
				UClass* AsyncOwner = ClaireonNameResolver::ResolveClassName(AsyncFunctionClass, nullptr, AsyncR);
				if (!IsValid(AsyncOwner))
				{
					Out.Error = FString::Printf(
						TEXT("Generic '%s': function_class '%s' could not be resolved. (%s)"),
						*ClassName, *AsyncFunctionClass, *AsyncR.Error);
					return Out;
				}
				if (!AsyncR.ResolutionNote.IsEmpty()) Out.Warnings.Add(AsyncR.ResolutionNote);

				// Resolve through the name resolver so K2_ prefixes, case drift, and
				// ini FunctionRedirects (stale serialized factory names after C++
				// renames) all apply.
				ClaireonNameResolver::FNameResolveResult AsyncFnResult;
				UFunction* AsyncFactoryFn = ClaireonNameResolver::ResolveFunctionName(AsyncOwner, AsyncFunctionName, AsyncFnResult);
				if (!IsValid(AsyncFactoryFn))
				{
					Out.Error = FString::Printf(
						TEXT("Generic '%s': factory function '%s' not found on class '%s' (%s)"),
						*ClassName, *AsyncFunctionName, *AsyncOwner->GetName(), *AsyncFnResult.Error);
					return Out;
				}
				if (!AsyncFnResult.ResolutionNote.IsEmpty()) Out.Warnings.Add(AsyncFnResult.ResolutionNote);

				if (UK2Node_AsyncAction* AsAsyncAction = Cast<UK2Node_AsyncAction>(NewNode); IsValid(AsAsyncAction))
				{
					AsAsyncAction->InitializeProxyFromFunction(AsyncFactoryFn);
				}
				else
				{
					// Non-UK2Node_AsyncAction BaseAsyncTask subclass: write the three
					// protected proxy fields via reflection (same idiom as the guard
					// below reads them).
					auto WriteObjProp = [&](const TCHAR* PropName, UObject* Value)
					{
						if (const FObjectPropertyBase* P = CastField<FObjectPropertyBase>(
							NewNode->GetClass()->FindPropertyByName(PropName)))
						{
							P->SetObjectPropertyValue_InContainer(NewNode, Value);
						}
					};
					auto WriteNameProp = [&](const TCHAR* PropName, FName Value)
					{
						if (const FNameProperty* P = CastField<FNameProperty>(
							NewNode->GetClass()->FindPropertyByName(PropName)))
						{
							P->SetPropertyValue_InContainer(NewNode, Value);
						}
					};
					UClass* ProxyReturnClass = nullptr;
					if (const FObjectProperty* ReturnProp = CastField<FObjectProperty>(AsyncFactoryFn->GetReturnProperty()))
					{
						ProxyReturnClass = ReturnProp->PropertyClass;
					}
					WriteObjProp(TEXT("ProxyFactoryClass"), AsyncOwner);
					WriteObjProp(TEXT("ProxyClass"), IsValid(ProxyReturnClass) ? ProxyReturnClass : AsyncOwner);
					WriteNameProp(TEXT("ProxyFactoryFunctionName"), AsyncFactoryFn->GetFName());
				}
				Desc = FString::Printf(TEXT("Generic async: %s (%s::%s)"), *ClassName, *AsyncOwner->GetName(), *AsyncFunctionName);
			}

			// Typed identity params for input event nodes (authored via the Generic
			// path since their classes live in the EnhancedInput plugin):
			//   input_action=<asset path> -> UInputAction 'InputAction' property
			//     (K2Node_EnhancedInputAction / K2Node_GetInputActionValue)
			//   key=<FKey name>           -> FKey 'InputKey' property
			//     (K2Node_InputDebugKey / K2Node_InputKey)
			// Applied before AllocateDefaultPins so the pins derive correctly.
			FString InputActionPath;
			if (Params->TryGetStringField(TEXT("input_action"), InputActionPath) && !InputActionPath.IsEmpty())
			{
				FObjectPropertyBase* ActionProp = CastField<FObjectPropertyBase>(
					NewNode->GetClass()->FindPropertyByName(TEXT("InputAction")));
				if (!ActionProp)
				{
					Out.Error = FString::Printf(
						TEXT("Generic '%s': input_action supplied but the node class has no 'InputAction' property"),
						*ClassName);
					return Out;
				}
				UObject* ActionAsset = LoadObject<UObject>(nullptr, *InputActionPath);
				if (!IsValid(ActionAsset) && !InputActionPath.Contains(TEXT(".")))
				{
					// Accept package-path shorthand /Game/Dir/IA_Foo
					const FString ObjectPath = InputActionPath + TEXT(".") + FPackageName::GetShortName(InputActionPath);
					ActionAsset = LoadObject<UObject>(nullptr, *ObjectPath);
				}
				if (!IsValid(ActionAsset) || (ActionProp->PropertyClass && !ActionAsset->IsA(ActionProp->PropertyClass)))
				{
					Out.Error = FString::Printf(
						TEXT("Generic '%s': could not load InputAction asset '%s' (expected %s)"),
						*ClassName, *InputActionPath,
						ActionProp->PropertyClass ? *ActionProp->PropertyClass->GetName() : TEXT("UInputAction"));
					return Out;
				}
				ActionProp->SetObjectPropertyValue(ActionProp->ContainerPtrToValuePtr<void>(NewNode), ActionAsset);
				Desc += FString::Printf(TEXT(" [input_action=%s]"), *ActionAsset->GetName());
			}

			FString KeyName;
			if (Params->TryGetStringField(TEXT("key"), KeyName) && !KeyName.IsEmpty())
			{
				FStructProperty* KeyProp = CastField<FStructProperty>(
					NewNode->GetClass()->FindPropertyByName(TEXT("InputKey")));
				if (!KeyProp)
				{
					KeyProp = CastField<FStructProperty>(
						NewNode->GetClass()->FindPropertyByName(TEXT("Key")));
				}
				if (!KeyProp)
				{
					Out.Error = FString::Printf(
						TEXT("Generic '%s': key supplied but the node class has no 'InputKey'/'Key' struct property"),
						*ClassName);
					return Out;
				}
				void* KeyValuePtr = KeyProp->ContainerPtrToValuePtr<void>(NewNode);
				if (KeyProp->ImportText_Direct(*KeyName, KeyValuePtr, NewNode, PPF_None) == nullptr)
				{
					Out.Error = FString::Printf(
						TEXT("Generic '%s': '%s' is not a valid key name for %s"),
						*ClassName, *KeyName, *KeyProp->Struct->GetName());
					return Out;
				}
				Desc += FString::Printf(TEXT(" [key=%s]"), *KeyName);
			}
		}
		else
		{
			Out.Error = FString::Printf(
				TEXT("Unsupported node_type '%s' in factory. Use 'Generic' with 'class_name' for custom types, "
				     "or bp_add_node for typed support not yet in the factory "
				     "(SpawnActor variants, Delegate nodes, Timeline, EventOverride)."), *NodeType);
			return Out;
		}

		if (!IsValid(NewNode))
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

		// Loud-failure guard for Generic + UK2Node_BaseAsyncTask subclasses
		// (e.g. K2Node_AsyncAction, K2Node_LatentAbilityCall) constructed
		// without the proxy bag. The engine's AllocateDefaultPins synthesizes
		// the BlueprintAssignable delegate exec pins from ProxyFactoryClass +
		// ProxyFactoryFunctionName + ProxyClass; if any are unset (via
		// node_properties), the result is the inert "Async Task: Missing
		// Function" stub.
		//
		// The three proxy fields are protected on UK2Node_BaseAsyncTask with no
		// public accessors, so we read them via the same reflection idiom used in
		// ClaireonBPNodeMapper.cpp:2155-2179 (MapAsyncActionNode).
		if (IsValid(NewNode) && NewNode->IsA<UK2Node_BaseAsyncTask>())
		{
			auto ReadObjProp = [&](const TCHAR* PropName) -> UClass*
			{
				if (const FObjectPropertyBase* P = CastField<FObjectPropertyBase>(
					NewNode->GetClass()->FindPropertyByName(PropName)))
				{
					return Cast<UClass>(P->GetObjectPropertyValue_InContainer(NewNode));
				}
				return nullptr;
			};
			auto ReadNameProp = [&](const TCHAR* PropName) -> FName
			{
				if (const FNameProperty* P = CastField<FNameProperty>(
					NewNode->GetClass()->FindPropertyByName(PropName)))
				{
					return P->GetPropertyValue_InContainer(NewNode);
				}
				return NAME_None;
			};

			UClass* PFC      = ReadObjProp(TEXT("ProxyFactoryClass"));
			UClass* PC       = ReadObjProp(TEXT("ProxyClass"));
			const FName PFF  = ReadNameProp(TEXT("ProxyFactoryFunctionName"));

			if (PFC == nullptr || PC == nullptr || PFF.IsNone())
			{
				Out.Error = FString::Printf(
					TEXT("Generic '%s' was created without proxy fields populated -- ")
					TEXT("the node would render as 'Async Task: Missing Function' with no delegate pins. ")
					TEXT("Use `node_type='AsyncAction'` with `function_name` and `function_class` (recommended). ")
					TEXT("As a fallback, supply `node_properties` containing 'ProxyFactoryFunctionName', 'ProxyFactoryClass', and 'ProxyClass'."),
					*NewNode->GetClass()->GetName());
				return Out;
			}
		}

		// -------- Add to graph + AllocateDefaultPins --------
		if (!bSelfInserted)
		{
			NewNode->NodePosX = static_cast<int32>(Position.X);
			NewNode->NodePosY = static_cast<int32>(Position.Y);
			NewNode->CreateNewGuid();
			NewNode->SetFlags(RF_Transactional);
			Graph->AddNode(NewNode, false, false);
			NewNode->AllocateDefaultPins();
		}

		if (PostAllocate)
		{
			PostAllocate();
		}

		// -------- Variable-node bind validation (WI-2) --------
		// A VariableGet/VariableSet whose reference failed to resolve allocates NO
		// value pin (UK2Node_Variable::CreatePinForVariable bails when
		// GetPropertyForVariable returns null), leaving a pinless node that only
		// fails at connect/compile time. Never report success for one: remove it
		// from the graph and fail loudly instead.
		// UK2Node_StructOperation (MakeStruct/BreakStruct/SetFieldsInStruct) derives
		// from UK2Node_Variable but binds a StructType, not a VariableReference --
		// GetVarName() is None by design. Applying the variable-bind validation to
		// them deleted every struct node (iteration-5 finding).
		if (UK2Node_Variable* BoundVarNode = NewNode->IsA<UK2Node_StructOperation>() ? nullptr : Cast<UK2Node_Variable>(NewNode); IsValid(BoundVarNode))
		{
			const FName BoundVarName = BoundVarNode->GetVarName();
			FProperty* BoundVarProperty = BoundVarNode->GetPropertyForVariable();
			UEdGraphPin* BoundValuePin = BoundVarNode->FindPin(BoundVarName);
			if (!BoundVarProperty || !BoundValuePin)
			{
				FBlueprintEditorUtils::RemoveNode(Blueprint, BoundVarNode, /*bDontRecompile=*/true);
				const FString ScopeDesc = VariableBindScopeName.IsEmpty() ? Graph->GetName() : VariableBindScopeName;
				if (bVariableBoundAsParameter)
				{
					Out.Error = FString::Printf(
						TEXT("%s: '%s' is a function input parameter of '%s' and could not be bound as a variable node ")
						TEXT("(%s after AllocateDefaultPins). The node was removed. ")
						TEXT("Read the parameter by wiring from its output pin on the function's K2Node_FunctionEntry node instead."),
						*NodeType, *BoundVarName.ToString(), *ScopeDesc,
						!BoundVarProperty ? TEXT("no resolved FProperty") : TEXT("no value pin"));
				}
				else
				{
					Out.Error = FString::Printf(
						TEXT("%s: variable '%s' not found in member/local/parameter scope of '%s'. ")
						TEXT("The node did not bind after AllocateDefaultPins (%s) and was removed from the graph."),
						*NodeType, *BoundVarName.ToString(), *ScopeDesc,
						!BoundVarProperty ? TEXT("no resolved FProperty") : TEXT("no value pin"));
				}
				return Out;
			}
		}

		// -------- Function-call bind validation (P1-9d) --------
		// Same failure shape as the variable guard above, one class over. The
		// factory resolves the UFunction loudly up front, but a FunctionReference
		// can still fail to bind during AllocateDefaultPins (skeleton class not yet
		// regenerated, or a generated-class/skeleton-class mismatch), and the guard
		// above is scoped to UK2Node_Variable -- so a dead zero-pin call node was
		// committed to the graph with a success message.
		//
		// The invariant is the PIN COUNT, not GetTargetFunction(). UK2Node_CallFunction
		// ::AllocateDefaultPins creates pins if and only if it ended up with a
		// function, but it can find that function on the skeleton class without
		// writing it back to the reference -- so GetTargetFunction() returning null
		// while pins exist is a legitimate, working state, and testing it would
		// delete good nodes.
		if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(NewNode); IsValid(CallNode))
		{
			if (CallNode->Pins.Num() == 0)
			{
				const FString TargetName = CallNode->FunctionReference.GetMemberName().ToString();
				FBlueprintEditorUtils::RemoveNode(Blueprint, CallNode, /*bDontRecompile=*/true);
				Out.Error = FString::Printf(
					TEXT("%s: function '%s' did not bind after AllocateDefaultPins -- no pins were allocated, ")
					TEXT("so the node would be dead. It was removed from the graph. This usually means the ")
					TEXT("owning class has not been compiled since the function was added; compile the Blueprint and retry."),
					*NodeType,
					TargetName.IsEmpty() ? TEXT("<unnamed>") : *TargetName);
				return Out;
			}
		}

		// Dynamic-pin nodes regenerate their real pin set in ReconstructNode.
		// Fire ReconstructNode either when reflection wrote properties OR when
		// the node is one of the typed branches whose pins are derived from a
		// resolved engine reference (UFunction, UScriptStruct, UEnum, macro graph,
		// target UClass). VariableGet/VariableSet are intentionally excluded:
		// AllocateDefaultPins already builds the pin from the FProperty lookup,
		// and ReconstructNode would be a no-op there. See T4 for per-branch
		// commentary.
		//
		// Some K2Node ReconstructNode overrides reset NodePosX/Y to 0 by
		// calling AllocateDefaultPins on a freshly-constructed inner node.
		// Snapshot + restore positions so get_state summary reports the authored
		// coordinates rather than (0, 0).
		// UK2Node_BaseAsyncTask intentionally omitted: AllocateDefaultPins is sufficient
		// for the AsyncAction family (it builds delegate exec pins and parameter pins from
		// the proxy fields populated by InitializeProxyFromFunction). The bWroteProperties
		// branch above still reconstructs when node_properties writes proxy fields directly.
		const bool bReconstructForTypedBranch =
			   NewNode->IsA<UK2Node_CallFunction>()          // includes CallArray / CallDataTable / CallMaterialParameterCollection / CommutativeAssociativeBinaryOperator subclasses
			|| NewNode->IsA<UK2Node_DynamicCast>()
			|| NewNode->IsA<UK2Node_MakeStruct>()
			|| NewNode->IsA<UK2Node_BreakStruct>()
			|| NewNode->IsA<UK2Node_SwitchEnum>()
			|| NewNode->IsA<UK2Node_ForEachElementInEnum>()
			|| NewNode->IsA<UK2Node_MacroInstance>();

		if (bWroteProperties || bReconstructForTypedBranch)
		{
			const int32 PrevX = NewNode->NodePosX;
			const int32 PrevY = NewNode->NodePosY;
			NewNode->ReconstructNode();
			NewNode->NodePosX = PrevX;
			NewNode->NodePosY = PrevY;
		}

		// -------- Dynamic pins --------
		ApplyExtraPins(NewNode, Params);

		// -------- Post-insert hooks --------
		// FormatText: applying the format string through the pin-changed path
		// synthesizes the {Argument} pins with their authored names.
		FString FormatTextValue;
		if (Params->TryGetStringField(TEXT("format_text"), FormatTextValue))
		{
			if (UK2Node_FormatText* FormatNode = Cast<UK2Node_FormatText>(NewNode); IsValid(FormatNode))
			{
				if (UEdGraphPin* FormatPin = FormatNode->FindPin(TEXT("Format")))
				{
					FormatPin->DefaultTextValue = FText::FromString(FormatTextValue);
					FormatNode->PinDefaultValueChanged(FormatPin);
				}
			}
			else
			{
				Out.Warnings.Add(TEXT("format_text supplied but node is not a FormatText node; ignored"));
			}
		}

		// CustomEvent: user-defined parameter pins ({name, type} objects). Same
		// idiom as add_function's parameter authoring.
		const TArray<TSharedPtr<FJsonValue>>* UserPinsArray = nullptr;
		if (Params->TryGetArrayField(TEXT("user_defined_pins"), UserPinsArray) && UserPinsArray)
		{
			if (UK2Node_CustomEvent* EventNode = Cast<UK2Node_CustomEvent>(NewNode); IsValid(EventNode))
			{
				for (const TSharedPtr<FJsonValue>& PinVal : *UserPinsArray)
				{
					const TSharedPtr<FJsonObject> PinObj = PinVal->AsObject();
					if (!PinObj.IsValid()) continue;
					FString PinName, PinTypeString;
					PinObj->TryGetStringField(TEXT("name"), PinName);
					PinObj->TryGetStringField(TEXT("type"), PinTypeString);
					if (PinName.IsEmpty() || PinTypeString.IsEmpty())
					{
						Out.Warnings.Add(TEXT("user_defined_pins entry missing 'name' or 'type'; skipped"));
						continue;
					}
					ClaireonBlueprintHelpers::FParseVariableTypeResult PinParse =
						ClaireonBlueprintHelpers::ParseVariableTypeChecked(PinTypeString);
					if (!PinParse.bSucceeded)
					{
						Out.Error = FString::Printf(
							TEXT("CustomEvent user_defined_pins: failed to parse type '%s' for pin '%s': %s"),
							*PinTypeString, *PinName, *PinParse.Error);
						return Out;
					}
					EventNode->CreateUserDefinedPin(FName(*PinName), PinParse.PinType, EGPD_Output);
				}
			}
			else
			{
				Out.Warnings.Add(TEXT("user_defined_pins supplied but node is not a CustomEvent; ignored"));
			}
		}

		Out.Node = NewNode;
		Out.Description = Desc;
		Out.bAlreadyAdded = true; // factory already added to graph
		return Out;
	}
}
