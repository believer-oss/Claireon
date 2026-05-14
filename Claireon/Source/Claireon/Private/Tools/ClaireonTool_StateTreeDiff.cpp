// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_StateTreeDiff.h"

#include "ClaireonLog.h"
#include "Tools/ClaireonDiffHelpers.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "DiffUtils.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEditorPropertyBindings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ââ Local Helpers âââââââââââââââââââââââââââââââââââââââââââââââââââââââ

namespace
{

/** Format a state's hierarchical path (e.g. "Root.Combat.Attack"). */
FString GetStatePath(const UStateTreeState* State)
{
	if (!State)
	{
		return TEXT("(null)");
	}

	TArray<FString> PathParts;
	const UStateTreeState* Current = State;
	while (Current)
	{
		PathParts.Insert(Current->Name.ToString(), 0);
		Current = Current->Parent;
	}

	return FString::Join(PathParts, TEXT("."));
}

/** Recursively collect all states from a tree into a flat array with their paths. */
struct FStateEntry
{
	UStateTreeState* State;
	FString Path;
	FGuid ID;
};

void CollectStatesRecursive(UStateTreeState* State, TArray<FStateEntry>& OutEntries)
{
	if (!State)
	{
		return;
	}

	FStateEntry Entry;
	Entry.State = State;
	Entry.Path = GetStatePath(State);
	Entry.ID = State->ID;
	OutEntries.Add(Entry);

	for (UStateTreeState* Child : State->Children)
	{
		CollectStatesRecursive(Child, OutEntries);
	}
}

void CollectAllStates(UStateTreeEditorData* EditorData, TArray<FStateEntry>& OutEntries)
{
	if (!EditorData)
	{
		return;
	}

	for (UStateTreeState* SubTree : EditorData->SubTrees)
	{
		CollectStatesRecursive(SubTree, OutEntries);
	}
}

/** Match states between two collections by GUID first, then by name. */
struct FStatePair
{
	FStateEntry* EntryA = nullptr;
	FStateEntry* EntryB = nullptr;
};

TArray<FStatePair> MatchStates(TArray<FStateEntry>& StatesA, TArray<FStateEntry>& StatesB)
{
	TArray<FStatePair> Pairs;
	TSet<int32> MatchedB;

	// First pass: match by GUID
	for (FStateEntry& EntryA : StatesA)
	{
		bool bFound = false;
		for (int32 i = 0; i < StatesB.Num(); ++i)
		{
			if (!MatchedB.Contains(i) && EntryA.ID == StatesB[i].ID)
			{
				FStatePair Pair;
				Pair.EntryA = &EntryA;
				Pair.EntryB = &StatesB[i];
				Pairs.Add(Pair);
				MatchedB.Add(i);
				bFound = true;
				break;
			}
		}

		if (!bFound)
		{
			// Second pass: match by path
			for (int32 i = 0; i < StatesB.Num(); ++i)
			{
				if (!MatchedB.Contains(i) && EntryA.Path == StatesB[i].Path)
				{
					FStatePair Pair;
					Pair.EntryA = &EntryA;
					Pair.EntryB = &StatesB[i];
					Pairs.Add(Pair);
					MatchedB.Add(i);
					bFound = true;
					break;
				}
			}
		}

		if (!bFound)
		{
			// Only in A
			FStatePair Pair;
			Pair.EntryA = &EntryA;
			Pairs.Add(Pair);
		}
	}

	// States only in B
	for (int32 i = 0; i < StatesB.Num(); ++i)
	{
		if (!MatchedB.Contains(i))
		{
			FStatePair Pair;
			Pair.EntryB = &StatesB[i];
			Pairs.Add(Pair);
		}
	}

	return Pairs;
}

/** Match editor nodes by GUID between two arrays. */
struct FNodePair
{
	const FStateTreeEditorNode* NodeA = nullptr;
	const FStateTreeEditorNode* NodeB = nullptr;
	FString Label;
};

TArray<FNodePair> MatchEditorNodes(
	const TArray<FStateTreeEditorNode>& NodesA,
	const TArray<FStateTreeEditorNode>& NodesB,
	const FString& TypeLabel)
{
	TArray<FNodePair> Pairs;
	TSet<int32> MatchedB;

	for (const FStateTreeEditorNode& NodeA : NodesA)
	{
		bool bFound = false;
		for (int32 i = 0; i < NodesB.Num(); ++i)
		{
			if (!MatchedB.Contains(i) && NodeA.ID == NodesB[i].ID)
			{
				FNodePair Pair;
				Pair.NodeA = &NodeA;
				Pair.NodeB = &NodesB[i];
				Pair.Label = FString::Printf(TEXT("[%s] %s"),
					*TypeLabel, *ClaireonStateTreeHelpers::FormatEditorNode(NodeA));
				Pairs.Add(Pair);
				MatchedB.Add(i);
				bFound = true;
				break;
			}
		}

		if (!bFound)
		{
			FNodePair Pair;
			Pair.NodeA = &NodeA;
			Pair.Label = FString::Printf(TEXT("[%s] %s"),
				*TypeLabel, *ClaireonStateTreeHelpers::FormatEditorNode(NodeA));
			Pairs.Add(Pair);
		}
	}

	for (int32 i = 0; i < NodesB.Num(); ++i)
	{
		if (!MatchedB.Contains(i))
		{
			FNodePair Pair;
			Pair.NodeB = &NodesB[i];
			Pair.Label = FString::Printf(TEXT("[%s] %s"),
				*TypeLabel, *ClaireonStateTreeHelpers::FormatEditorNode(NodesB[i]));
			Pairs.Add(Pair);
		}
	}

	return Pairs;
}

/** Compare two editor node arrays and return diff text. */
bool CompareNodeArrays(
	const TArray<FStateTreeEditorNode>& NodesA,
	const TArray<FStateTreeEditorNode>& NodesB,
	const FString& TypeLabel,
	ClaireonDiffHelpers::EDiffResolution Resolution,
	int32& OutDiffCount,
	FString& OutText)
{
	TArray<FNodePair> Pairs = MatchEditorNodes(NodesA, NodesB, TypeLabel);

	for (const FNodePair& Pair : Pairs)
	{
		if (Pair.NodeA && !Pair.NodeB)
		{
			++OutDiffCount;
			if (Resolution == ClaireonDiffHelpers::EDiffResolution::Exists)
			{
				return true; // early out
			}
			OutText += FString::Printf(TEXT("- %s [AddedToA]\n"), *Pair.Label);
		}
		else if (!Pair.NodeA && Pair.NodeB)
		{
			++OutDiffCount;
			if (Resolution == ClaireonDiffHelpers::EDiffResolution::Exists)
			{
				return true;
			}
			OutText += FString::Printf(TEXT("- %s [AddedToB]\n"), *Pair.Label);
		}
		else if (Pair.NodeA && Pair.NodeB)
		{
			// Compare the node instances
			const UScriptStruct* StructA = Pair.NodeA->Node.GetScriptStruct();
			const UScriptStruct* StructB = Pair.NodeB->Node.GetScriptStruct();

			if (StructA != StructB)
			{
				++OutDiffCount;
				if (Resolution == ClaireonDiffHelpers::EDiffResolution::Exists)
				{
					return true;
				}
				OutText += FString::Printf(TEXT("- %s [TypeChanged]\n"), *Pair.Label);
			}
			else if (StructA && StructB)
			{
				// Compare struct data
				const uint8* DataA = Pair.NodeA->Node.GetMemory();
				const uint8* DataB = Pair.NodeB->Node.GetMemory();

				if (DataA && DataB)
				{
					TArray<FSingleObjectDiffEntry> NodeDiffs;
					DiffUtils::CompareUnrelatedStructs(StructA, DataA, nullptr, StructB, DataB, nullptr, NodeDiffs);

					if (NodeDiffs.Num() > 0)
					{
						++OutDiffCount;
						if (Resolution == ClaireonDiffHelpers::EDiffResolution::Exists)
						{
							return true;
						}
						OutText += FString::Printf(TEXT("- %s [Changed] (%d properties differ)\n"),
							*Pair.Label, NodeDiffs.Num());
					}
				}
			}
		}
	}

	return false; // did not early-out
}

} // anonymous namespace

