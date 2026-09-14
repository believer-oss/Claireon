// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintGraphEditToolBase.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonLog.h"
#include "ClaireonBlueprintHelpers.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "EdGraphUtilities.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonSafeExec.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase_Internal.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "GraphEditor.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;

TMap<FString, FBlueprintEditToolData> ClaireonBlueprintGraphEditToolBase::ToolData;
bool ClaireonBlueprintGraphEditToolBase::bDelegateRegistered = false;

// Prefix helpers to avoid unity-build collisions.
namespace ClaireonBlueprintGraphEditToolBaseInternal
{

/** Identity of a link endpoint. */
FString BPEditBase_EndpointKey(const FGuid& NodeGuid, const FGuid& PinId)
{
	return FString::Printf(TEXT("%s/%s"),
		*NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
		*PinId.ToString(EGuidFormats::DigitsWithHyphens));
}

/**
 * Label live endpoints by title and deleted endpoints by captured class and GUID.
 * Labels do not participate in identity comparison.
 */
FString BPEditBase_DescribeEndpoint(UEdGraph* Graph, const FClaireonBPSnapshot& PreOp, const FGuid& NodeGuid)
{
	if (UEdGraphNode* Live = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, NodeGuid); IsValid(Live))
	{
		return Live->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	}
	if (const FClaireonBPNodeSnapshot* PreNode = PreOp.FindNode(NodeGuid))
	{
		FString ClassName = PreNode->NodeClassPath;
		int32 DotIndex = INDEX_NONE;
		if (ClassName.FindLastChar(TEXT('.'), DotIndex))
		{
			ClassName = ClassName.RightChop(DotIndex + 1);
		}
		return FString::Printf(TEXT("deleted %s [GUID: %s]"),
			*ClassName, *NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	}
	return FString::Printf(TEXT("[GUID: %s]"), *NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
}

/** Log asset and phase on entry so the last line identifies where a failure occurred. */
FString BPEditBase_SaveLogPrefix(const UBlueprint* Blueprint, const TCHAR* Phase)
{
	// Use object paths to distinguish assets sharing a package.
	const FString AssetPath = IsValid(Blueprint) ? Blueprint->GetPathName() : TEXT("(unresolved)");
	return FString::Printf(TEXT("[EditBlueprintGraph] Save phase=%s asset=%s:"), Phase, *AssetPath);
}

} // namespace ClaireonBlueprintGraphEditToolBaseInternal


void ClaireonBlueprintGraphEditToolBase::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == TEXT("bp"))
	{
		ToolData.Remove(Info.SessionId);
	}
}


FString ClaireonBlueprintGraphEditToolBase::BuildAvailableGraphsList(const UBlueprint* Blueprint) const
{
	TArray<FString> Names;
	if (IsValid(Blueprint))
	{
		for (UEdGraph* Graph : Blueprint->UbergraphPages)       { if (IsValid(Graph)) Names.Add(Graph->GetName()); }
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)       { if (IsValid(Graph)) Names.Add(Graph->GetName()); }
		for (UEdGraph* Graph : Blueprint->MacroGraphs)          { if (IsValid(Graph)) Names.Add(Graph->GetName()); }
		for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs) { if (IsValid(Graph)) Names.Add(Graph->GetName()); }
	}
	if (Names.Num() == 0)
	{
		return TEXT("(no graphs)");
	}
	return FString::Join(Names, TEXT(", "));
}

bool ClaireonBlueprintGraphEditToolBase::CompileAndSaveSession(
	FBlueprintEditToolData* Data,
	FString& OutSavedPathOrError,
	TArray<FString>& OutWarnings)
{
	using ClaireonBlueprintGraphEditToolBaseInternal::BPEditBase_SaveLogPrefix;

	UBlueprint* Blueprint = Data ? Data->Blueprint.Get() : nullptr;
	if (!IsValid(Blueprint))
	{
		UE_LOG(LogClaireon, Warning, TEXT("%s Blueprint is no longer valid"),
			*BPEditBase_SaveLogPrefix(nullptr, TEXT("resolve")));
		OutSavedPathOrError = TEXT("Blueprint is no longer valid");
		return false;
	}

	UPackage* Package = Blueprint->GetOutermost();
	if (!IsValid(Package))
	{
		UE_LOG(LogClaireon, Warning, TEXT("%s Failed to get package for Blueprint"),
			*BPEditBase_SaveLogPrefix(Blueprint, TEXT("resolve")));
		OutSavedPathOrError = TEXT("Failed to get package for Blueprint");
		return false;
	}

	// Scrub trashed LinkedTo references before compile/save; serializing them can assert or corrupt the asset.
	UE_LOG(LogClaireon, Log, TEXT("%s Scrubbing trashed pin links"),
		*BPEditBase_SaveLogPrefix(Blueprint, TEXT("scrub")));
	TArray<FString> ScrubDetails;
	const int32 ScrubbedRefs = ClaireonBPGraphInternal::ScrubTrashedPinLinks(Blueprint, ScrubDetails);
	if (ScrubbedRefs > 0)
	{
		for (const FString& Detail : ScrubDetails)
		{
			UE_LOG(LogClaireon, Warning, TEXT("%s %s"),
				*BPEditBase_SaveLogPrefix(Blueprint, TEXT("scrub")), *Detail);
		}
		OutWarnings.Add(FString::Printf(
			TEXT("Scrubbed %d stale reference(s) to trashed pins before save (see log for details); the graph had dangling links from a prior split/recombine or reconstruct."),
			ScrubbedRefs));
	}

	UE_LOG(LogClaireon, Log, TEXT("%s Compiling Blueprint before save"),
		*BPEditBase_SaveLogPrefix(Blueprint, TEXT("compile")));
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);

	Package->SetIsExternallyReferenceable(true);
	Package->MarkPackageDirty();

	const FString PackageFileName = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());

	UE_LOG(LogClaireon, Log, TEXT("%s Attempting to save to %s"),
		*BPEditBase_SaveLogPrefix(Blueprint, TEXT("save")), *PackageFileName);

	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		OutSavedPathOrError = TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor.");
		return false;
	}

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_None;

	if (UPackage::SavePackage(Package, Blueprint, *PackageFileName, SaveArgs))
	{
		UE_LOG(LogClaireon, Log, TEXT("%s Successfully saved Blueprint to %s"),
			*BPEditBase_SaveLogPrefix(Blueprint, TEXT("save")), *PackageFileName);
		OutSavedPathOrError = PackageFileName;
		return true;
	}

	UE_LOG(LogClaireon, Error, TEXT("%s Failed to save Blueprint to %s"),
		*BPEditBase_SaveLogPrefix(Blueprint, TEXT("save")), *PackageFileName);

	// A sharing violation can indicate another editor still holds the asset; report the file and recovery procedure.
	const FString PathHint = FString::Printf(
		TEXT(" If this is a 'sharing violation' or 'file in use' error, a previously-"
			 "crashed UnrealEditor process may still be holding %s. Run "
			 "`Get-Process UnrealEditor` (Windows) or `ps aux | grep UnrealEditor` "
			 "(Linux) and stop any stale processes, then retry. As a stronger fix, "
			 "call claireon.proxy with command='launch_editor' to rebuild+relaunch."),
		*PackageFileName);
	OutSavedPathOrError = FString::Printf(
		TEXT("Failed to save Blueprint to %s.%s"), *PackageFileName, *PathHint);
	return false;
}

