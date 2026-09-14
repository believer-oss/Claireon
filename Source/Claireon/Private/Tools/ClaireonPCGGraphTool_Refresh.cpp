// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_Refresh.h"

#include "ClaireonLog.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PCGComponent.h"
#include "PCGGraph.h"
#include "Tools/ClaireonPCGEditorSync.h"
#include "Tools/ClaireonPCGGraphEditToolBase.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "UObject/UObjectIterator.h"

using FToolResult = IClaireonTool::FToolResult;

namespace ClaireonPCGGraphTool_Refresh_anon
{
	/** Error result carrying the failed_phase field the other PCG tools report. */
	static FToolResult MakeValidateError(const FString& Message)
	{
		FToolResult Result;
		Result.bIsError = true;
		Result.ErrorMessage = Message;
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("family"), TEXT("pcg"));
		Data->SetStringField(TEXT("status"), TEXT("error"));
		Data->SetStringField(TEXT("failed_phase"), TEXT("validate"));
		Result.Data = Data;
		return Result;
	}

	/** Regenerate editor-world components using the graph, including GraphInstance wrappers. */
	static int32 RegenerateDependentComponents(UPCGGraph* Graph, int32& OutSkipped)
	{
		OutSkipped = 0;

		if (!IsValid(Graph) || !IsValid(GEditor))
		{
			return 0;
		}

		const UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
		if (!IsValid(EditorWorld))
		{
			return 0;
		}

		int32 Regenerated = 0;

		for (TObjectIterator<UPCGComponent> It; It; ++It)
		{
			UPCGComponent* Component = *It;
			if (!IsValid(Component) || Component->GetGraph() != Graph)
			{
				continue;
			}

			// Templates and archetypes are not live components; generating them would
			// dirty class defaults.
			if (Component->IsTemplate())
			{
				continue;
			}

			if (Component->GetWorld() != EditorWorld)
			{
				continue;
			}

			if (!IsValid(Component->GetOwner()))
			{
				continue;
			}

			if (Regenerated >= ClaireonPCGGraphTool_Refresh::MaxComponentsPerCall)
			{
				++OutSkipped;
				continue;
			}

			Component->DirtyGenerated(EPCGComponentDirtyFlag::Actor);
			Component->Generate();
			++Regenerated;
		}

		return Regenerated;
	}
} // namespace ClaireonPCGGraphTool_Refresh_anon

FString ClaireonPCGGraphTool_Refresh::GetCategory() const { return TEXT("pcg"); }
FString ClaireonPCGGraphTool_Refresh::GetOperation() const { return TEXT("refresh"); }

FString ClaireonPCGGraphTool_Refresh::GetDescription() const
{
	return TEXT("Refresh a PCG graph after MCP edits: notifies the graph, evicting the "
				"compiled-graph cache recursively through subgraphs, rebuilds the open asset "
				"editor's view so added nodes and wires appear, and optionally regenerates "
				"dependent PCGComponents (opt-in, expensive). Pass session_id for an open pcg "
				"session, or asset_path for a graph with none.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_Refresh::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Existing pcg edit session id (mutually exclusive with asset_path)."));
	S.AddString(TEXT("asset_path"), TEXT("PCG Graph asset path, for a graph with no open session. Fail-on-missing."));
	S.AddBoolean(TEXT("regenerate_components"), TEXT("Also dirty and regenerate placed PCGComponents that use this graph. Default false; expensive on heavy graphs."));
	S.AddBoolean(TEXT("include_parent_graphs"), TEXT("Also check graphs that reference this one, so a subgraph edit reports the parent being viewed. Default true."));
	return S.Build();
}

