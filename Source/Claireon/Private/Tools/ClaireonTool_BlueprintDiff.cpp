// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_BlueprintDiff.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h" // kBPCategory

#include "ClaireonBlueprintHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "Tools/ClaireonDiffHelpers.h"
#include "DiffUtils.h"
#include "DiffResults.h"
#include "GraphDiffControl.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_Knot.h"
#include "EdGraphNode_Comment.h"

// Ã¢Â”Â€Ã¢Â”Â€ Local Helpers Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€

namespace
{

/** Collect all graphs from a Blueprint into a name->graph map. */
TMap<FString, UEdGraph*> CollectGraphs(const UBlueprint* Blueprint)
{
	TMap<FString, UEdGraph*> Result;
	if (!Blueprint)
	{
		return Result;
	}

	auto AddGraphs = [&Result](const TArray<UEdGraph*>& Graphs)
	{
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph)
			{
				Result.Add(Graph->GetName(), Graph);
			}
		}
	};

	AddGraphs(Blueprint->UbergraphPages);
	AddGraphs(Blueprint->FunctionGraphs);
	AddGraphs(Blueprint->MacroGraphs);

	return Result;
}

/** Recursively build SCS hierarchy without UI widgets. */
void BuildSCSHierarchyRecursive(USCS_Node* Node, TArray<int32>& TreeAddress, TArray<FSCSResolvedIdentifier>& Out)
{
	if (!Node)
	{
		return;
	}

	FSCSIdentifier Id;
	Id.Name = Node->GetVariableName();
	Id.TreeLocation = TreeAddress;

	FSCSResolvedIdentifier Resolved;
	Resolved.Identifier = Id;
	Resolved.Object = Node->ComponentTemplate;
	Out.Add(Resolved);

	const TArray<USCS_Node*>& Children = Node->GetChildNodes();
	for (int32 i = 0; i < Children.Num(); ++i)
	{
		TreeAddress.Push(i);
		BuildSCSHierarchyRecursive(Children[i], TreeAddress, Out);
		TreeAddress.Pop();
	}
}

/** Build the full SCS hierarchy for a Blueprint. */
TArray<FSCSResolvedIdentifier> BuildSCSHierarchy(const UBlueprint* Blueprint)
{
	TArray<FSCSResolvedIdentifier> Hierarchy;
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return Hierarchy;
	}

	const TArray<USCS_Node*>& RootNodes = Blueprint->SimpleConstructionScript->GetRootNodes();
	TArray<int32> TreeAddress;

	for (int32 i = 0; i < RootNodes.Num(); ++i)
	{
		TreeAddress.Reset();
		TreeAddress.Push(i);
		BuildSCSHierarchyRecursive(RootNodes[i], TreeAddress, Hierarchy);
	}

	return Hierarchy;
}

FString FormatEDiffCategory(EDiffType::Category Category)
{
	switch (Category)
	{
	case EDiffType::ADDITION:    return TEXT("Addition");
	case EDiffType::SUBTRACTION: return TEXT("Subtraction");
	case EDiffType::MODIFICATION: return TEXT("Modification");
	case EDiffType::MINOR:       return TEXT("Minor");
	default:                     return TEXT("Unknown");
	}
}

FString FormatTreeDiffType(ETreeDiffType::Type DiffType)
{
	switch (DiffType)
	{
	case ETreeDiffType::NODE_ADDED:            return TEXT("NodeAdded");
	case ETreeDiffType::NODE_REMOVED:          return TEXT("NodeRemoved");
	case ETreeDiffType::NODE_TYPE_CHANGED:     return TEXT("TypeChanged");
	case ETreeDiffType::NODE_PROPERTY_CHANGED: return TEXT("PropertyChanged");
	case ETreeDiffType::NODE_MOVED:            return TEXT("NodeMoved");
	case ETreeDiffType::NODE_CORRUPTED:        return TEXT("NodeCorrupted");
	case ETreeDiffType::NODE_FIXED:            return TEXT("NodeFixed");
	default:                                   return TEXT("Unknown");
	}
}

// -- apply_spec-vs-asset diff helpers --

/** Pin-default snapshot for a single node. Maps pin name -> default value string. */
using FPinDefaultMap = TMap<FString, FString>;

/** One side of a spec-vs-asset diff entry. */
struct FSpecNodeEntry
{
	/** Display-facing spec id or asset node GUID. */
	FString Id;
	/** Normalized type tag, e.g. "CallFunction", "VariableGet". */
	FString TypeTag;
	/** Secondary key that disambiguates nodes of the same type (e.g. function name). */
	FString Key;
	/** Pin default values snapshotted for this node. */
	FPinDefaultMap PinDefaults;
};