FToolResult ClaireonBlueprintGraphEditToolBase::BuildStateResponse(const FString& SessionId, FBlueprintEditToolData* Data)
{
	// Deliberately NOT Data->IsValid(): that requires a live Graph as well, and a
	// session on a MacroLibrary/Interface Blueprint legitimately has none until
	// bp_add_macro or bp_add_function creates one. Reporting state is exactly what
	// such a session needs to be able to do; the graph-less branch below handles it.
	if (!Data || !Data->Blueprint.IsValid())
	{
		return MakeErrorResult(TEXT("Invalid session"));
	}

	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();

	// Validate cursor
	ValidateCursor(Data);

	// =========================================================================
	// Determine effective response mode
	// =========================================================================
	// bSuppressOutput maps to "status"
	FString EffectiveMode = Data->ResponseMode;
	if (Data->bSuppressOutput && EffectiveMode == TEXT("changed"))
	{
		EffectiveMode = TEXT("status");
	}

	// "changed" falls back to "status" when no affected nodes were recorded
	// (e.g. non-connectivity ops like save/format, or open which bypasses this path)
	if (EffectiveMode == TEXT("changed") && Data->LastOperationAffectedNodes.IsEmpty())
	{
		EffectiveMode = TEXT("status");
	}

	// =========================================================================
	// Structured response data (session_id contract: callers read Data.session_id
	// instead of grepping the Summary's "Session ID:" line).
	// See CLAIREON_BP_SESSION_ID_PROPOSAL.md.
	// =========================================================================
	// An EMPTY SessionId is the read-only contract from BeginReadOnlySessionOp: the asset
	// was resolved but no session was registered, so nothing here may imply one exists.
	const bool bReadOnlyNoSession = SessionId.IsEmpty();

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetStringField(TEXT("asset_path"),
		Data->Blueprint.IsValid() ? Data->Blueprint->GetPathName() : FString());
	ResponseData->SetStringField(TEXT("graph_name"),
		Data->Graph.IsValid() ? Data->Graph->GetName() : FString());
	ResponseData->SetStringField(TEXT("response_mode"), EffectiveMode);
	if (bReadOnlyNoSession)
	{
		ResponseData->SetBoolField(TEXT("read_only"), true);
	}

	// Report window effects on every call, including asset_path auto-opens, and revalidate the current binding.
	{
		TSharedPtr<FJsonObject> WindowData = MakeShared<FJsonObject>();
		WindowData->SetStringField(TEXT("state"),
			ClaireonAssetEditorWindow::ToWireString(Data->EditorWindow.State));
		if (!Data->EditorWindow.Reason.IsEmpty())
		{
			WindowData->SetStringField(TEXT("reason"), Data->EditorWindow.Reason);
		}
		WindowData->SetStringField(TEXT("binding"),
			ClaireonEditorBindingStatusToWireString(Data->EditorBinding.Revalidate(Graph)));
		ResponseData->SetObjectField(TEXT("editor_window"), WindowData);
	}

	// Nudge after sustained auto-opens: calls 6, 11, 16, and so on. Explicit session IDs reset the count.
	// Read-only access creates no session and needs no nudge.
	TSharedPtr<FJsonObject> SessionHint;
	if (!bReadOnlyNoSession)
	{
		const FString HintAssetPath = Data->Blueprint.IsValid() ? Data->Blueprint->GetPathName() : TEXT("<unknown>");
		ClaireonAssetUtils::EmitSessionHintIfNeeded(ResponseData, Data->ConsecutiveAssetPathCalls, HintAssetPath, SessionId, GetName(), SessionHint);
	}

	// =========================================================================
	// Surface GUID corrections so the MCP client can update stale references
	// =========================================================================
	FString GuidCorrectionNote;
	if (Data->GuidCorrections.Num() > 0)
	{
		GuidCorrectionNote = TEXT("\n\n## GUID Corrections (blueprint was recompiled — update your references)\n");
		for (const auto& Pair : Data->GuidCorrections)
		{
			GuidCorrectionNote += FString::Printf(TEXT("  %s → %s\n"),
				*Pair.Key.ToString(), *Pair.Value.ToString());
		}
		Data->GuidCorrections.Empty();
	}

	// =========================================================================
	// "status" mode — brief status line only
	// =========================================================================
	if (EffectiveMode == TEXT("status"))
	{
		FString StatusMsg = Data->Cursor.LastOperationStatus.IsEmpty() ? TEXT("ok") : FString::Printf(TEXT("ok: %s"), *Data->Cursor.LastOperationStatus);
		return MakeSuccessResultWithHint(ResponseData, StatusMsg + GuidCorrectionNote, SessionHint);
	}

	// =========================================================================
	// No active graph. MacroLibrary and Interface Blueprints have no ubergraph
	// (FBlueprintEditorUtils::DoesSupportEventGraphs is false for both), so a
	// session on one legitimately has Data->Graph == null until bp_add_macro or
	// bp_add_function creates a graph. Both the "changed" and "full" blocks below
	// dereference Graph unconditionally, so answer here instead.
	// =========================================================================
	if (!IsValid(Graph))
	{
		FString NoGraphText;
		if (!Data->Cursor.LastOperationStatus.IsEmpty())
		{
			NoGraphText += FString::Printf(TEXT("## Status\n%s\n\n"), *Data->Cursor.LastOperationStatus);
		}
		NoGraphText += FString::Printf(TEXT("## Session\nSession ID: %s\nBlueprint: %s\n\n"),
			bReadOnlyNoSession ? TEXT("(none -- read-only, no session opened)") : *SessionId,
			IsValid(Blueprint) ? *Blueprint->GetPathName() : TEXT("<invalid>"));
		NoGraphText += TEXT("Graph: (none -- no active graph; call bp_add_macro to create one, then bp_add_node targets it)\n");
		return MakeSuccessResultWithHint(ResponseData, NoGraphText + GuidCorrectionNote, SessionHint);
	}

	// =========================================================================
	// "changed" mode — pin-level diff of affected nodes
	// =========================================================================
	if (EffectiveMode == TEXT("changed"))
	{
		int32 TotalNodes = Graph->Nodes.Num();
		int32 AffectedCount = Data->LastOperationAffectedNodes.Num();

		FString DiffText;

		// Status header
		if (!Data->Cursor.LastOperationStatus.IsEmpty())
		{
			DiffText += FString::Printf(TEXT("## Status\n%s\n\n"), *Data->Cursor.LastOperationStatus);
		}

		DiffText += FString::Printf(TEXT("## Changed nodes (%d of %d):\n\n"), AffectedCount, TotalNodes);

		for (const FGuid& AffGuid : Data->LastOperationAffectedNodes)
		{
			// Find the node in the current graph
			UEdGraphNode* AffNode = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, AffGuid);
			if (!IsValid(AffNode))
			{
				continue;
			}

			FString NodeTitle = AffNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			FString NodeClass = AffNode->GetClass()->GetName();

			DiffText += FString::Printf(TEXT("[%s] (%s) [GUID: %s]\n"),
				*NodeTitle, *NodeClass, *AffGuid.ToString());

			// Per-pin diff, keyed by PinId and by the connected node's GUID.
			bool bAnyPinDiff = false;
			const FClaireonBPNodeSnapshot* PreNode = Data->PreOpSnapshot.FindNode(AffGuid);

			// Track live pins so removed pre-op pins can also be reported.
			TSet<FGuid> SeenPinIds;

			for (UEdGraphPin* DiffPin : AffNode->Pins)
			{
				if (!DiffPin)
				{
					continue;
				}
				SeenPinIds.Add(DiffPin->PinId);

				const FString DirArrow = (DiffPin->Direction == EGPD_Output) ? TEXT("->") : TEXT("<-");
				const FClaireonBPPinSnapshot* PrePin = PreNode ? PreNode->Pins.Find(DiffPin->PinId) : nullptr;

				TSet<FString> CurrentLinks;
				TMap<FString, FString> CurrentLabels;
				for (UEdGraphPin* LinkedDiff : DiffPin->LinkedTo)
				{
					if (!LinkedDiff || !IsValid(LinkedDiff->GetOwningNodeUnchecked()))
					{
						continue;
					}
					const UEdGraphNode* Other = LinkedDiff->GetOwningNodeUnchecked();
					const FString Key = ClaireonBlueprintGraphEditToolBaseInternal::BPEditBase_EndpointKey(Other->NodeGuid, LinkedDiff->PinId);
					CurrentLinks.Add(Key);
					CurrentLabels.Add(Key, Other->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
				}

				TSet<FString> PreviousLinks;
				TMap<FString, FGuid> PreviousNodeGuids;
				if (PrePin)
				{
					for (const FClaireonBPLinkEndpoint& Endpoint : PrePin->Links)
					{
						const FString Key = ClaireonBlueprintGraphEditToolBaseInternal::BPEditBase_EndpointKey(Endpoint.NodeGuid, Endpoint.PinId);
						PreviousLinks.Add(Key);
						PreviousNodeGuids.Add(Key, Endpoint.NodeGuid);
					}
				}

				// A preserved PinId makes an in-place name change a rename.
				if (PrePin && PrePin->PinName != DiffPin->PinName)
				{
					DiffText += FString::Printf(TEXT("  RENAMED: %s -> %s(%s)\n"),
						*PrePin->PinName.ToString(),
						*DiffPin->PinName.ToString(),
						*DiffPin->PinType.PinCategory.ToString());
					bAnyPinDiff = true;
				}

				// Report orphan transitions even when pin identity survives.
				if (DiffPin->bOrphanedPin && (!PrePin || !PrePin->bOrphaned))
				{
					DiffText += FString::Printf(TEXT("  ORPHANED: %s(%s) (no longer matched by the reconstructed node)\n"),
						*DiffPin->PinName.ToString(),
						*DiffPin->PinType.PinCategory.ToString());
					bAnyPinDiff = true;
				}

				// Sort rendered entries for deterministic output.
				TArray<FString> AddedKeys = CurrentLinks.Difference(PreviousLinks).Array();
				AddedKeys.Sort();
				TArray<FString> RemovedKeys = PreviousLinks.Difference(CurrentLinks).Array();
				RemovedKeys.Sort();

				for (const FString& Key : AddedKeys)
				{
					DiffText += FString::Printf(TEXT("  ADDED:   %s(%s) %s [%s]\n"),
						*DiffPin->PinName.ToString(),
						*DiffPin->PinType.PinCategory.ToString(),
						*DirArrow,
						*CurrentLabels[Key]);
					bAnyPinDiff = true;
				}

				for (const FString& Key : RemovedKeys)
				{
					const FString Label = ClaireonBlueprintGraphEditToolBaseInternal::BPEditBase_DescribeEndpoint(
						Graph, Data->PreOpSnapshot, PreviousNodeGuids[Key]);
					DiffText += FString::Printf(TEXT("  REMOVED: %s(%s) %s [%s]%s\n"),
						*DiffPin->PinName.ToString(),
						*DiffPin->PinType.PinCategory.ToString(),
						*DirArrow,
						*Label,
						CurrentLinks.IsEmpty() ? TEXT(" (now unconnected)") : TEXT(""));
					bAnyPinDiff = true;
				}
			}

			// Report removed pins and their previous links.
			if (PreNode)
			{
				TArray<const FClaireonBPPinSnapshot*> VanishedPins;
				for (const TPair<FGuid, FClaireonBPPinSnapshot>& PrePair : PreNode->Pins)
				{
					if (!SeenPinIds.Contains(PrePair.Key))
					{
						VanishedPins.Add(&PrePair.Value);
					}
				}
				VanishedPins.Sort([](const FClaireonBPPinSnapshot& A, const FClaireonBPPinSnapshot& B)
				{
					return A.PinName != B.PinName
						? A.PinName.LexicalLess(B.PinName)
						: A.PinId < B.PinId;
				});

				for (const FClaireonBPPinSnapshot* PrePin : VanishedPins)
				{
					const FString DirArrow = PrePin->bIsInput ? TEXT("<-") : TEXT("->");
					if (PrePin->Links.IsEmpty())
					{
						DiffText += FString::Printf(TEXT("  PIN REMOVED: %s\n"),
							*PrePin->PinName.ToString());
					}
					for (const FClaireonBPLinkEndpoint& Endpoint : PrePin->Links)
					{
						const FString Label = ClaireonBlueprintGraphEditToolBaseInternal::BPEditBase_DescribeEndpoint(
							Graph, Data->PreOpSnapshot, Endpoint.NodeGuid);
						DiffText += FString::Printf(TEXT("  PIN REMOVED: %s %s [%s]\n"),
							*PrePin->PinName.ToString(),
							*DirArrow,
							*Label);
					}
					bAnyPinDiff = true;
				}
			}

			if (!bAnyPinDiff)
			{
				DiffText += TEXT("  (exec connections unchanged)\n");
			}

			DiffText += TEXT("\n");
		}

		DiffText += FString::Printf(
			TEXT("(Full graph: %d nodes. Use bp_get_graph to see all.)"),
			TotalNodes);

		return MakeSuccessResultWithHint(ResponseData, DiffText + GuidCorrectionNote, SessionHint);
	}

	// =========================================================================
	// "full" mode — full graph state (JSON + T3D); also the fallback
	// =========================================================================
	{
		// Part 1: Operation status + Cursor info + Graph state summary
		FString StatusText;

		// Operation status
		if (!Data->Cursor.LastOperationStatus.IsEmpty())
		{
			StatusText += FString::Printf(TEXT("## Status\n%s\n\n"), *Data->Cursor.LastOperationStatus);
		}

		// Session info
		StatusText += FString::Printf(TEXT("## Session\nSession ID: %s\nBlueprint: %s\nGraph: %s\n\n"),
			*SessionId,
			*Blueprint->GetPathName(),
			*Graph->GetName());

		// Cursor info
		StatusText += TEXT("## Cursor\n");
		if (Data->Cursor.FocusedNodeGuid.IsValid())
		{
			UEdGraphNode* FocusedNode = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, Data->Cursor.FocusedNodeGuid);
			if (IsValid(FocusedNode))
			{
				StatusText += FString::Printf(TEXT("Focused Node: %s [GUID: %s]\n"),
					*FocusedNode->GetNodeTitle(ENodeTitleType::ListView).ToString(),
					*Data->Cursor.FocusedNodeGuid.ToString());

				if (Data->Cursor.FocusedPinName != NAME_None)
				{
					UEdGraphPin* FocusedPin = FocusedNode->FindPin(Data->Cursor.FocusedPinName, Data->Cursor.FocusedPinDirection);
					if (FocusedPin)
					{
						FString PinDir = (FocusedPin->Direction == EGPD_Input) ? TEXT("input") : TEXT("output");
						StatusText += FString::Printf(TEXT("Focused Pin: %s (%s, %s)\n"),
							*FocusedPin->PinName.ToString(),
							*PinDir,
							*FocusedPin->PinType.PinCategory.ToString());
					}
				}

				StatusText += FString::Printf(TEXT("Position: (%d, %d)\n"), FocusedNode->NodePosX, FocusedNode->NodePosY);
			}
			else
			{
				StatusText += TEXT("Focused Node: (invalid)\n");
			}
		}
		else
		{
			StatusText += TEXT("Focused Node: (none)\n");
		}
		StatusText += TEXT("\n");

		// Implemented interfaces (read-only reference pattern from
		// ClaireonTool_GetBlueprintProperties.cpp:214-222).
		StatusText += TEXT("## Interfaces\n");
		int32 InterfaceCount = 0;
		for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
		{
			if (IsValid(Interface.Interface))
			{
				StatusText += FString::Printf(TEXT("- %s\n"), *Interface.Interface->GetName());
				++InterfaceCount;
			}
		}
		if (InterfaceCount == 0)
		{
			StatusText += TEXT("(none)\n");
		}
		StatusText += TEXT("\n");

		// Graph state summary
		StatusText += FString::Printf(TEXT("## Graph State: %s (%d nodes)\n\n"), *Graph->GetName(), Graph->Nodes.Num());

		// List nodes with simple format
		int32 NodeIndex = 1;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node))
			{
				continue;
			}

			FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			bool bIsCursor = (Node->NodeGuid == Data->Cursor.FocusedNodeGuid);

			StatusText += FString::Printf(TEXT("%d. [%s] @ (%d, %d)%s\n"),
				NodeIndex++,
				*NodeTitle,
				Node->NodePosX,
				Node->NodePosY,
				bIsCursor ? TEXT(" <<<CURSOR>>>") : TEXT(""));

			// Show execution connections (simplified)
			TArray<UEdGraphPin*> ExecOutputs = ClaireonBlueprintHelpers::GetExecPins(Node, false, true);
			for (UEdGraphPin* ExecPin : ExecOutputs)
			{
				if (ExecPin->LinkedTo.Num() > 0)
				{
					for (UEdGraphPin* LinkedPin : ExecPin->LinkedTo)
					{
						if (LinkedPin && IsValid(LinkedPin->GetOwningNode()))
						{
							FString LinkedTitle = LinkedPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString();
							StatusText += FString::Printf(TEXT("   -> exec -> [%s]\n"), *LinkedTitle);
						}
					}
				}
			}
		}

		// Part 2: T3D export (if nodes exist)
		if (Graph->Nodes.Num() > 0)
		{
			// Convert TArray to TSet for FEdGraphUtilities::ExportNodesToText
			TSet<UObject*> NodeSet;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (IsValid(Node))
				{
					NodeSet.Add(Node);
				}
			}

			FString T3DText;
			FEdGraphUtilities::ExportNodesToText(NodeSet, T3DText);

			if (!T3DText.IsEmpty())
			{
				StatusText += FString::Printf(TEXT("\n## T3D Export\n\n```\n%s\n```"), *T3DText);
			}
		}

		return MakeSuccessResultWithHint(ResponseData, StatusText + GuidCorrectionNote, SessionHint);
	}
}

