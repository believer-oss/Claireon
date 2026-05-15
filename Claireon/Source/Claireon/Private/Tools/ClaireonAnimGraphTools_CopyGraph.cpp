// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimGraphTools_CopyGraph.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "Tools/ClaireonAnimGraphHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"

#include "Animation/AnimBlueprint.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"

#include "EdGraphUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_Variable.h"
#include "K2Node_FunctionEntry.h"
#include "AnimGraphNode_Root.h"
#include "EdGraphSchema_K2.h"

#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using FToolResult = IClaireonTool::FToolResult;

#define LOCTEXT_NAMESPACE "ClaireonAnimGraphTools_CopyGraph"

FString ClaireonAnimGraphTool_CopyGraph::GetName() const
{
	return TEXT("claireon.animgraph_copy_graph");
}

FString ClaireonAnimGraphTool_CopyGraph::GetDescription() const
{
	return TEXT("Copy a single graph's nodes (and any subobject graphs they own, such as state-machine "
		"state graphs and transition rule graphs) from one Animation Blueprint into another. "
		"Uses T3D round-trip so native UEdGraphNode serialization handles nested graphs automatically. "
		"Destination graph must already exist on the destination AnimBP; overwrite=true clears it first. "
		"A freshly-created AnimBP (via animgraph_create) already has its root AnimGraph auto-created and works "
		"as a destination — you don't need extra scaffolding to copy 'AnimGraph' into a fresh ABP. "
		"For custom-named graphs, scaffold them first via the session-based animgraph tools. "
		"Stateless — no session required.\n\n"
		"Known benign log noise: T3D paste of state-machine graphs that reference variables inside "
		"transition rule graphs can trigger a non-fatal UK2Node_Variable::FunctionParameterExists ensure. "
		"It's an engine-side edge case; the copied content is unaffected.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_CopyGraph::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("source_asset_path"), TEXT("Path to the source Animation Blueprint"), true);
	S.AddString(TEXT("source_graph_name"), TEXT("Name of the graph on the source AnimBP (case-insensitive)"), true);
	S.AddString(TEXT("destination_asset_path"), TEXT("Path to the destination Animation Blueprint (must already exist)"), true);
	S.AddString(TEXT("destination_graph_name"), TEXT("Name of the graph on the destination AnimBP. Defaults to source_graph_name."));
	S.AddBoolean(TEXT("overwrite"), TEXT("If true, clear existing nodes on the destination graph before copying. If false and destination has nodes, returns an error. Default: false."));
	S.AddBoolean(TEXT("copy_referenced_variables"), TEXT("If true, mirror all source BP variables onto the destination (any that aren't already present). Default: true."));
	S.AddBoolean(TEXT("copy_functions"), TEXT("If true, duplicate all source BP FunctionGraphs + UbergraphPages + MacroGraphs onto the destination. Needed for state-machine OnBecomeRelevant/OnUpdate bindings and any cross-graph function calls. Skips per-graph name collisions. Default: true."));
	S.AddBoolean(TEXT("compile"), TEXT("Compile the destination AnimBP after copy. Default: true."));
	S.AddBoolean(TEXT("save"), TEXT("Save the destination AnimBP after copy. Default: true."));
	return S.Build();
}

// ============================================================================
// Execute
// ============================================================================

FToolResult ClaireonAnimGraphTool_CopyGraph::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// -------- Parse inputs --------
	FString SourcePath;
	if (!Arguments->TryGetStringField(TEXT("source_asset_path"), SourcePath))
	{
		return MakeErrorResult(TEXT("Missing required field: source_asset_path"));
	}

	FString SourceGraphName;
	if (!Arguments->TryGetStringField(TEXT("source_graph_name"), SourceGraphName))
	{
		return MakeErrorResult(TEXT("Missing required field: source_graph_name"));
	}

	FString DestPath;
	if (!Arguments->TryGetStringField(TEXT("destination_asset_path"), DestPath))
	{
		return MakeErrorResult(TEXT("Missing required field: destination_asset_path"));
	}

	FString DestGraphName;
	if (!Arguments->TryGetStringField(TEXT("destination_graph_name"), DestGraphName) || DestGraphName.IsEmpty())
	{
		DestGraphName = SourceGraphName;
	}

	bool bOverwrite = false;
	Arguments->TryGetBoolField(TEXT("overwrite"), bOverwrite);

	bool bCopyVars = true;
	Arguments->TryGetBoolField(TEXT("copy_referenced_variables"), bCopyVars);

	bool bCopyFunctions = true;
	Arguments->TryGetBoolField(TEXT("copy_functions"), bCopyFunctions);

	bool bCompile = true;
	Arguments->TryGetBoolField(TEXT("compile"), bCompile);

	bool bSave = true;
	Arguments->TryGetBoolField(TEXT("save"), bSave);

	// -------- Resolve source --------
	auto SourceResolve = ClaireonPathResolver::Resolve(SourcePath);
	if (!SourceResolve.bSuccess)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to resolve source path: %s"), *SourceResolve.Error));
	}

	FString LoadError;
	UAnimBlueprint* SourceBP = ClaireonAnimGraphHelpers::LoadAnimBlueprint(SourceResolve.ResolvedPath.Path, LoadError);
	if (!SourceBP)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load source AnimBP: %s"), *LoadError));
	}

	UEdGraph* SourceGraph = ClaireonAnimGraphHelpers::FindAnimGraphByName(SourceBP, SourceGraphName, LoadError);
	if (!SourceGraph)
	{
		return MakeErrorResult(FString::Printf(TEXT("Source graph not found: %s"), *LoadError));
	}

	// -------- Resolve destination --------
	auto DestResolve = ClaireonPathResolver::Resolve(DestPath);
	if (!DestResolve.bSuccess)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to resolve destination path: %s"), *DestResolve.Error));
	}

	UAnimBlueprint* DestBP = ClaireonAnimGraphHelpers::LoadAnimBlueprint(DestResolve.ResolvedPath.Path, LoadError);
	if (!DestBP)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load destination AnimBP: %s"), *LoadError));
	}

	UEdGraph* DestGraph = ClaireonAnimGraphHelpers::FindAnimGraphByName(DestBP, DestGraphName, LoadError);
	if (!DestGraph)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Destination graph '%s' not found on %s. Create it first (e.g. via claireon.blueprint_edit_graph) — this tool does not create graphs. %s"),
			*DestGraphName, *DestBP->GetPathName(), *LoadError));
	}

	if (SourceGraph == DestGraph)
	{
		return MakeErrorResult(TEXT("Source and destination graphs are the same object — nothing to copy"));
	}

	// -------- Overwrite check --------
	TArray<FString> Warnings;
	const int32 ExistingNodeCount = DestGraph->Nodes.Num();
	if (ExistingNodeCount > 0 && !bOverwrite)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Destination graph '%s' already has %d node(s). Pass overwrite=true to clear it before copying."),
			*DestGraphName, ExistingNodeCount));
	}

	// -------- Copy --------
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Copy Graph")));
	DestBP->Modify();
	DestGraph->Modify();

	// Clear destination if overwriting — but preserve any UAnimGraphNode_Root instances.
	// The root (aka "OutputPose") is a singleton per AnimGraph; removing it leaves the asset
	// in an uncompilable state and T3D import won't replace it because the engine rejects
	// duplicate roots on paste. The source's root output connection must be reattached by
	// the caller if the copy inserts a node that should drive the pose output.
	int32 PreservedRootCount = 0;
	if (ExistingNodeCount > 0 && bOverwrite)
	{
		TArray<UEdGraphNode*> NodesToRemove = DestGraph->Nodes;
		for (UEdGraphNode* Node : NodesToRemove)
		{
			if (!Node) continue;
			if (Node->IsA<UAnimGraphNode_Root>())
			{
				++PreservedRootCount;
				continue;
			}
			Node->BreakAllNodeLinks();
			DestGraph->RemoveNode(Node);
		}
	}

	// Export source to T3D
	TSet<UObject*> NodesToExport;
	for (UEdGraphNode* Node : SourceGraph->Nodes)
	{
		if (Node)
		{
			NodesToExport.Add(Node);
		}
	}

	if (NodesToExport.Num() == 0)
	{
		Warnings.Add(TEXT("Source graph had zero nodes — destination graph left empty."));

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("nodes_copied"), 0);
		Data->SetNumberField(TEXT("nodes_cleared"), ExistingNodeCount);
		Data->SetNumberField(TEXT("variables_created"), 0);
		Data->SetStringField(TEXT("destination_asset_path"), DestBP->GetPathName());
		Data->SetStringField(TEXT("destination_graph_name"), DestGraphName);

		FToolResult R = MakeSuccessResult(Data, TEXT("Copy complete (source empty)"));
		R.Warnings = Warnings;
		return R;
	}

	FString T3DText;
	FEdGraphUtilities::ExportNodesToText(NodesToExport, T3DText);
	if (T3DText.IsEmpty())
	{
		return MakeErrorResult(TEXT("ExportNodesToText returned empty text"));
	}

	// Import into destination
	TSet<UEdGraphNode*> ImportedNodes;
	FEdGraphUtilities::ImportNodesFromText(DestGraph, T3DText, ImportedNodes);

	if (ImportedNodes.Num() == 0)
	{
		return MakeErrorResult(TEXT("ImportNodesFromText produced no nodes — T3D format may be incompatible"));
	}

	// -------- Mirror source variables --------
	// Walk the full NewVariables list on the source BP rather than scanning imported nodes.
	// The old scan missed refs in nested subobject graphs (state-machine inner graphs, transition
	// rule graphs) because ImportedNodes only contains the top-level UEdGraph->Nodes of the
	// destination graph. For replication workflows, mirroring all source BP variables is simpler
	// and avoids the nested-walk edge case that triggers PostPasteNode ensures on transition nodes.
	int32 VariablesCreated = 0;
	if (bCopyVars)
	{
		for (const FBPVariableDescription& SrcDesc : SourceBP->NewVariables)
		{
			if (SrcDesc.VarName.IsNone()) continue;
			if (FBlueprintEditorUtils::FindNewVariableIndex(DestBP, SrcDesc.VarName) != INDEX_NONE) continue;

			const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(DestBP, SrcDesc.VarName, SrcDesc.VarType);
			if (bAdded)
			{
				++VariablesCreated;
			}
			else
			{
				Warnings.Add(FString::Printf(
					TEXT("Failed to mirror variable '%s' onto destination"),
					*SrcDesc.VarName.ToString()));
			}
		}
	}

	// -------- Mirror blueprint-level graphs (functions, event graphs, macros) --------
	// Required for state-machine OnBecomeRelevant/OnUpdate bindings, transition helper
	// functions, and any CallFunction node in the copied graph that points at self.
	int32 FunctionGraphsCopied = 0;
	int32 UbergraphPagesCopied = 0;
	int32 MacroGraphsCopied = 0;
	int32 BPGraphsSkipped = 0;
	if (bCopyFunctions)
	{
		// Copy a list of blueprint-level graphs via T3D round-trip so PostPasteNode
		// retargets cross-BP FMemberReference / VariableReference usages to the destination
		// self-context. DuplicateObject is faster but does NOT run the post-paste fixup,
		// which leaves CallFunction nodes referencing the source class as external-member
		// and produces compile errors like "self is not <SourceClass>, Target must have a connection".
		auto CopyBPGraphList = [&](const TArray<TObjectPtr<UEdGraph>>& SrcList,
		                           TArray<TObjectPtr<UEdGraph>>& DstList,
		                           int32& OutCount,
		                           const TCHAR* Category)
		{
			TSet<FName> ExistingNames;
			for (const TObjectPtr<UEdGraph>& Existing : DstList)
			{
				if (Existing)
				{
					ExistingNames.Add(Existing->GetFName());
				}
			}

			for (const TObjectPtr<UEdGraph>& Src : SrcList)
			{
				if (!Src) continue;
				// Skip the graph we already copied via the main path (e.g., AnimGraph lives in FunctionGraphs too)
				if (Src.Get() == SourceGraph) continue;

				const FName SrcName = Src->GetFName();
				if (ExistingNames.Contains(SrcName))
				{
					++BPGraphsSkipped;
					Warnings.Add(FString::Printf(TEXT("%s '%s' already exists on destination — not overwritten"),
						Category, *SrcName.ToString()));
					continue;
				}

				// Determine schema class from source (preserves animgraph vs k2 vs state-machine schemas)
				const UEdGraphSchema* SrcSchema = Src->GetSchema();
				TSubclassOf<UEdGraphSchema> SchemaClass = SrcSchema
					? SrcSchema->GetClass()
					: UEdGraphSchema_K2::StaticClass();

				// Create empty graph on destination and register it BEFORE import so
				// PostPasteNode can walk the outer chain to find DestBP during retargeting.
				UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
					DestBP, SrcName, UEdGraph::StaticClass(), SchemaClass);
				if (!NewGraph)
				{
					Warnings.Add(FString::Printf(TEXT("%s '%s': CreateNewGraph returned null"),
						Category, *SrcName.ToString()));
					continue;
				}
				NewGraph->SetFlags(RF_Transactional | RF_Public);
				DstList.Add(NewGraph);
				ExistingNames.Add(SrcName);

				// Empty source graph — nothing to import, the empty destination is fine
				if (Src->Nodes.Num() == 0)
				{
					++OutCount;
					continue;
				}

				// Export source nodes to T3D
				TSet<UObject*> SrcNodesToExport;
				for (UEdGraphNode* N : Src->Nodes)
				{
					if (N) SrcNodesToExport.Add(N);
				}

				FString T3D;
				FEdGraphUtilities::ExportNodesToText(SrcNodesToExport, T3D);
				if (T3D.IsEmpty())
				{
					Warnings.Add(FString::Printf(TEXT("%s '%s': T3D export returned empty — graph copied but empty"),
						Category, *SrcName.ToString()));
					++OutCount;
					continue;
				}

				// Import — ImportNodesFromText invokes PostProcessPastedNodes which calls
				// PostPasteNode on each node, which in turn calls
				// FMemberReference::FixupSelfMemberReferencesOnPaste etc. — retargeting refs
				// from source class to destination self-context.
				TSet<UEdGraphNode*> ImportedFnNodes;
				FEdGraphUtilities::ImportNodesFromText(NewGraph, T3D, ImportedFnNodes);

				// Post-paste override repair: if this graph's name matches a parent-class
				// BlueprintEvent, the imported FunctionEntry has been "self-ified" by
				// FMemberReference::FixupSelfMemberReferencesOnPaste (bSelfContext=true,
				// MemberParent=null). The compiler's duplicate-name check at
				// KismetCompiler.cpp:3945-3969 only skips when GetMemberParentClass() is
				// non-null, so without this repair we get "The function name in node X is
				// already used" on every override. Mirrors Operation_AddFunctionOverride
				// at ClaireonTool_EditBlueprintGraph.cpp:5451.
				if (DestBP->ParentClass)
				{
					UFunction* ParentFunc = DestBP->ParentClass->FindFunctionByName(SrcName);
					if (ParentFunc)
					{
						for (UEdGraphNode* Imp : ImportedFnNodes)
						{
							if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Imp))
							{
								Entry->FunctionReference.SetExternalMember(SrcName, DestBP->ParentClass);
								Entry->ReconstructNode();
							}
						}
					}
				}

				++OutCount;
			}
		};

		CopyBPGraphList(SourceBP->FunctionGraphs, DestBP->FunctionGraphs, FunctionGraphsCopied, TEXT("FunctionGraph"));
		CopyBPGraphList(SourceBP->UbergraphPages, DestBP->UbergraphPages, UbergraphPagesCopied, TEXT("UbergraphPage"));
		CopyBPGraphList(SourceBP->MacroGraphs, DestBP->MacroGraphs, MacroGraphsCopied, TEXT("MacroGraph"));

		// Flag out-of-scope dependencies so the caller knows what we didn't cover
		if (SourceBP->ImplementedInterfaces.Num() > 0)
		{
			Warnings.Add(FString::Printf(
				TEXT("Source implements %d interface(s) — interface graphs are NOT copied by copy_graph. Replicate them manually on the destination if needed."),
				SourceBP->ImplementedInterfaces.Num()));
		}
	}

	// -------- Auto-wire preserved root to an imported pose source --------
	// When we preserved the destination root and source had exactly one root (the common case),
	// the imported set contains a node whose output should drive the root's input. Try each
	// imported node's output pins and let the schema validate compatibility. First match wins.
	bool bRootWired = false;
	if (PreservedRootCount == 1 && ImportedNodes.Num() > 0)
	{
		UAnimGraphNode_Root* DestRoot = nullptr;
		for (UEdGraphNode* N : DestGraph->Nodes)
		{
			if (UAnimGraphNode_Root* R = Cast<UAnimGraphNode_Root>(N))
			{
				DestRoot = R;
				break;
			}
		}

		if (DestRoot)
		{
			UEdGraphPin* RootInput = nullptr;
			for (UEdGraphPin* Pin : DestRoot->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input)
				{
					RootInput = Pin;
					break;
				}
			}

			if (RootInput && RootInput->LinkedTo.Num() == 0)
			{
				const UEdGraphSchema* DestSchema = DestGraph->GetSchema();
				for (UEdGraphNode* Imported : ImportedNodes)
				{
					if (!Imported) continue;
					for (UEdGraphPin* OutPin : Imported->Pins)
					{
						if (!OutPin || OutPin->Direction != EGPD_Output) continue;
						if (DestSchema && DestSchema->TryCreateConnection(OutPin, RootInput))
						{
							bRootWired = true;
							break;
						}
					}
					if (bRootWired) break;
				}
				if (!bRootWired)
				{
					Warnings.Add(TEXT("Preserved destination root left unwired — no imported node produced a pose-compatible output. Wire the root's input pin manually."));
				}
			}
		}
	}

	// -------- Finalize --------
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(DestBP);
	DestGraph->NotifyGraphChanged();

	if (bCompile)
	{
		// RefreshAllNodes walks every graph on the BP and calls ReconstructNode on each node.
		// Critical after copy_functions=true: CallFunction nodes in imported function graphs
		// cache pin layouts from the source compile, and those layouts become stale because
		// we re-created the target functions on the destination. Reconstructing rebuilds pins
		// from the current function signatures so the subsequent compile sees correct pin sets.
		FBlueprintEditorUtils::RefreshAllNodes(DestBP);
		FKismetEditorUtilities::CompileBlueprint(DestBP);
	}

	if (bSave)
	{
		FString SaveError;
		if (!ClaireonAssetUtils::SaveAsset(DestBP, SaveError))
		{
			Warnings.Add(FString::Printf(TEXT("Copy complete but save failed: %s"), *SaveError));
		}
	}

	// -------- Build response --------
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("nodes_copied"), ImportedNodes.Num());
	Data->SetNumberField(TEXT("nodes_cleared"), ExistingNodeCount);
	Data->SetNumberField(TEXT("variables_created"), VariablesCreated);
	Data->SetNumberField(TEXT("preserved_root_count"), PreservedRootCount);
	Data->SetBoolField(TEXT("root_auto_wired"), bRootWired);
	Data->SetNumberField(TEXT("function_graphs_copied"), FunctionGraphsCopied);
	Data->SetNumberField(TEXT("ubergraph_pages_copied"), UbergraphPagesCopied);
	Data->SetNumberField(TEXT("macro_graphs_copied"), MacroGraphsCopied);
	Data->SetNumberField(TEXT("blueprint_graphs_skipped"), BPGraphsSkipped);
	Data->SetStringField(TEXT("source_asset_path"), SourceBP->GetPathName());
	Data->SetStringField(TEXT("source_graph_name"), SourceGraphName);
	Data->SetStringField(TEXT("destination_asset_path"), DestBP->GetPathName());
	Data->SetStringField(TEXT("destination_graph_name"), DestGraphName);

	const FString Summary = FString::Printf(
		TEXT("Copied %d node(s) from %s::%s → %s::%s (cleared %d existing, mirrored %d var(s))"),
		ImportedNodes.Num(),
		*SourceBP->GetName(), *SourceGraphName,
		*DestBP->GetName(), *DestGraphName,
		ExistingNodeCount, VariablesCreated);

	FToolResult Result = MakeSuccessResult(Data, Summary);
	Result.Warnings = Warnings;
	return Result;
}

#undef LOCTEXT_NAMESPACE
