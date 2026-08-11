// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintGraphEditToolBase_Internal.h"
#include "ClaireonBlueprintHelpers.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallDataTableFunction.h"
#include "K2Node_CallMaterialParameterCollectionFunction.h"
#include "K2Node_CommutativeAssociativeBinaryOperator.h"
#include "K2Node_Event.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Timeline.h"
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
#include "K2Node_SetFieldsInStruct.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_Knot.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_Literal.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeMap.h"
#include "K2Node_MakeSet.h"
#include "K2Node_GetArrayItem.h"
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
#include "GameplayTagsK2Node_SwitchGameplayTag.h"
#include "ClaireonNameResolver.h"
#include "ClaireonBlueprintNodeTypeRegistry.h"
#include "Abilities/Tasks/AbilityTask.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_AssignDelegate.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Tunnel.h"
#include "Animation/AnimBlueprint.h"
#include "AnimationGraph.h"
#include "AnimGraphNode_Root.h"
#include "Dom/JsonObject.h"
#include "UObject/UObjectGlobals.h"

namespace ClaireonMacroShorthand
{
	// The shorthand list now lives in the shared node-type registry (Shorthand kind),
	// so bp_list_node_types and this rewrite cannot disagree about what exists.
	//
	// NOTE: MultiGate is intentionally absent -- it is a native node
	// (UK2Node_MultiGate), not a StandardMacros macro. SwitchHasAuthority is
	// absent because it lives in ActorMacros under the display name
	// "Switch Has Authority"; the factory handles that node_type directly.
	static const TCHAR* const GStandardMacroLibrary =
		TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros");

	static bool IsKnownMacroName(const FString& Name)
	{
		return ClaireonBlueprintNodeTypes::IsMacroShorthand(Name);
	}

	// Alias -> the macro graph's actual name.
	//
	// Nearly every shorthand is spelled exactly like its graph, so passing the alias
	// through was right often enough to look correct. "DoN" is the exception: the graph in
	// StandardMacros is named "Do N", with a space, so the lookup searched for a graph
	// that does not exist and bp_add_node could never create a Do N node -- even though
	// the registry advertised the alias and this resolver accepted it. Caught by the first
	// real execution of Claireon.EditBlueprintGraph.MacroShorthand.AllResolve.
	static FString MacroGraphNameForAlias(const FString& Alias)
	{
		if (Alias == TEXT("DoN"))
		{
			return TEXT("Do N");
		}
		return Alias;
	}

	void ResolveIfShorthand(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return;
		}

		FString NodeType;
		if (!Params->TryGetStringField(TEXT("node_type"), NodeType))
		{
			return;
		}
		if (!IsKnownMacroName(NodeType))
		{
			return;
		}
		if (Params->HasField(TEXT("function_class")))
		{
			return;
		}

		Params->SetStringField(TEXT("node_type"), TEXT("MacroInstance"));
		if (!Params->HasField(TEXT("macro_library")))
		{
			Params->SetStringField(TEXT("macro_library"), GStandardMacroLibrary);
		}
		if (!Params->HasField(TEXT("macro_name")))
		{
			Params->SetStringField(TEXT("macro_name"), MacroGraphNameForAlias(NodeType));
		}
	}
}