struct FSpecVariableEntry
{
	FString Name;
	FString TypeString;
};

struct FSpecConnectionEntry
{
	FString SourceNodeKey;
	FString SourcePin;
	FString TargetNodeKey;
	FString TargetPin;

	FString ToKey() const
	{
		return FString::Printf(TEXT("%s::%s -> %s::%s"),
			*SourceNodeKey, *SourcePin, *TargetNodeKey, *TargetPin);
	}
};

/** Normalize a spec's "type" field to a short tag used for signature matching. */
FString NormalizeNodeType(const FString& SpecType)
{
	if (SpecType.StartsWith(TEXT("K2Node_")))
	{
		return SpecType.RightChop(7);
	}
	if (SpecType == TEXT("Branch")) return TEXT("IfThenElse");
	if (SpecType == TEXT("Sequence")) return TEXT("ExecutionSequence");
	if (SpecType == TEXT("Cast")) return TEXT("DynamicCast");
	if (SpecType == TEXT("Reroute")) return TEXT("Knot");
	if (SpecType == TEXT("Comment")) return TEXT("EdGraphNode_Comment");
	return SpecType;
}

/** Extract a signature (type tag + disambiguation key) from a spec node JSON entry. */
void BuildSpecNodeEntry(const TSharedPtr<FJsonObject>& NodeObj, FSpecNodeEntry& Out)
{
	NodeObj->TryGetStringField(TEXT("id"), Out.Id);
	FString RawType;
	NodeObj->TryGetStringField(TEXT("type"), RawType);
	Out.TypeTag = NormalizeNodeType(RawType);

	if (Out.TypeTag == TEXT("CallFunction"))
	{
		NodeObj->TryGetStringField(TEXT("function"), Out.Key);
		if (Out.Key.IsEmpty())
		{
			NodeObj->TryGetStringField(TEXT("function_name"), Out.Key);
		}
	}
	else if (Out.TypeTag == TEXT("VariableGet") || Out.TypeTag == TEXT("VariableSet"))
	{
		NodeObj->TryGetStringField(TEXT("variable_name"), Out.Key);
	}
	else if (Out.TypeTag == TEXT("CustomEvent"))
	{
		NodeObj->TryGetStringField(TEXT("event_name"), Out.Key);
	}
	else if (Out.TypeTag == TEXT("DynamicCast"))
	{
		NodeObj->TryGetStringField(TEXT("target_class"), Out.Key);
	}
	else if (Out.TypeTag == TEXT("MacroInstance"))
	{
		NodeObj->TryGetStringField(TEXT("macro_name"), Out.Key);
	}

	const TSharedPtr<FJsonObject>* PinDefaultsPtr = nullptr;
	if (NodeObj->TryGetObjectField(TEXT("pin_defaults"), PinDefaultsPtr) && PinDefaultsPtr && (*PinDefaultsPtr).IsValid())
	{
		for (const auto& Pair : (*PinDefaultsPtr)->Values)
		{
			FString PinValue;
			if (Pair.Value.IsValid() && Pair.Value->TryGetString(PinValue))
			{
				Out.PinDefaults.Add(Pair.Key, PinValue);
			}
		}
	}
}

/** Extract a signature entry from a live Blueprint graph node. */
void BuildAssetNodeEntry(const UEdGraphNode* Node, FSpecNodeEntry& Out)
{
	if (!Node)
	{
		return;
	}

	if (const UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
	{
		Out.TypeTag = TEXT("CallFunction");
		Out.Key = CallNode->FunctionReference.GetMemberName().ToString();
	}
	else if (const UK2Node_VariableGet* GetNode = Cast<UK2Node_VariableGet>(Node))
	{
		Out.TypeTag = TEXT("VariableGet");
		Out.Key = GetNode->VariableReference.GetMemberName().ToString();
	}
	else if (const UK2Node_VariableSet* SetNode = Cast<UK2Node_VariableSet>(Node))
	{
		Out.TypeTag = TEXT("VariableSet");
		Out.Key = SetNode->VariableReference.GetMemberName().ToString();
	}
	else if (Node->IsA<UK2Node_IfThenElse>())
	{
		Out.TypeTag = TEXT("IfThenElse");
	}
	else if (Node->IsA<UK2Node_ExecutionSequence>())
	{
		Out.TypeTag = TEXT("ExecutionSequence");
	}
	else if (const UK2Node_CustomEvent* EventNode = Cast<UK2Node_CustomEvent>(Node))
	{
		Out.TypeTag = TEXT("CustomEvent");
		Out.Key = EventNode->CustomFunctionName.ToString();
	}
	else if (const UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
	{
		Out.TypeTag = TEXT("DynamicCast");
		Out.Key = CastNode->TargetType ? CastNode->TargetType->GetName() : FString();
	}
	else if (Node->IsA<UK2Node_Knot>())
	{
		Out.TypeTag = TEXT("Knot");
	}
	else if (Node->IsA<UEdGraphNode_Comment>())
	{
		Out.TypeTag = TEXT("EdGraphNode_Comment");
	}
	else
	{
		FString ClassName = Node->GetClass()->GetName();
		if (ClassName.StartsWith(TEXT("K2Node_")))
		{
			ClassName = ClassName.RightChop(7);
		}
		Out.TypeTag = ClassName;
	}

	Out.Id = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin || Pin->PinName == NAME_None)
		{
			continue;
		}
		if (!Pin->DefaultValue.IsEmpty())
		{
			Out.PinDefaults.Add(Pin->PinName.ToString(), Pin->DefaultValue);
		}
	}
}