// ââ Tool Interface ââââââââââââââââââââââââââââââââââââââââââââââââââââââ

FString ClaireonTool_StateTreeDiff::GetName() const
{
	return TEXT("claireon.statetree_diff");
}

FString ClaireonTool_StateTreeDiff::GetDescription() const
{
	return TEXT("Diff two State Tree assets structurally: states, tasks, conditions, transitions, evaluators, global tasks, and bindings. Stateless / read-only / non-session: never mutates and requires no open session. Each side is an asset path with an optional git revision. Use 'sections' to scope and 'resolution' for detail (exists/summary/detailed).");
}

TSharedPtr<FJsonObject> ClaireonTool_StateTreeDiff::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path_a (required)
	TSharedPtr<FJsonObject> PathAProp = MakeShared<FJsonObject>();
	PathAProp->SetStringField(TEXT("type"), TEXT("string"));
	PathAProp->SetStringField(TEXT("description"), TEXT("Unreal content path for side A (e.g. /Game/AI/ST_Enemy)"));
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
	SectionsProp->SetStringField(TEXT("description"), TEXT("Which sections to include: 'states', 'global', 'bindings'. Default: all three."));
	{
		TSharedPtr<FJsonObject> ItemSchema = MakeShared<FJsonObject>();
		ItemSchema->SetStringField(TEXT("type"), TEXT("string"));
		TArray<TSharedPtr<FJsonValue>> ItemEnum;
		ItemEnum.Add(MakeShared<FJsonValueString>(TEXT("states")));
		ItemEnum.Add(MakeShared<FJsonValueString>(TEXT("global")));
		ItemEnum.Add(MakeShared<FJsonValueString>(TEXT("bindings")));
		ItemSchema->SetArrayField(TEXT("enum"), ItemEnum);
		SectionsProp->SetObjectField(TEXT("items"), ItemSchema);
	}
	Properties->SetObjectField(TEXT("sections"), SectionsProp);

	// property_filter (optional)
	TSharedPtr<FJsonObject> FilterProp = MakeShared<FJsonObject>();
	FilterProp->SetStringField(TEXT("type"), TEXT("array"));
	FilterProp->SetStringField(TEXT("description"), TEXT("Optional list of property names to filter state property diffs."));
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