void ClaireonBlueprintGraphEditToolBase::ValidateCursor(FBlueprintEditToolData* Data)
{
	if (!Data || !Data->Graph.IsValid())
	{
		return;
	}

	// Check if focused node still exists
	if (Data->Cursor.FocusedNodeGuid.IsValid())
	{
		UEdGraphNode* Node = ClaireonBlueprintHelpers::FindNodeByGuid(Data->Graph.Get(), Data->Cursor.FocusedNodeGuid);
		if (!IsValid(Node))
		{
			// Node was deleted, reset cursor to first root node
			TArray<UEdGraphNode*> RootNodes = ClaireonBlueprintHelpers::FindRootNodes(Data->Graph.Get());
			if (RootNodes.Num() > 0)
			{
				Data->Cursor.FocusedNodeGuid = RootNodes[0]->NodeGuid;
				UEdGraphPin* FirstOutput = ClaireonBlueprintHelpers::GetFirstOutputPin(RootNodes[0]);
				if (FirstOutput)
				{
					Data->Cursor.FocusedPinName = FirstOutput->PinName;
					Data->Cursor.FocusedPinDirection = FirstOutput->Direction;
				}
			}
			else
			{
				// No nodes left, reset cursor
				Data->Cursor.FocusedNodeGuid = FGuid();
				Data->Cursor.FocusedPinName = NAME_None;
			}
		}
	}
}