/** Format a node signature for human-readable output. */
FString FormatSignature(const FSpecNodeEntry& Entry)
{
	if (Entry.Key.IsEmpty())
	{
		return Entry.TypeTag;
	}
	return FString::Printf(TEXT("%s(%s)"), *Entry.TypeTag, *Entry.Key);
}

/** Build a stable matching key from (TypeTag, Key). */
FString MakeSignatureKey(const FSpecNodeEntry& Entry)
{
	return FString::Printf(TEXT("%s::%s"), *Entry.TypeTag, *Entry.Key);
}

/** Format a variable's type for human-readable comparison. */
FString FormatVariableTypeTag(const FBPVariableDescription& Var)
{
	FString Category = Var.VarType.PinCategory.ToString();
	if (Var.VarType.ContainerType == EPinContainerType::Array)
	{
		return FString::Printf(TEXT("Array<%s>"), *Category);
	}
	if (Var.VarType.ContainerType == EPinContainerType::Set)
	{
		return FString::Printf(TEXT("Set<%s>"), *Category);
	}
	if (Var.VarType.ContainerType == EPinContainerType::Map)
	{
		return FString::Printf(TEXT("Map<%s,?>"), *Category);
	}
	return Category;
}

/** Stable key for a spec connection. */
FString MakeConnectionKey(const FSpecConnectionEntry& Conn)
{
	return Conn.ToKey();
}

/** Append a section header and its bullet list (with "(none)" placeholder when empty). */
void AppendSection(FString& Out, const FString& Header, const TArray<FString>& Lines)
{
	Out.Append(Header);
	Out.Append(TEXT("\n"));
	if (Lines.Num() == 0)
	{
		Out.Append(TEXT("- (none)\n"));
	}
	else
	{
		for (const FString& Line : Lines)
		{
			Out.Append(TEXT("- "));
			Out.Append(Line);
			Out.Append(TEXT("\n"));
		}
	}
	Out.Append(TEXT("\n"));
}

