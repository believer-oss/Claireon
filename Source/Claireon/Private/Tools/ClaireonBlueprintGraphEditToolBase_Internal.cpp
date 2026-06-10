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
	static const TCHAR* const GKnownMacros[] = {
		TEXT("DoN"),
		TEXT("DoOnce"),
		TEXT("FlipFlop"),
		TEXT("ForEachLoop"),
		TEXT("ForEachLoopWithBreak"),
		TEXT("ForLoop"),
		TEXT("ForLoopWithBreak"),
		TEXT("Gate"),
		TEXT("IsValid"),
		TEXT("MultiGate"),
		TEXT("StandardMacroBranch"),
		TEXT("SwitchHasAuthority"),
		TEXT("WhileLoop"),
	};

	static const TCHAR* const GStandardMacroLibrary =
		TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros");

	static bool IsKnownMacroName(const FString& Name)
	{
		for (const TCHAR* Known : GKnownMacros)
		{
			if (Name.Equals(Known, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
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
			Params->SetStringField(TEXT("macro_name"), NodeType);
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
			M.Emplace(UK2Node_SwitchInteger::StaticClass(),       TEXT("SwitchInteger"));
			M.Emplace(UK2Node_SwitchString::StaticClass(),        TEXT("SwitchString"));
			M.Emplace(UK2Node_SwitchName::StaticClass(),          TEXT("SwitchName"));
			M.Emplace(UK2Node_SwitchEnum::StaticClass(),          TEXT("SwitchEnum"));
			M.Emplace(UK2Node_ForEachElementInEnum::StaticClass(),TEXT("ForEachElementInEnum"));
			M.Emplace(UK2Node_DoOnceMultiInput::StaticClass(),    TEXT("DoOnceMultiInput"));
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
				TEXT("SwitchInteger"),
				TEXT("SwitchString"),
				TEXT("SwitchName"),
				TEXT("SwitchEnum"),
				TEXT("ForEachElementInEnum"),
				TEXT("DoOnceMultiInput"),
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
		if (!NodeClass)
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

		UClass* Resolved = FindObject<UClass>(nullptr, *NodeType);
		if (!Resolved)
		{
			const FString WithPrefix = FString(TEXT("U")) + NodeType;
			Resolved = FindObject<UClass>(nullptr, *WithPrefix);
		}
		if (!Resolved)
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
		if (!Graph)
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
		else if (Blueprint && Blueprint->FunctionGraphs.Contains(Graph))
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Cast<UK2Node_FunctionEntry>(Node))
				{
					return Node;
				}
			}
		}
		else if (Blueprint && Blueprint->MacroGraphs.Contains(Graph))
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node))
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
		if (Node && CorrectedGuid.IsValid() && Data)
		{
			Data->GuidCorrections.Add(RequestedGuid, CorrectedGuid);
		}
		return Node;
	}
}