bool ClaireonBlueprintGraphEditToolBase::ResolveTargetNode(
	const TSharedPtr<FJsonObject>& Params,
	UEdGraph* Graph,
	UEdGraphNode*& OutNode,
	FToolResult& OutError)
{
	OutNode = nullptr;

	if (!IsValid(Graph))
	{
		OutError = MakeErrorResult(TEXT("ResolveTargetNode: Graph is null"));
		return false;
	}

	FString NodeGuidStr;
	if (Params.IsValid() && Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
	{
		FString ResolveError;
		if (!ClaireonBlueprintHelpers::ResolveNodeGuidString(Graph, NodeGuidStr, OutNode, ResolveError))
		{
			OutError = MakeErrorResult(ResolveError);
			return false;
		}
		return true;
	}

	FString NodeTitle;
	if (Params.IsValid() && Params->TryGetStringField(TEXT("node_title"), NodeTitle))
	{
		TArray<UEdGraphNode*> Matches = ClaireonBlueprintHelpers::FindNodesByTitle(Graph, NodeTitle, /*bExactMatch=*/true);
		if (Matches.Num() != 1)
		{
			OutError = MakeErrorResult(ClaireonBlueprintHelpers::FormatTitleMatchFailure(
				Graph, NodeTitle, Matches, TEXT("node_guid")));
			return false;
		}
		OutNode = Matches[0];
		return true;
	}

	OutError = MakeErrorResult(TEXT("Missing required field: node_guid or node_title"));
	return false;
}

void ClaireonBlueprintGraphEditToolBase::EnsureSessionEditorWindow(FBlueprintEditToolData& Data)
{
	UBlueprint* Blueprint = Data.Blueprint.Get();
	if (!IsValid(Blueprint))
	{
		return;
	}
	UEdGraph* Graph = Data.Graph.Get();

	const EClaireonEditorBindingStatus Status = Data.EditorBinding.Revalidate(Graph);
	if (Status == EClaireonEditorBindingStatus::Valid)
	{
		// Report already_open per call rather than repeating the session's initial opening effect.
		Data.EditorWindow.State = EClaireonEditorWindowState::AlreadyOpen;
		Data.EditorWindow.Availability = EClaireonEditorAvailability::Available;
		Data.EditorWindow.Reason.Empty();
		return;
	}
	if (Status != EClaireonEditorBindingStatus::NotBound)
	{
		// Report a dead binding without adopting another editor or rewriting opening history.
		return;
	}

	// Keep the editor open beyond this call for the session.
	FScopedBlueprintEditor ScopedEditor(Blueprint, /*bInSilent=*/false, /*bInCloseOnDestroy=*/false);
	Data.EditorWindow = ScopedEditor.GetOpenOutcome();
	if (ScopedEditor.IsValid() && IsValid(Graph))
	{
		Data.EditorBinding.BindTo(Blueprint, Graph,
			ScopedEditor.GetBlueprintEditor(), ScopedEditor.GetGraphEditor(Graph));
	}
}

void ClaireonBlueprintGraphEditToolBase::MirrorCursorToView(FBlueprintEditToolData& Data)
{
	UEdGraph* Graph = Data.Graph.Get();
	if (!IsValid(Graph) || !Data.Cursor.FocusedNodeGuid.IsValid())
	{
		return;
	}

	EClaireonEditorBindingStatus Status = EClaireonEditorBindingStatus::NotBound;
	TSharedPtr<SGraphEditor> Widget = Data.EditorBinding.ResolveGraphEditor(Graph, Status);
	if (!Widget.IsValid())
	{
		return;
	}

	UEdGraphNode* Node = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, Data.Cursor.FocusedNodeGuid);
	if (!IsValid(Node))
	{
		return;
	}

	// Cursor movement must preserve selection; JumpToNode otherwise selects by default.
	Widget->JumpToNode(Node, /*bRequestRename=*/false, /*bSelectNode=*/false);
}