namespace ClaireonNodeTypeAlias
{
	static const TMap<const UClass*, FString>& GetAliasMap()
	{
		static const TMap<const UClass*, FString> Map = []() {
			TMap<const UClass*, FString> M;
			M.Emplace(UK2Node_CallFunction::StaticClass(),        TEXT("CallFunction"));
			M.Emplace(UK2Node_VariableGet::StaticClass(),         TEXT("VariableGet"));
			M.Emplace(UK2Node_VariableSet::StaticClass(),         TEXT("VariableSet"));
			M.Emplace(UK2Node_IfThenElse::StaticClass(),          TEXT("Branch"));
			M.Emplace(UK2Node_ExecutionSequence::StaticClass(),   TEXT("Sequence"));
			M.Emplace(UK2Node_DynamicCast::StaticClass(),         TEXT("Cast"));
			M.Emplace(UK2Node_SpawnActorFromClass::StaticClass(), TEXT("SpawnActor"));
			M.Emplace(UK2Node_CustomEvent::StaticClass(),         TEXT("CustomEvent"));
			M.Emplace(UK2Node_Knot::StaticClass(),                TEXT("Knot"));
			M.Emplace(UEdGraphNode_Comment::StaticClass(),        TEXT("Comment"));
			M.Emplace(UK2Node_Select::StaticClass(),              TEXT("Select"));
			M.Emplace(UK2Node_MakeArray::StaticClass(),           TEXT("MakeArray"));
			M.Emplace(UK2Node_MakeSet::StaticClass(),             TEXT("MakeSet"));
			M.Emplace(UK2Node_MakeMap::StaticClass(),             TEXT("MakeMap"));
			M.Emplace(UK2Node_GetArrayItem::StaticClass(),        TEXT("GetArrayItem"));
			M.Emplace(UK2Node_MakeStruct::StaticClass(),          TEXT("MakeStruct"));
			M.Emplace(UK2Node_BreakStruct::StaticClass(),         TEXT("BreakStruct"));
			M.Emplace(UK2Node_SetFieldsInStruct::StaticClass(),   TEXT("SetFieldsInStruct"));
			M.Emplace(UK2Node_SwitchInteger::StaticClass(),       TEXT("SwitchInteger"));
			M.Emplace(UK2Node_SwitchString::StaticClass(),        TEXT("SwitchString"));
			M.Emplace(UK2Node_SwitchName::StaticClass(),          TEXT("SwitchName"));
			M.Emplace(UK2Node_SwitchEnum::StaticClass(),          TEXT("SwitchEnum"));
			M.Emplace(UK2Node_ForEachElementInEnum::StaticClass(),TEXT("ForEachElementInEnum"));
			M.Emplace(UK2Node_DoOnceMultiInput::StaticClass(),    TEXT("DoOnceMultiInput"));
			M.Emplace(UK2Node_MultiGate::StaticClass(),           TEXT("MultiGate"));
			M.Emplace(UK2Node_FormatText::StaticClass(),          TEXT("FormatText"));
			M.Emplace(UK2Node_Composite::StaticClass(),           TEXT("Composite"));
			M.Emplace(UGameplayTagsK2Node_SwitchGameplayTag::StaticClass(), TEXT("SwitchGameplayTag"));
			M.Emplace(UK2Node_MacroInstance::StaticClass(),       TEXT("MacroInstance"));
			M.Emplace(UK2Node_Event::StaticClass(),               TEXT("EventOverride"));
			M.Emplace(UK2Node_CallParentFunction::StaticClass(),  TEXT("CallParentFunction"));
			M.Emplace(UK2Node_Timeline::StaticClass(),            TEXT("Timeline"));
			M.Emplace(UK2Node_AddDelegate::StaticClass(),         TEXT("AddDelegate"));
			M.Emplace(UK2Node_RemoveDelegate::StaticClass(),      TEXT("RemoveDelegate"));
			M.Emplace(UK2Node_ClearDelegate::StaticClass(),       TEXT("ClearDelegate"));
			M.Emplace(UK2Node_CallDelegate::StaticClass(),        TEXT("CallDelegate"));
			M.Emplace(UK2Node_CreateDelegate::StaticClass(),      TEXT("CreateDelegate"));
			M.Emplace(UK2Node_AssignDelegate::StaticClass(),      TEXT("AssignDelegate"));
			M.Emplace(UK2Node_ComponentBoundEvent::StaticClass(), TEXT("ComponentBoundEvent"));
			M.Emplace(UK2Node_FunctionEntry::StaticClass(),       TEXT("FunctionEntry"));
			M.Emplace(UK2Node_FunctionResult::StaticClass(),      TEXT("FunctionResult"));
			M.Emplace(UK2Node_Tunnel::StaticClass(),              TEXT("Tunnel"));

			// Structural drift check: every key non-null, every value non-empty,
			// and every value drawn from the known-alias whitelist.
			static const TCHAR* const KnownAliases[] = {
				TEXT("CallFunction"),
				TEXT("VariableGet"),
				TEXT("VariableSet"),
				TEXT("Branch"),
				TEXT("Sequence"),
				TEXT("Cast"),
				TEXT("SpawnActor"),
				TEXT("CustomEvent"),
				TEXT("Knot"),
				TEXT("Comment"),
				TEXT("Select"),
				TEXT("MakeArray"),
				TEXT("MakeSet"),
				TEXT("MakeMap"),
				TEXT("GetArrayItem"),
				TEXT("MakeStruct"),
				TEXT("BreakStruct"),
				TEXT("SetFieldsInStruct"),
				TEXT("SwitchInteger"),
				TEXT("SwitchString"),
				TEXT("SwitchName"),
				TEXT("SwitchEnum"),
				TEXT("ForEachElementInEnum"),
				TEXT("DoOnceMultiInput"),
				TEXT("MultiGate"),
				TEXT("FormatText"),
				TEXT("Composite"),
				TEXT("SwitchGameplayTag"),
				TEXT("MacroInstance"),
				TEXT("EventOverride"),
				TEXT("CallParentFunction"),
				TEXT("Timeline"),
				TEXT("AddDelegate"),
				TEXT("RemoveDelegate"),
				TEXT("ClearDelegate"),
				TEXT("CallDelegate"),
				TEXT("CreateDelegate"),
				TEXT("AssignDelegate"),
				TEXT("ComponentBoundEvent"),
				TEXT("FunctionEntry"),
				TEXT("FunctionResult"),
				TEXT("Tunnel"),
			};
			for (const TPair<const UClass*, FString>& Pair : M)
			{
				checkf(Pair.Key != nullptr,
					TEXT("ClaireonNodeTypeAlias: invalid map entry -- key is null (value=%s)"),
					*Pair.Value);
				checkf(!Pair.Value.IsEmpty(),
					TEXT("ClaireonNodeTypeAlias: invalid map entry -- value is empty (key=%s)"),
					*GetNameSafe(Pair.Key));
				bool bFound = false;
				for (const TCHAR* Known : KnownAliases)
				{
					if (Pair.Value.Equals(Known, ESearchCase::CaseSensitive))
					{
						bFound = true;
						break;
					}
				}
				checkf(bFound,
					TEXT("ClaireonNodeTypeAlias: invalid map entry -- value not in whitelist (key=%s value=%s)"),
					*GetNameSafe(Pair.Key), *Pair.Value);
			}
			return M;
		}();
		return Map;
	}

