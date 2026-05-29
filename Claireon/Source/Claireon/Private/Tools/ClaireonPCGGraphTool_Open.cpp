// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_Open.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "PCGGraph.h"
#include "Misc/DateTime.h"
#include "Misc/Timespan.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_Open::GetOperation() const { return TEXT("open"); }

TArray<FString> ClaireonPCGGraphTool_Open::GetSearchKeywords() const
{
	return {TEXT("pcg"), TEXT("procedural"), TEXT("content"), TEXT("generation"), TEXT("graph"), TEXT("open"), TEXT("session")};
}

FString ClaireonPCGGraphTool_Open::GetDescription() const
{
	return TEXT("Open a PCG (Procedural Content Generation) Graph asset for transactional editing "
				"and lock it under tool name 'pcg_graph_edit' so only one cohort holds it at a time. "
				"Returns a session_id used by all subsequent PCG-graph session ops (add_node, "
				"connect, set_node_property, save, close). Also surfaces the asset editor when "
				"running headed.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_Open::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Path to the PCG Graph asset to edit."), true);
	return Builder.Build();
}

FToolResult ClaireonPCGGraphTool_Open::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	// Resolve path to canonical form
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	FString Error;
	UPCGGraph* Graph = ClaireonPCGGraphHelpers::LoadPCGGraphAsset(AssetPath, Error);
	if (!Graph)
	{
		return MakeErrorResult(Error);
	}

	EnsureDelegateRegistered();

	const FString ResolvedAssetPath = Graph->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, PCGSessionToolName);
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		const FTimespan Elapsed = FDateTime::UtcNow() - Blocker.LastAccessTime;
		return MakeErrorResult(FString::Printf(
			TEXT("Asset is locked by %s session %s (last activity %dm %ds ago). Close that session first."),
			*Blocker.ToolName, *Blocker.SessionId,
			static_cast<int32>(Elapsed.GetTotalMinutes()),
			static_cast<int32>(Elapsed.GetTotalSeconds()) % 60));
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *ResolvedAssetPath));
	}
	const FString SessionId = OpenResult.SessionId;

	// If ReusedExistingSession, still update tool data
	FPCGGraphEditToolData NewData;
	NewData.PCGGraph = Graph;
	NewData.LastOperationStatus = TEXT("Session opened");
	NewData.bSuppressOutput = false; // Always show full output on open
	ToolData.Add(SessionId, MoveTemp(NewData));

	ClaireonAssetUtils::OpenAssetEditorIfHeadless(Graph);

	FPCGGraphEditToolData* DataPtr = ToolData.Find(SessionId);
	return BuildStateResponse(SessionId, DataPtr);
}