void ClaireonBlueprintGraphEditToolBase::InitToolDataForSession(const FString& SessionId, UBlueprint* Blueprint, UEdGraph* Graph)
{
	FBlueprintEditToolData NewData;
	NewData.Blueprint = Blueprint;
	NewData.Graph = Graph;
	// Macro libraries and interfaces may have no graph yet.
	NewData.Cursor.GraphName = IsValid(Graph) ? Graph->GetName() : FString();
	NewData.Cursor.ViewportCenter = FVector2D(0.0f, 0.0f);

	if (IsValid(Graph))
	{
		TArray<UEdGraphNode*> RootNodes = ClaireonBlueprintHelpers::FindRootNodes(Graph);
		if (RootNodes.Num() > 0)
		{
			UEdGraphNode* FirstNode = RootNodes[0];
			NewData.Cursor.FocusedNodeGuid = FirstNode->NodeGuid;
			UEdGraphPin* FirstOutput = ClaireonBlueprintHelpers::GetFirstOutputPin(FirstNode);
			if (FirstOutput)
			{
				NewData.Cursor.FocusedPinName = FirstOutput->PinName;
				NewData.Cursor.FocusedPinDirection = FirstOutput->Direction;
			}
		}
	}

	EnsureSessionEditorWindow(NewData);

	ToolData.Add(SessionId, MoveTemp(NewData));
	// Re-find ToolData after Add because map rehashing can invalidate prior pointers.
}