	FString GetAliasForNodeClass(const UClass* NodeClass)
	{
		if (!IsValid(NodeClass))
		{
			return FString();
		}

		// Subclass fan-in: every UK2Node_CallFunction descendant reports as "CallFunction".
		if (NodeClass->IsChildOf(UK2Node_CallFunction::StaticClass()))
		{
			return TEXT("CallFunction");
		}

		const TMap<const UClass*, FString>& Map = GetAliasMap();
		if (const FString* Found = Map.Find(NodeClass))
		{
			return *Found;
		}
		return FString();
	}

	void ResolveNodeTypeAlias(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return;
		}

		FString NodeType;
		if (!Params->TryGetStringField(TEXT("node_type"), NodeType))
		{
			return;
		}
		if (NodeType.IsEmpty())
		{
			return;
		}

		// Latent task spellings. These are not class names, so nothing below would
		// resolve them, and bp_add_node's final else would reject the call outright.
		// Rewrite to the Generic + class_name route the factory already supports,
		// choosing the node class by what the factory actually returns:
		// UK2Node_LatentAbilityCall for UAbilityTask, UK2Node_LatentGameplayTaskCall
		// for any other UGameplayTask. function_name/function_class forward untouched.
		if (NodeType.Equals(TEXT("LatentAbilityCall"), ESearchCase::IgnoreCase)
			|| NodeType.Equals(TEXT("LatentGameplayTaskCall"), ESearchCase::IgnoreCase))
		{
			const bool bAbilityTaskRequested = NodeType.Equals(TEXT("LatentAbilityCall"), ESearchCase::IgnoreCase);
			bool bUseAbilityCall = bAbilityTaskRequested;

			// If the caller named a factory, let the factory's own type decide -- the
			// spelling is a hint, the resolved class is the truth.
			FString FunctionClassName;
			if (Params->TryGetStringField(TEXT("function_class"), FunctionClassName) && !FunctionClassName.IsEmpty())
			{
				ClaireonNameResolver::FNameResolveResult ClassResult;
				if (UClass* FactoryOwner = ClaireonNameResolver::ResolveClassName(FunctionClassName, nullptr, ClassResult); IsValid(FactoryOwner))
				{
					bUseAbilityCall = FactoryOwner->IsChildOf(UAbilityTask::StaticClass());
				}
			}

			Params->SetStringField(TEXT("node_type"), TEXT("Generic"));
			Params->SetStringField(TEXT("class_name"),
				bUseAbilityCall ? TEXT("K2Node_LatentAbilityCall") : TEXT("K2Node_LatentGameplayTaskCall"));
			return;
		}