/** Compute and format the apply_spec-vs-asset diff. Does not mutate the Blueprint. */
FString ComputeSpecDiff(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Spec,
	int32& OutNodesAdded, int32& OutNodesRemoved, int32& OutNodesChanged,
	int32& OutConnectionsAdded, int32& OutConnectionsRemoved, int32& OutVariableDeltas)
{
	OutNodesAdded = 0;
	OutNodesRemoved = 0;
	OutNodesChanged = 0;
	OutConnectionsAdded = 0;
	OutConnectionsRemoved = 0;
	OutVariableDeltas = 0;

	// Resolve target graph -- same default as the applicator uses.
	FString GraphName = TEXT("EventGraph");
	Spec->TryGetStringField(TEXT("graph"), GraphName);

	UEdGraph* Graph = ClaireonBlueprintHelpers::FindGraphByName(Blueprint, GraphName);

	// --- Build spec node entries ---
	TArray<FSpecNodeEntry> SpecNodes;
	TMap<FString, FString> SpecIdToSignature;
	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("nodes"), NodesArray) && NodesArray)
	{
		for (const TSharedPtr<FJsonValue>& NodeVal : *NodesArray)
		{
			if (!NodeVal.IsValid() || NodeVal->Type != EJson::Object) continue;
			FSpecNodeEntry Entry;
			BuildSpecNodeEntry(NodeVal->AsObject(), Entry);
			SpecIdToSignature.Add(Entry.Id, MakeSignatureKey(Entry));
			SpecNodes.Add(MoveTemp(Entry));
		}
	}

	// --- Build asset node entries (from the target graph) ---
	TArray<FSpecNodeEntry> AssetNodes;
	TArray<UEdGraphNode*> AssetNodePtrs;
	if (Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			FSpecNodeEntry Entry;
			BuildAssetNodeEntry(Node, Entry);
			AssetNodes.Add(MoveTemp(Entry));
			AssetNodePtrs.Add(Node);
		}
	}

	// Signature -> indices into each side (arrays to handle duplicates by count).
	TMap<FString, TArray<int32>> SpecBySig;
	TMap<FString, TArray<int32>> AssetBySig;
	for (int32 i = 0; i < SpecNodes.Num(); ++i)
	{
		SpecBySig.FindOrAdd(MakeSignatureKey(SpecNodes[i])).Add(i);
	}
	for (int32 i = 0; i < AssetNodes.Num(); ++i)
	{
		AssetBySig.FindOrAdd(MakeSignatureKey(AssetNodes[i])).Add(i);
	}

	TArray<FString> AddedLines;
	TArray<FString> RemovedLines;
	TArray<FString> ChangedLines;

	// Nodes added: spec count exceeds asset count for that signature.
	for (const auto& Pair : SpecBySig)
	{
		const TArray<int32>* AssetIdx = AssetBySig.Find(Pair.Key);
		const int32 SpecCount = Pair.Value.Num();
		const int32 AssetCount = AssetIdx ? AssetIdx->Num() : 0;
		if (SpecCount > AssetCount)
		{
			for (int32 k = AssetCount; k < SpecCount; ++k)
			{
				const FSpecNodeEntry& Entry = SpecNodes[Pair.Value[k]];
				AddedLines.Add(FString::Printf(TEXT("%s [spec id: %s]"),
					*FormatSignature(Entry), *Entry.Id));
				OutNodesAdded++;
			}
		}

		// Nodes changed: for paired nodes, report pin-default differences.
		const int32 PairedCount = FMath::Min(SpecCount, AssetCount);
		for (int32 k = 0; k < PairedCount; ++k)
		{
			const FSpecNodeEntry& SpecEntry = SpecNodes[Pair.Value[k]];
			const FSpecNodeEntry& AssetEntry = AssetNodes[(*AssetIdx)[k]];
			for (const auto& SpecPin : SpecEntry.PinDefaults)
			{
				const FString* AssetVal = AssetEntry.PinDefaults.Find(SpecPin.Key);
				if (!AssetVal)
				{
					ChangedLines.Add(FString::Printf(TEXT("%s pin '%s' old=<unset> new='%s'"),
						*FormatSignature(SpecEntry), *SpecPin.Key, *SpecPin.Value));
					OutNodesChanged++;
				}
				else if (*AssetVal != SpecPin.Value)
				{
					ChangedLines.Add(FString::Printf(TEXT("%s pin '%s' old='%s' new='%s'"),
						*FormatSignature(SpecEntry), *SpecPin.Key, **AssetVal, *SpecPin.Value));
					OutNodesChanged++;
				}
			}
		}
	}

	// Nodes removed: asset has signatures not covered by spec.
	for (const auto& Pair : AssetBySig)
	{
		const TArray<int32>* SpecIdx = SpecBySig.Find(Pair.Key);
		const int32 AssetCount = Pair.Value.Num();
		const int32 SpecCount = SpecIdx ? SpecIdx->Num() : 0;
		if (AssetCount > SpecCount)
		{
			for (int32 k = SpecCount; k < AssetCount; ++k)
			{
				const FSpecNodeEntry& Entry = AssetNodes[Pair.Value[k]];
				RemovedLines.Add(FString::Printf(TEXT("%s [node guid: %s]"),
					*FormatSignature(Entry), *Entry.Id));
				OutNodesRemoved++;
			}
		}
	}

	// --- Connections ---
	TArray<FSpecConnectionEntry> SpecConnections;
	const TArray<TSharedPtr<FJsonValue>>* ConnArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("connections"), ConnArray) && ConnArray)
	{
		for (const TSharedPtr<FJsonValue>& ConnVal : *ConnArray)
		{
			if (!ConnVal.IsValid() || ConnVal->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& ConnObj = ConnVal->AsObject();
			FSpecConnectionEntry Conn;
			FString SrcId, TgtId;
			ConnObj->TryGetStringField(TEXT("source_node"), SrcId);
			ConnObj->TryGetStringField(TEXT("source_pin"), Conn.SourcePin);
			ConnObj->TryGetStringField(TEXT("target_node"), TgtId);
			ConnObj->TryGetStringField(TEXT("target_pin"), Conn.TargetPin);

			const FString* SrcSig = SpecIdToSignature.Find(SrcId);
			const FString* TgtSig = SpecIdToSignature.Find(TgtId);
			Conn.SourceNodeKey = SrcSig ? *SrcSig : SrcId;
			Conn.TargetNodeKey = TgtSig ? *TgtSig : TgtId;
			SpecConnections.Add(MoveTemp(Conn));
		}
	}

	TArray<FSpecConnectionEntry> AssetConnections;
	if (Graph)
	{
		TMap<UEdGraphNode*, FString> AssetNodeToSig;
		for (int32 i = 0; i < AssetNodePtrs.Num(); ++i)
		{
			AssetNodeToSig.Add(AssetNodePtrs[i], MakeSignatureKey(AssetNodes[i]));
		}
		for (UEdGraphNode* Node : AssetNodePtrs)
		{
			if (!Node) continue;
			const FString* SrcSig = AssetNodeToSig.Find(Node);
			if (!SrcSig) continue;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output) continue;
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin || !LinkedPin->GetOwningNode()) continue;
					const FString* TgtSig = AssetNodeToSig.Find(LinkedPin->GetOwningNode());
					if (!TgtSig) continue;
					FSpecConnectionEntry Conn;
					Conn.SourceNodeKey = *SrcSig;
					Conn.SourcePin = Pin->PinName.ToString();
					Conn.TargetNodeKey = *TgtSig;
					Conn.TargetPin = LinkedPin->PinName.ToString();
					AssetConnections.Add(MoveTemp(Conn));
				}
			}
		}
	}

	TSet<FString> SpecConnKeys;
	for (const FSpecConnectionEntry& C : SpecConnections)
	{
		SpecConnKeys.Add(MakeConnectionKey(C));
	}
	TSet<FString> AssetConnKeys;
	for (const FSpecConnectionEntry& C : AssetConnections)
	{
		AssetConnKeys.Add(MakeConnectionKey(C));
	}

	TArray<FString> ConnAddedLines;
	TArray<FString> ConnRemovedLines;
	for (const FSpecConnectionEntry& C : SpecConnections)
	{
		const FString Key = MakeConnectionKey(C);
		if (!AssetConnKeys.Contains(Key))
		{
			ConnAddedLines.Add(Key);
			OutConnectionsAdded++;
		}
	}
	for (const FSpecConnectionEntry& C : AssetConnections)
	{
		const FString Key = MakeConnectionKey(C);
		if (!SpecConnKeys.Contains(Key))
		{
			ConnRemovedLines.Add(Key);
			OutConnectionsRemoved++;
		}
	}

	// --- Variables ---
	TArray<FSpecVariableEntry> SpecVars;
	const TArray<TSharedPtr<FJsonValue>>* VarsArray = nullptr;
	if (Spec->TryGetArrayField(TEXT("variables"), VarsArray) && VarsArray)
	{
		for (const TSharedPtr<FJsonValue>& VarVal : *VarsArray)
		{
			if (!VarVal.IsValid() || VarVal->Type != EJson::Object) continue;
			FSpecVariableEntry Entry;
			VarVal->AsObject()->TryGetStringField(TEXT("name"), Entry.Name);
			VarVal->AsObject()->TryGetStringField(TEXT("type"), Entry.TypeString);
			SpecVars.Add(MoveTemp(Entry));
		}
	}

	TMap<FString, FString> AssetVarNameToType;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		AssetVarNameToType.Add(Var.VarName.ToString(), FormatVariableTypeTag(Var));
	}
	TSet<FString> SpecVarNames;
	for (const FSpecVariableEntry& V : SpecVars) SpecVarNames.Add(V.Name);

	TArray<FString> VarLines;
	for (const FSpecVariableEntry& V : SpecVars)
	{
		const FString* ExistingType = AssetVarNameToType.Find(V.Name);
		if (!ExistingType)
		{
			VarLines.Add(FString::Printf(TEXT("added: %s (%s)"), *V.Name, *V.TypeString));
			OutVariableDeltas++;
		}
		else if (!ExistingType->Equals(V.TypeString, ESearchCase::IgnoreCase))
		{
			VarLines.Add(FString::Printf(TEXT("changed: %s old=%s new=%s"),
				*V.Name, **ExistingType, *V.TypeString));
			OutVariableDeltas++;
		}
	}
	for (const auto& Pair : AssetVarNameToType)
	{
		if (!SpecVarNames.Contains(Pair.Key))
		{
			VarLines.Add(FString::Printf(TEXT("removed: %s (%s)"), *Pair.Key, *Pair.Value));
			OutVariableDeltas++;
		}
	}

	// --- Render output ---
	FString Out;
	AppendSection(Out, TEXT("### Nodes added"), AddedLines);
	AppendSection(Out, TEXT("### Nodes removed"), RemovedLines);
	AppendSection(Out, TEXT("### Nodes changed"), ChangedLines);
	AppendSection(Out, TEXT("### Connections added"), ConnAddedLines);
	AppendSection(Out, TEXT("### Connections removed"), ConnRemovedLines);
	AppendSection(Out, TEXT("### Variables"), VarLines);
	return Out;
}

} // anonymous namespace