bool ClaireonBlueprintGraphEditToolBase::BeginSessionOp(
	const TSharedPtr<FJsonObject>& Arguments,
	const FString& OperationName,
	TSharedPtr<FJsonObject>& OutParams,
	FString& OutSessionId,
	FBlueprintEditToolData*& OutData,
	FToolResult& OutError)
{
	OutSessionId.Reset();
	OutData = nullptr;

	TSharedPtr<FJsonObject> Params = Arguments.IsValid() ? Arguments : MakeShared<FJsonObject>();
	if (Params->HasField(TEXT("params")))
	{
		const TSharedPtr<FJsonObject>* NestedObj = nullptr;
		if (Params->TryGetObjectField(TEXT("params"), NestedObj) && NestedObj && NestedObj->IsValid())
		{
			Params = *NestedObj;
		}
	}

	bool bSuppressOutput = false;
	Arguments->TryGetBoolField(TEXT("suppress_output"), bSuppressOutput);

	FString ResponseMode = TEXT("changed");
	Arguments->TryGetStringField(TEXT("response_mode"), ResponseMode);
	if (bSuppressOutput && !Arguments->HasField(TEXT("response_mode")))
	{
		ResponseMode = TEXT("status");
	}

	if (!ResolveOrOpenSession(Params, OperationName, OutSessionId, OutData, OutError))
	{
		return false;
	}

	FClaireonSessionManager::Get().TouchSession(OutSessionId);
	OutData->bSuppressOutput = bSuppressOutput;
	OutData->ResponseMode = ResponseMode;
	OutData->LastOperationAffectedNodes.Empty();
	// Reset shared per-operation status before another tool uses the session.
	OutData->Cursor.LastOperationStatus = FString();

	// Capture after resolution and reset, before the first mutating handler.
	OutData->PreOpSnapshot = FClaireonBPSnapshot();
	if (UEdGraph* SnapGraph = OutData->Graph.Get(); IsValid(SnapGraph))
	{
		TArray<UEdGraph*> SnapGraphs;
		SnapGraphs.Add(SnapGraph);
		ClaireonBPSnapshot::CaptureForChangedDiff(SnapGraphs, OutData->PreOpSnapshot);
	}

	OutParams = Params;
	return true;
}