		UClass* Resolved = FindObject<UClass>(nullptr, *NodeType);
		if (!IsValid(Resolved))
		{
			const FString WithPrefix = FString(TEXT("U")) + NodeType;
			Resolved = FindObject<UClass>(nullptr, *WithPrefix);
		}
		if (!IsValid(Resolved) && (NodeType.Contains(TEXT("Node_")) || NodeType.StartsWith(TEXT("/Script/"))))
		{
			// FindObject with a null outer only resolves full object paths, so short
			// class names ("K2Node_MakeArray", project K2Node subclasses) never hit.
			// Fall back to the fuzzy resolver, but only for strings that structurally
			// look like node-class names so plain aliases ("Branch") are never
			// misrouted through fuzzy class matching.
			ClaireonNameResolver::FNameResolveResult R;
			Resolved = ClaireonNameResolver::ResolveClassName(NodeType, UEdGraphNode::StaticClass(), R);
		}
		if (!IsValid(Resolved))
		{
			return;
		}

		const FString Alias = GetAliasForNodeClass(Resolved);
		if (!Alias.IsEmpty())
		{
			Params->SetStringField(TEXT("node_type"), Alias);
		}
		else
		{
			Params->SetStringField(TEXT("node_type"), TEXT("Generic"));
			Params->SetStringField(TEXT("class_name"), NodeType);
		}
	}
}