FToolResult ClaireonPCGGraphTool_Refresh::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	using namespace ClaireonPCGGraphTool_Refresh_anon;

	if (!Arguments.IsValid())
	{
		return MakeValidateError(TEXT("pcg_refresh: arguments object is null"));
	}

	FString SessionId;
	FString AssetPath;
	const bool bHasSessionId = Arguments->TryGetStringField(TEXT("session_id"), SessionId) && !SessionId.IsEmpty();
	const bool bHasAssetPath = Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.IsEmpty();

	if (bHasSessionId == bHasAssetPath)
	{
		return MakeValidateError(bHasSessionId
			? TEXT("pcg_refresh: provide exactly one of 'session_id' or 'asset_path', not both")
			: TEXT("pcg_refresh: provide exactly one of 'session_id' or 'asset_path'"));
	}

	UPCGGraph* Graph = nullptr;
	if (bHasSessionId)
	{
		FPCGGraphEditToolData* Data = ClaireonPCGGraphEditToolBase::ToolData.Find(SessionId);
		if (!Data || !Data->IsValid())
		{
			return MakeValidateError(FString::Printf(TEXT("pcg_refresh: session_id '%s' not found"), *SessionId));
		}
		Graph = Data->PCGGraph.Get();
	}
	else
	{
		FString LoadError;
		Graph = ClaireonPCGGraphHelpers::LoadPCGGraphAsset(AssetPath, LoadError);
		if (!IsValid(Graph))
		{
			return MakeValidateError(LoadError);
		}
	}

	if (!IsValid(Graph))
	{
		return MakeValidateError(TEXT("pcg_refresh: PCG Graph is no longer loaded"));
	}

	bool bRegenerateComponents = false;
	Arguments->TryGetBoolField(TEXT("regenerate_components"), bRegenerateComponents);
	bool bIncludeParents = true;
	Arguments->TryGetBoolField(TEXT("include_parent_graphs"), bIncludeParents);

	// Use Structural to invalidate compiled graphs and dependent parents.
	ClaireonPCGGraphHelpers::NotifyGraphChanged(Graph, EPCGChangeType::Structural);

	// Rebuild now for the report, then remove the pending request to avoid a second rebuild.
	const EPCGReconstructResult ReconstructResult = ClaireonPCGEditorSync::ReconstructOpenEditor(Graph);
	const EPCGEditorViewState ViewState = ClaireonPCGEditorSync::GetEditorViewState(Graph);
	ClaireonPCGEditorSync::CancelPendingReconstruct(Graph);

	const int32 ParentsRebuilt = bIncludeParents
		? ClaireonPCGEditorSync::ReconstructOpenParentEditors(Graph)
		: 0;

	int32 ComponentsRegenerated = 0;
	int32 ComponentsSkipped = 0;
	if (bRegenerateComponents)
	{
		ComponentsRegenerated = RegenerateDependentComponents(Graph, ComponentsSkipped);
	}

	const bool bReconstructed = (ReconstructResult == EPCGReconstructResult::Reconstructed);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("graph"), Graph->GetPathName());
	Data->SetBoolField(TEXT("notified"), true);
	Data->SetBoolField(TEXT("reconstructed"), bReconstructed);
	Data->SetBoolField(TEXT("reconstruct_available"), ClaireonPCGEditorSync::IsReconstructAvailable());
	Data->SetNumberField(TEXT("editors_reconstructed"), bReconstructed ? 1 : 0);
	Data->SetNumberField(TEXT("parents_reconstructed"), ParentsRebuilt);
	Data->SetNumberField(TEXT("components_regenerated"), ComponentsRegenerated);
	Data->SetNumberField(TEXT("components_skipped"), ComponentsSkipped);

	FString ReconstructNote;
	switch (ReconstructResult)
	{
	case EPCGReconstructResult::Reconstructed:
		ReconstructNote = TEXT("editor graph rebuilt");
		break;
	case EPCGReconstructResult::NothingToRefresh:
		ReconstructNote = TEXT("nothing stale to refresh");
		break;
	case EPCGReconstructResult::Unavailable:
		ReconstructNote = TEXT("editor view is stale and this engine's PCG editor classes could not be resolved");
		break;
	}
	Data->SetStringField(TEXT("reconstruct_status"), ReconstructNote);
	Data->SetStringField(TEXT("view_state"), ClaireonPCGEditorSync::DescribeViewState(ViewState));

	FString Summary = FString::Printf(TEXT("Refreshed %s: notified, %s"),
		*Graph->GetName(), *ReconstructNote);
	if (ReconstructResult == EPCGReconstructResult::Unavailable)
	{
		Summary += FString::Printf(TEXT(" -- %s"), *ClaireonPCGEditorSync::DescribeViewState(ViewState));
	}
	if (ParentsRebuilt > 0)
	{
		Summary += FString::Printf(TEXT("; rebuilt %d referencing graph(s) too"), ParentsRebuilt);
	}
	if (bRegenerateComponents)
	{
		Summary += FString::Printf(TEXT(", %d component(s) regenerated"), ComponentsRegenerated);
		if (ComponentsSkipped > 0)
		{
			Summary += FString::Printf(TEXT(" (%d skipped: cap of %d per call)"),
				ComponentsSkipped, MaxComponentsPerCall);
		}
	}

	return MakeSuccessResult(Data, Summary);
}