// Ã¢Â”Â€Ã¢Â”Â€ Tool Interface Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€Ã¢Â”Â€

FString ClaireonTool_BlueprintDiff::GetCategory() const { return kBPCategory; }
FString ClaireonTool_BlueprintDiff::GetOperation() const { return TEXT("diff"); }

TArray<FString> ClaireonTool_BlueprintDiff::GetSearchKeywords() const
{
	return {TEXT("bp"), TEXT("blueprint"), TEXT("diff"), TEXT("compare"), TEXT("comparison"), TEXT("difference"), TEXT("git")};
}

FString ClaireonTool_BlueprintDiff::GetDescription() const
{
	return TEXT("Full Blueprint comparison: graphs, CDO properties, and SCS components. "
		"Supports loading Blueprints from the current editor state or from git revisions. "
		"Each side is an asset path with an optional git revision. "
		"Use 'sections' to select which parts to compare (graphs, cdo, scs). "
		"Use 'resolution' for output detail level: exists, summary, or detailed. "
		"Supply 'spec_json' to run apply_spec-vs-asset diff mode (read-only; compares an "
		"apply_spec payload against asset_path_a without mutating the Blueprint). Immediate-mode tool: no session required.");
}

TSharedPtr<FJsonObject> ClaireonTool_BlueprintDiff::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path_a (required)
	TSharedPtr<FJsonObject> PathAProp = MakeShared<FJsonObject>();
	PathAProp->SetStringField(TEXT("type"), TEXT("string"));
	PathAProp->SetStringField(TEXT("description"), TEXT("Unreal content path for side A (e.g. /Game/Characters/BP_Hero)"));
	Properties->SetObjectField(TEXT("asset_path_a"), PathAProp);

	// revision_a (optional)
	TSharedPtr<FJsonObject> RevAProp = MakeShared<FJsonObject>();
	RevAProp->SetStringField(TEXT("type"), TEXT("string"));
	RevAProp->SetStringField(TEXT("description"), TEXT("Git revision for side A. If omitted, loads from current editor state."));
	Properties->SetObjectField(TEXT("revision_a"), RevAProp);

	// asset_path_b (optional)
	TSharedPtr<FJsonObject> PathBProp = MakeShared<FJsonObject>();
	PathBProp->SetStringField(TEXT("type"), TEXT("string"));
	PathBProp->SetStringField(TEXT("description"), TEXT("Unreal content path for side B. Defaults to asset_path_a if omitted."));
	Properties->SetObjectField(TEXT("asset_path_b"), PathBProp);

	// revision_b (optional)
	TSharedPtr<FJsonObject> RevBProp = MakeShared<FJsonObject>();
	RevBProp->SetStringField(TEXT("type"), TEXT("string"));
	RevBProp->SetStringField(TEXT("description"), TEXT("Git revision for side B. If omitted, loads from current editor state."));
	Properties->SetObjectField(TEXT("revision_b"), RevBProp);

	// resolution (optional)
	TSharedPtr<FJsonObject> ResProp = MakeShared<FJsonObject>();
	ResProp->SetStringField(TEXT("type"), TEXT("string"));
	ResProp->SetStringField(TEXT("description"), TEXT("Output detail level: 'exists', 'summary', 'detailed'. Default: summary."));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("exists")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("summary")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("detailed")));
		ResProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("resolution"), ResProp);

	// sections (optional)
	TSharedPtr<FJsonObject> SectionsProp = MakeShared<FJsonObject>();
	SectionsProp->SetStringField(TEXT("type"), TEXT("array"));
	SectionsProp->SetStringField(TEXT("description"), TEXT("Which sections to include: 'graphs', 'cdo', 'scs'. Default: all three."));
	{
		TSharedPtr<FJsonObject> ItemSchema = MakeShared<FJsonObject>();
		ItemSchema->SetStringField(TEXT("type"), TEXT("string"));
		TArray<TSharedPtr<FJsonValue>> ItemEnum;
		ItemEnum.Add(MakeShared<FJsonValueString>(TEXT("graphs")));
		ItemEnum.Add(MakeShared<FJsonValueString>(TEXT("cdo")));
		ItemEnum.Add(MakeShared<FJsonValueString>(TEXT("scs")));
		ItemSchema->SetArrayField(TEXT("enum"), ItemEnum);
		SectionsProp->SetObjectField(TEXT("items"), ItemSchema);
	}
	Properties->SetObjectField(TEXT("sections"), SectionsProp);

	// property_filter (optional)
	TSharedPtr<FJsonObject> FilterProp = MakeShared<FJsonObject>();
	FilterProp->SetStringField(TEXT("type"), TEXT("array"));
	FilterProp->SetStringField(TEXT("description"), TEXT("Optional list of property names to filter CDO/SCS diffs."));
	{
		TSharedPtr<FJsonObject> ItemSchema = MakeShared<FJsonObject>();
		ItemSchema->SetStringField(TEXT("type"), TEXT("string"));
		FilterProp->SetObjectField(TEXT("items"), ItemSchema);
	}
	Properties->SetObjectField(TEXT("property_filter"), FilterProp);

	// spec_json (optional) -- triggers apply_spec-vs-asset diff mode
	TSharedPtr<FJsonObject> SpecJsonProp = MakeShared<FJsonObject>();
	SpecJsonProp->SetStringField(TEXT("type"), TEXT("string"));
	SpecJsonProp->SetStringField(TEXT("description"),
		TEXT("Optional apply_spec JSON payload (nodes[], connections[], variables[]). "
			"When provided, the tool switches to apply_spec-vs-asset diff mode: asset_path_a "
			"is opened read-only and compared against what applying this spec would produce. "
			"Other diff parameters (asset_path_b, revisions, sections, property_filter) are ignored."));
	Properties->SetObjectField(TEXT("spec_json"), SpecJsonProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path_a")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_BlueprintDiff::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString PathA;
	if (!Arguments->TryGetStringField(TEXT("asset_path_a"), PathA) || PathA.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path_a"));
	}

	auto ResolveResult = ClaireonPathResolver::Resolve(PathA);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	PathA = ResolveResult.ResolvedPath.Path;

	// apply_spec-vs-asset diff mode: triggered by presence of 'spec_json'.
	FString SpecJson;
	if (Arguments->TryGetStringField(TEXT("spec_json"), SpecJson) && !SpecJson.IsEmpty())
	{
		TSharedPtr<FJsonObject> SpecObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SpecJson);
		if (!FJsonSerializer::Deserialize(Reader, SpecObj) || !SpecObj.IsValid())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("spec_json parse failed at line %d col %d: %s"),
				Reader->GetLineNumber(), Reader->GetCharacterNumber(),
				*Reader->GetErrorMessage()));
		}

		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *PathA);
		if (!Blueprint)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Blueprint not found at path: %s"), *PathA));
		}

		int32 NA = 0, NR = 0, NC = 0, CA = 0, CR = 0, VD = 0;
		const FString DiffText = ComputeSpecDiff(Blueprint, SpecObj, NA, NR, NC, CA, CR, VD);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("path"), PathA);
		Data->SetStringField(TEXT("mode"), TEXT("apply_spec_diff"));
		Data->SetStringField(TEXT("diff"), DiffText);
		Data->SetNumberField(TEXT("nodes_added"), NA);
		Data->SetNumberField(TEXT("nodes_removed"), NR);
		Data->SetNumberField(TEXT("nodes_changed"), NC);
		Data->SetNumberField(TEXT("connections_added"), CA);
		Data->SetNumberField(TEXT("connections_removed"), CR);
		Data->SetNumberField(TEXT("variable_deltas"), VD);

		const FString Summary = FString::Printf(
			TEXT("spec diff: %d nodes added, %d removed, %d changed; %d conns added, %d removed; %d var deltas"),
			NA, NR, NC, CA, CR, VD);

		return MakeSuccessResult(Data, Summary);
	}

	FString PathB = PathA;
	Arguments->TryGetStringField(TEXT("asset_path_b"), PathB);

	FString RevisionA;
	FString RevisionB;
	Arguments->TryGetStringField(TEXT("revision_a"), RevisionA);
	Arguments->TryGetStringField(TEXT("revision_b"), RevisionB);

	FString ParamError;
	if (!ClaireonDiffHelpers::ValidateDiffParameters(PathA, RevisionA, PathB, RevisionB, ParamError))
	{
		return MakeErrorResult(ParamError);
	}

	FString ResolutionStr = TEXT("summary");
	Arguments->TryGetStringField(TEXT("resolution"), ResolutionStr);
	ClaireonDiffHelpers::EDiffResolution Resolution;
	FString ResError;
	if (!ClaireonDiffHelpers::ParseResolution(ResolutionStr, Resolution, ResError))
	{
		return MakeErrorResult(ResError);
	}

	// Section filter
	bool bDiffGraphs = true;
	bool bDiffCDO = true;
	bool bDiffSCS = true;
	const TArray<TSharedPtr<FJsonValue>>* SectionsArray = nullptr;
	if (Arguments->TryGetArrayField(TEXT("sections"), SectionsArray) && SectionsArray)
	{
		bDiffGraphs = bDiffCDO = bDiffSCS = false;
		for (const TSharedPtr<FJsonValue>& Val : *SectionsArray)
		{
			FString Sec;
			if (Val->TryGetString(Sec))
			{
				if (Sec == TEXT("graphs")) bDiffGraphs = true;
				else if (Sec == TEXT("cdo")) bDiffCDO = true;
				else if (Sec == TEXT("scs")) bDiffSCS = true;
			}
		}
	}

	// Resolve both sides
	FString ValidationError;
	ClaireonDiffHelpers::FResolvedDiffSide SideA = ClaireonDiffHelpers::ResolveDiffSide(PathA, RevisionA, ValidationError);
	if (!SideA.IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to resolve side A: %s"), *ValidationError));
	}
	ClaireonDiffHelpers::FScopedTempFile TempA(SideA.TempFilePath);

	ClaireonDiffHelpers::FResolvedDiffSide SideB = ClaireonDiffHelpers::ResolveDiffSide(PathB, RevisionB, ValidationError);
	if (!SideB.IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to resolve side B: %s"), *ValidationError));
	}
	ClaireonDiffHelpers::FScopedTempFile TempB(SideB.TempFilePath);

	const UBlueprint* BPA = Cast<UBlueprint>(SideA.Object);
	const UBlueprint* BPB = Cast<UBlueprint>(SideB.Object);

	if (!BPA || !BPB)
	{
		return MakeErrorResult(TEXT("One or both assets are not Blueprints"));
	}

	int32 NodesAdded = 0;
	int32 NodesRemoved = 0;
	int32 NodesChanged = 0;
	int32 ConnectionsChanged = 0;

	// Graph diff
	if (bDiffGraphs)
	{
		TMap<FString, UEdGraph*> GraphsA = CollectGraphs(BPA);
		TMap<FString, UEdGraph*> GraphsB = CollectGraphs(BPB);

		for (const auto& PairA : GraphsA)
		{
			UEdGraph** GraphBPtr = GraphsB.Find(PairA.Key);
			if (!GraphBPtr)
			{
				NodesRemoved++;
				continue;
			}

			TArray<FGraphDiffControl::FNodeMatch> NodeMatches;
			TArray<FDiffSingleResult> DiffResults;
			FGraphDiffControl::DiffGraphs(PairA.Value, *GraphBPtr, DiffResults);

			for (const FDiffSingleResult& Diff : DiffResults)
			{
				switch (Diff.Category)
				{
				case EDiffType::ADDITION:    NodesAdded++; break;
				case EDiffType::SUBTRACTION: NodesRemoved++; break;
				case EDiffType::MODIFICATION: NodesChanged++; break;
				default: ConnectionsChanged++; break;
				}
			}
		}

		// Count graphs only in B (added)
		for (const auto& PairB : GraphsB)
		{
			if (!GraphsA.Contains(PairB.Key))
			{
				NodesAdded++;
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("path"), PathA);
	Data->SetNumberField(TEXT("nodes_added"), NodesAdded);
	Data->SetNumberField(TEXT("nodes_removed"), NodesRemoved);
	Data->SetNumberField(TEXT("nodes_changed"), NodesChanged);
	Data->SetNumberField(TEXT("connections_changed"), ConnectionsChanged);

	const FString Summary = FString::Printf(TEXT("BP diff: %d nodes added, %d removed, %d changed"),
		NodesAdded, NodesRemoved, NodesChanged);

	return MakeSuccessResult(Data, Summary);
}