FToolResult ClaireonBlueprintGraphEditToolBase::CheckMutationAffectedNodes(const FString& OpName, FBlueprintEditToolData* Data, const FToolResult& Result)
{
	if (!Result.bIsError && Data && Data->ResponseMode == TEXT("changed") && Data->LastOperationAffectedNodes.IsEmpty())
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("response_mode=changed: no affected nodes recorded after mutation op '%s' — check handler"),
			*OpName);
	}
	return Result;
}

bool ClaireonBlueprintGraphEditToolBase::ResolveBlueprintAndGraph(
	const TSharedPtr<FJsonObject>& Params,
	FString& InOutAssetPath,
	UBlueprint*& OutBlueprint,
	UEdGraph*& OutGraph,
	FToolResult& OutError)
{
	OutBlueprint = nullptr;
	OutGraph = nullptr;

	// Canonicalize via the same resolver bp_open uses.
	auto ResolveResult = ClaireonPathResolver::Resolve(InOutAssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = MakeErrorResult(ResolveResult.Error);
		return false;
	}
	InOutAssetPath = ResolveResult.ResolvedPath.Path;

	OutBlueprint = LoadObject<UBlueprint>(nullptr, *InOutAssetPath);
	if (!IsValid(OutBlueprint))
	{
		OutError = MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint: %s"), *InOutAssetPath));
		return false;
	}

	// Graph: default to EventGraph, or caller-supplied graph_name. Mirrors
	// bp_open's default handling.
	FString GraphName;
	const bool bGraphNameExplicit = Params->TryGetStringField(TEXT("graph_name"), GraphName);
	if (!bGraphNameExplicit)
	{
		GraphName = TEXT("EventGraph");
	}
	OutGraph = ClaireonBlueprintHelpers::FindGraphByName(OutBlueprint, GraphName);
	if (!IsValid(OutGraph))
	{
		// P2-12: a BPTYPE_Normal Blueprint's ubergraph page is not always named
		// "EventGraph" (anim-notify-state BPs rename it), so the DEFAULT lookup
		// falls back to the first ubergraph page before erroring.
		if (!bGraphNameExplicit && OutBlueprint->UbergraphPages.Num() > 0
			&& IsValid(OutBlueprint->UbergraphPages[0]))
		{
			OutGraph = OutBlueprint->UbergraphPages[0];
			return true;
		}

		// A MacroLibrary/Interface Blueprint has no EventGraph, and a freshly created
		// one has no graphs at all -- erroring on the DEFAULT graph name would make
		// bp_add_macro unable to auto-open the very asset it exists to populate.
		// Open graph-less instead. An explicitly named missing graph still errors.
		// P2-12: the type test is DoesSupportEventGraphs (the sibling code's
		// idiom), not BlueprintType == BPTYPE_Normal -- the old test let
		// BPTYPE_LevelScript open graph-less and hard-errored renamed-ubergraph
		// normal BPs the fallback above now handles.
		const bool bSupportsEventGraph = FBlueprintEditorUtils::DoesSupportEventGraphs(OutBlueprint);
		if (bGraphNameExplicit || bSupportsEventGraph)
		{
			OutError = MakeErrorResult(FString::Printf(TEXT("Graph '%s' not found in Blueprint %s"), *GraphName, *InOutAssetPath));
			return false;
		}
	}

	return true;
}

bool ClaireonBlueprintGraphEditToolBase::BeginReadOnlySessionOp(
	const TSharedPtr<FJsonObject>& Arguments,
	const FString& OperationName,
	TSharedPtr<FJsonObject>& OutParams,
	FString& OutSessionId,
	FBlueprintEditToolData*& OutData,
	FToolResult& OutError)
{
	OutSessionId.Reset();
	OutData = nullptr;

	TSharedPtr<FJsonObject> Params = Arguments.IsValid() ? Arguments : MakeShared<FJsonObject>();
	if (Params->HasField(TEXT("params")))
	{
		const TSharedPtr<FJsonObject>* NestedObj = nullptr;
		if (Params->TryGetObjectField(TEXT("params"), NestedObj) && NestedObj && NestedObj->IsValid())
		{
			Params = *NestedObj;
		}
	}
	OutParams = Params;

	// An explicit session_id means the caller already owns a session and is asking us
	// to read within it. Reusing it is correct -- and skipping it would be worse, since
	// the response would stop reporting the session state they are tracking. Only the
	// asset_path path, where WE would be the one to open a session, avoids doing so.
	FString SessionId;
	if (Params->TryGetStringField(TEXT("session_id"), SessionId) && !SessionId.IsEmpty())
	{
		return BeginSessionOp(Arguments, OperationName, OutParams, OutSessionId, OutData, OutError);
	}

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = MakeErrorResult(FString::Printf(
			TEXT("Missing 'session_id' (or 'asset_path') for operation '%s'. Supply one of: session_id (from a prior open/create) or asset_path (read-only, opens no session)."),
			*OperationName));
		return false;
	}

	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = nullptr;
	if (!ResolveBlueprintAndGraph(Params, AssetPath, Blueprint, Graph, OutError))
	{
		return false;
	}

	// Keep read-only scratch data outside the session map and reset it for each call.
	ReadOnlyScratchData = FBlueprintEditToolData();
	ReadOnlyScratchData.Blueprint = Blueprint;
	ReadOnlyScratchData.Graph = Graph;
	ReadOnlyScratchData.Cursor.GraphName = IsValid(Graph) ? Graph->GetName() : FString();
	// Read-only access intentionally opens no window.
	ReadOnlyScratchData.EditorWindow.Reason =
		TEXT("this call resolved the asset read-only and registered no session, so it opened no window");

	bool bSuppressOutput = false;
	Arguments->TryGetBoolField(TEXT("suppress_output"), bSuppressOutput);
	FString ResponseMode = TEXT("changed");
	Arguments->TryGetStringField(TEXT("response_mode"), ResponseMode);
	if (bSuppressOutput && !Arguments->HasField(TEXT("response_mode")))
	{
		ResponseMode = TEXT("status");
	}
	ReadOnlyScratchData.bSuppressOutput = bSuppressOutput;
	ReadOnlyScratchData.ResponseMode = ResponseMode;

	ReadOnlyScratchData.ConsecutiveAssetPathCalls = 0;

	OutSessionId = FString();
	OutData = &ReadOnlyScratchData;
	return true;
}