IClaireonTool::FToolResult ClaireonTool_StateTreeDiff::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	const UStateTree* STA = Cast<UStateTree>(SideA.Object);
	const UStateTree* STB = Cast<UStateTree>(SideB.Object);

	if (!STA || !STB)
	{
		return MakeErrorResult(TEXT("One or both assets are not State Trees"));
	}

	// Get editor data
	UStateTreeEditorData* EditorDataA = Cast<UStateTreeEditorData>(STA->EditorData);
	UStateTreeEditorData* EditorDataB = Cast<UStateTreeEditorData>(STB->EditorData);

	if (!EditorDataA || !EditorDataB)
	{
		return MakeErrorResult(TEXT("State Tree editor data not available"));
	}

	// Collect states and match them
	TArray<FStateEntry> StatesA;
	TArray<FStateEntry> StatesB;
	CollectAllStates(EditorDataA, StatesA);
	CollectAllStates(EditorDataB, StatesB);

	TArray<FStatePair> Pairs = MatchStates(StatesA, StatesB);

	int32 StatesAdded = 0;
	int32 StatesRemoved = 0;
	int32 TransitionsChanged = 0;

	for (const FStatePair& Pair : Pairs)
	{
		if (Pair.EntryA && !Pair.EntryB)
		{
			StatesRemoved++;
		}
		else if (!Pair.EntryA && Pair.EntryB)
		{
			StatesAdded++;
		}
		else if (Pair.EntryA && Pair.EntryB)
		{
			// Compare transitions
			const int32 TransitionsA = Pair.EntryA->State->Transitions.Num();
			const int32 TransitionsB = Pair.EntryB->State->Transitions.Num();
			if (TransitionsA != TransitionsB)
			{
				TransitionsChanged++;
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("path"), PathA);
	Data->SetNumberField(TEXT("states_added"), StatesAdded);
	Data->SetNumberField(TEXT("states_removed"), StatesRemoved);
	Data->SetNumberField(TEXT("transitions_changed"), TransitionsChanged);

	const FString Summary = FString::Printf(TEXT("ST diff: %d state%s added, %d transition%s changed"),
		StatesAdded, StatesAdded == 1 ? TEXT("") : TEXT("s"),
		TransitionsChanged, TransitionsChanged == 1 ? TEXT("") : TEXT("s"));

	return MakeSuccessResult(Data, Summary);
}
