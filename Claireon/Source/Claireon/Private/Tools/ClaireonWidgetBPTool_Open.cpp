// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_Open.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "ClaireonLog.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_Open::GetName() const
{
    return TEXT("claireon.widgetbp_open");
}

TArray<FString> ClaireonWidgetBPTool_Open::GetSearchKeywords() const
{
    return {TEXT("widgetbp"), TEXT("widget"), TEXT("umg"), TEXT("ui"), TEXT("hud"), TEXT("open"), TEXT("session")};
}

FString ClaireonWidgetBPTool_Open::GetDescription() const
{
    return TEXT("Open an existing Widget Blueprint for editing and acquire an asset lock. Creates or reuses a session keyed by the MCP client and returns the session_id plus initial widget tree. Transactional. The session must be closed via claireon.widgetbp_close to release the lock.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_Open::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("asset_path"), TEXT("Widget Blueprint asset path (e.g. '/Game/UI/WBP_MyWidget.WBP_MyWidget')."), true);
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_Open::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    // open is session-less; it creates the session.
    TSharedPtr<FJsonObject> Params = Arguments.IsValid() ? Arguments : MakeShared<FJsonObject>();
    if (Params->HasField(TEXT("params")))
    {
        const TSharedPtr<FJsonObject>* NestedObj = nullptr;
        if (Params->TryGetObjectField(TEXT("params"), NestedObj) && NestedObj && NestedObj->IsValid())
        {
            Params = *NestedObj;
        }
    }
	// Extract asset_path (required)
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path. open requires: asset_path (e.g. \"/Game/UI/WBP_MyWidget.WBP_MyWidget\")"));
	}

	// Resolve path to canonical form
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	// Load the widget blueprint
	UWidgetBlueprint* WBP = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath);
	if (!WBP)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Widget Blueprint: %s"), *AssetPath));
	}

	// Register delegate if not done yet
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonWidgetBPEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	// Use the canonical path from the loaded object as the lock key
	FString CanonicalPath = WBP->GetPathName();

	// Open session (acts as exclusive lock)
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(CanonicalPath, TEXT("claireon.widgetbp_edit"));
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
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *CanonicalPath));
	}
	const FString SessionId = OpenResult.SessionId;

	// Create and populate tool data
	FWidgetBPEditToolData NewData;
	NewData.WidgetBlueprint = WBP;
	NewData.bModified = false;
	NewData.LastCommandTime = FDateTime::Now();

	// Set initial focus to root widget if one exists
	if (WBP->WidgetTree && WBP->WidgetTree->RootWidget)
	{
		NewData.FocusedWidget = WBP->WidgetTree->RootWidget->GetFName();
	}

	ToolData.Add(SessionId, NewData);

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Opened session %s for %s"), *SessionId, *AssetPath);

	FWidgetBPEditToolData* LiveData = ToolData.Find(SessionId);
	if (!LiveData)
	{
		return MakeErrorResult(TEXT("Failed to create session after open"));
	}

	return BuildStateResponse(SessionId, LiveData);
}