namespace ClaireonBPGraphInternal
{
	UEdGraphNode* SelectEntryNodeForSwitch(const UBlueprint* Blueprint, UEdGraph* Graph)
	{
		if (!IsValid(Graph))
		{
			return nullptr;
		}

		// AnimGraph detection: UAnimationGraph is the graph container type for anim function graphs.
		if (Cast<UAnimationGraph>(Graph))
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Cast<UAnimGraphNode_Root>(Node))
				{
					return Node;
				}
			}
		}
		else if (IsValid(Blueprint) && Blueprint->FunctionGraphs.Contains(Graph))
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Cast<UK2Node_FunctionEntry>(Node))
				{
					return Node;
				}
			}
		}
		else if (IsValid(Blueprint) && Blueprint->MacroGraphs.Contains(Graph))
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node); IsValid(Tunnel))
				{
					bool bHasInputPin = false;
					for (UEdGraphPin* Pin : Tunnel->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Input)
						{
							bHasInputPin = true;
							break;
						}
					}
					if (!bHasInputPin)
					{
						return Node;
					}
				}
			}
		}

		TArray<UEdGraphNode*> Roots = ClaireonBlueprintHelpers::FindRootNodes(Graph);
		return Roots.Num() > 0 ? Roots[0] : nullptr;
	}

	UEdGraphNode* FindNodeForOperation(UEdGraph* Graph, const FGuid& RequestedGuid, FBlueprintEditToolData* Data)
	{
		FGuid CorrectedGuid;
		UEdGraphNode* Node = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, RequestedGuid, &CorrectedGuid);
		if (IsValid(Node) && CorrectedGuid.IsValid() && Data)
		{
			Data->GuidCorrections.Add(RequestedGuid, CorrectedGuid);
		}
		return Node;
	}

	UEdGraphNode* FindNodeForOperationStr(UEdGraph* Graph, const FString& NodeGuidStr, FBlueprintEditToolData* Data,
	                                      FString& OutError, const TCHAR* FieldName)
	{
		UEdGraphNode* OutNode = nullptr;
		FGuid CorrectedFullGuid;
		if (!ClaireonBlueprintHelpers::ResolveNodeGuidString(Graph, NodeGuidStr, OutNode, OutError, FieldName, &CorrectedFullGuid))
		{
			return nullptr;
		}

		// Preserve the pre-existing A-field recompile-recovery bookkeeping: only a
		// full-GUID input that got A-field-corrected produces a valid CorrectedFullGuid.
		// Prefix-resolved lookups leave it invalid and record no correction.
		if (Data && CorrectedFullGuid.IsValid())
		{
			FGuid RequestedGuid;
			FGuid::Parse(NodeGuidStr, RequestedGuid);
			Data->GuidCorrections.Add(RequestedGuid, CorrectedFullGuid);
		}
		return OutNode;
	}

	int32 ScrubTrashedPinLinks(UBlueprint* Blueprint, TArray<FString>& OutDetails)
	{
		if (!IsValid(Blueprint))
		{
			return 0;
		}

		int32 Removed = 0;
		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!IsValid(Graph))
			{
				continue;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!IsValid(Node))
				{
					continue;
				}
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin || Pin->WasTrashed())
					{
						continue;
					}
					for (int32 LinkIdx = Pin->LinkedTo.Num() - 1; LinkIdx >= 0; --LinkIdx)
					{
						UEdGraphPin* Linked = Pin->LinkedTo[LinkIdx];
						if (!Linked || Linked->WasTrashed())
						{
							OutDetails.Add(FString::Printf(
								TEXT("Removed stale link on pin '%s' of node '%s' (graph '%s'): linked pin %s"),
								*Pin->PinName.ToString(),
								*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
								*Graph->GetName(),
								Linked ? *FString::Printf(TEXT("'%s' was trashed"), *Linked->PinName.ToString()) : TEXT("was null")));
							Node->Modify();
							Pin->LinkedTo.RemoveAt(LinkIdx);
							++Removed;
						}
					}
					// A trashed pin lingering in SubPins would also serialize; drop it.
					for (int32 SubIdx = Pin->SubPins.Num() - 1; SubIdx >= 0; --SubIdx)
					{
						UEdGraphPin* Sub = Pin->SubPins[SubIdx];
						if (!Sub || Sub->WasTrashed())
						{
							OutDetails.Add(FString::Printf(
								TEXT("Removed trashed sub-pin entry under pin '%s' of node '%s' (graph '%s')"),
								*Pin->PinName.ToString(),
								*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
								*Graph->GetName()));
							Node->Modify();
							Pin->SubPins.RemoveAt(SubIdx);
							++Removed;
						}
					}
					if (Pin->ParentPin && Pin->ParentPin->WasTrashed())
					{
						OutDetails.Add(FString::Printf(
							TEXT("Cleared trashed parent-pin reference on pin '%s' of node '%s' (graph '%s')"),
							*Pin->PinName.ToString(),
							*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
							*Graph->GetName()));
						Node->Modify();
						Pin->ParentPin = nullptr;
						++Removed;
					}
				}
			}
		}
		return Removed;
	}
}