bool ClaireonBlueprintGraphEditToolBase::ResolveOrOpenSession(
	const TSharedPtr<FJsonObject>& Params,
	const FString& OperationName,
	FString& OutSessionId,
	FBlueprintEditToolData*& OutData,
	FToolResult& OutError)
{
	OutSessionId.Reset();
	OutData = nullptr;

	// Resolution order 1: explicit session_id wins.
	FString SessionId;
	if (Params->TryGetStringField(TEXT("session_id"), SessionId) && !SessionId.IsEmpty())
	{
		FMCPSession* MgrSession = FClaireonSessionManager::Get().FindSession(SessionId);
		if (!MgrSession)
		{
			OutError = MakeErrorResult(FString::Printf(TEXT("Invalid or expired session_id: %s"), *SessionId));
			return false;
		}

		FBlueprintEditToolData* Data = ToolData.Find(SessionId);
		if (!Data)
		{
			OutError = MakeErrorResult(FString::Printf(TEXT("Tool data not found for session_id: %s"), *SessionId));
			return false;
		}

		// Caller used an explicit session_id -- they understand the open/close
		// discipline, so reset the consecutive-auto-open nudge counter.
		Data->ConsecutiveAssetPathCalls = 0;

		OutSessionId = SessionId;
		OutData = Data;
		return true;
	}

	// Resolution order 2: asset_path triggers auto-open (or reuse existing
	// session for the same tool+asset via FClaireonSessionManager::OpenSession).
	FString AssetPath;
	if (Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.IsEmpty())
	{
		// Register delegate on first use (same guard as bp_open).
		if (!bDelegateRegistered)
		{
			FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonBlueprintGraphEditToolBase::HandleSessionClosed);
			bDelegateRegistered = true;
		}

		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
		if (!ResolveBlueprintAndGraph(Params, AssetPath, Blueprint, Graph, OutError))
		{
			return false;
		}

		// Open (or reuse) a session via the manager.
		double TimeoutMinutes = ClaireonDefaultSessionTimeoutMinutes;
		Params->TryGetNumberField(TEXT("timeout_minutes"), TimeoutMinutes);
		FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(AssetPath, TEXT("bp"), TimeoutMinutes);

		if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
		{
			const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
			const FTimespan Elapsed = FDateTime::UtcNow() - Blocker.LastAccessTime;
			OutError = MakeErrorResult(FString::Printf(
				TEXT("Asset is locked by %s session %s (last activity %dm %ds ago). Close that session first, or call session_release(session_id='%s') to force-release it."),
				*Blocker.ToolName, *Blocker.SessionId,
				static_cast<int32>(Elapsed.GetTotalMinutes()),
				static_cast<int32>(Elapsed.GetTotalSeconds()) % 60,
				*Blocker.SessionId));
			return false;
		}

		if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
		{
			OutError = MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
			return false;
		}

		// Success or ReusedExistingSession: populate ToolData if needed.
		const FString& OpenedSessionId = OpenResult.SessionId;
		FBlueprintEditToolData* Data = ToolData.Find(OpenedSessionId);
		if (!Data)
		{
			InitToolDataForSession(OpenedSessionId, Blueprint, Graph);
			Data = ToolData.Find(OpenedSessionId);
			UE_LOG(LogClaireon, Log,
				TEXT("[EditBlueprintGraph] Auto-opened session %s for Blueprint %s (op='%s' asset_path fallback)"),
				*OpenedSessionId, *Blueprint->GetPathName(), *OperationName);
		}

		if (!Data)
		{
			OutError = MakeErrorResult(FString::Printf(TEXT("Failed to initialize tool data for auto-opened session on asset %s"), *AssetPath));
			return false;
		}

		// Track consecutive asset_path resolutions on this session so
		// BuildStateResponse can emit a nudge toward explicit open/close discipline
		// after 5 (first hint at call 6, then every 5 past that).
		Data->ConsecutiveAssetPathCalls++;

		OutSessionId = OpenedSessionId;
		OutData = Data;
		return true;
	}

	// Resolution order 3: neither supplied.
	OutError = MakeErrorResult(FString::Printf(
		TEXT("Missing 'session_id' (or 'asset_path') for operation '%s'. Supply one of: session_id (from a prior open/create) or asset_path (will auto-open a session)."),
		*OperationName));
	return false;
}

#undef LOCTEXT_NAMESPACE
