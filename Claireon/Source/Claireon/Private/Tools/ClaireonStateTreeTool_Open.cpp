// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_Open.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "Misc/Paths.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_Open::GetName() const
{
	return TEXT("claireon.statetree_open");
}

TArray<FString> ClaireonStateTreeTool_Open::GetSearchKeywords() const
{
	return {TEXT("st"), TEXT("state"), TEXT("tree"), TEXT("statetree"), TEXT("hierarchical"), TEXT("open"), TEXT("session")};
}

FString ClaireonStateTreeTool_Open::GetDescription() const
{
	return TEXT("Open a State Tree asset for editing and acquire an asset lock. Returns a session_id used as the handle for all subsequent claireon.statetree_* operations. Transactional. The session must be closed via claireon.statetree_close to release the lock; only one session per asset at a time.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_Open::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Object path to the State Tree asset to edit."), true);
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("Return only a brief status."));
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_Open::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'open' requires 'asset_path'"));
	}

	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	FString Error;
	UStateTree* StateTree = ClaireonStateTreeHelpers::LoadStateTreeAsset(AssetPath, Error);
	if (!StateTree)
	{
		return MakeErrorResult(Error);
	}

	UStateTreeEditorData* EditorData = ClaireonStateTreeHelpers::GetEditorData(StateTree, Error);
	if (!EditorData)
	{
		return MakeErrorResult(Error);
	}

	EnsureDelegateRegistered();

	const FString ResolvedAssetPath = StateTree->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, StateTreeSessionToolName);
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

	FStateTreeEditToolData NewData;
	NewData.StateTree = StateTree;
	NewData.LastOperationStatus = TEXT("Opened session");

	// Set cursor to first subtree root
	if (EditorData->SubTrees.Num() > 0 && EditorData->SubTrees[0])
	{
		NewData.FocusedStateId = EditorData->SubTrees[0]->ID;
	}

	if (Arguments->HasField(TEXT("suppress_output")))
	{
		NewData.bSuppressOutput = Arguments->GetBoolField(TEXT("suppress_output"));
	}

	ToolData.Add(SessionId, MoveTemp(NewData));

	// Return full tree structure + session ID
	FString StructureText = ClaireonStateTreeHelpers::FormatStateTreeStructure(EditorData);

	TSharedPtr<FJsonObject> OpenData = MakeShared<FJsonObject>();
	OpenData->SetStringField(TEXT("session_id"), SessionId);
	OpenData->SetStringField(TEXT("asset_path"), AssetPath);
	OpenData->SetStringField(TEXT("status"), TEXT("Opened session"));
	ClaireonStateTreeEditInternal::ApplyStructuredSpill(
		*OpenData, TEXT("structure"), TEXT("structure_full"), StructureText);

	return MakeSuccessResult(OpenData, FString::Printf(TEXT("Opened session for %s"), *FPaths::GetBaseFilename(AssetPath)));
}
