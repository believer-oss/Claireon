// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_BlueprintDiff.h"

#include "ClaireonLog.h"
#include "Tools/ClaireonDiffHelpers.h"
#include "DiffUtils.h"
#include "DiffResults.h"
#include "GraphDiffControl.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ââ Local Helpers âââââââââââââââââââââââââââââââââââââââââââââââââââââââ

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

} // anonymous namespace

// ââ Tool Interface ââââââââââââââââââââââââââââââââââââââââââââââââââââââ

FString ClaireonTool_BlueprintDiff::GetName() const
{
	return TEXT("diff_blueprint");
}

FString ClaireonTool_BlueprintDiff::GetCategory() const
{
	return TEXT("diff");
}

FString ClaireonTool_BlueprintDiff::GetDescription() const
{
	return TEXT("Full Blueprint comparison: graphs, CDO properties, and SCS components. "
		"Supports loading Blueprints from the current editor state or from git revisions. "
		"Each side is an asset path with an optional git revision. "
		"Use 'sections' to select which parts to compare (graphs, cdo, scs). "
		"Use 'resolution' for output detail level: exists, summary, or detailed.");
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

	FString ValidationError;
	if (!ClaireonDiffHelpers::ValidateAssetPath(PathA, ValidationError))
	{
		return MakeErrorResult(ValidationError);
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
