// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeSplineTool_Open.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonLandscapeHelpers.h"
#include "ClaireonSessionManager.h"
#include "ClaireonLog.h"
#include "Editor.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"
#include "LandscapeSplinesComponent.h"
#include "Engine/World.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeSplineTool_Open::GetOperation() const { return TEXT("spline_open"); }

FString ClaireonLandscapeSplineTool_Open::GetDescription() const
{
	return TEXT("Open a session on a landscape's spline component. Returns a session_id for subsequent spline operations.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeSplineTool_Open::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("landscape_name"), TEXT("Name (or substring) of the landscape actor whose spline component to open."), true);
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("When true, response omits control points and segments arrays."));
	return Builder.Build();
}

FToolResult ClaireonLandscapeSplineTool_Open::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!IsValid(GEditor))
	{
		return MakeErrorResult(TEXT("Editor not available"));
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!IsValid(World))
	{
		return MakeErrorResult(TEXT("No editor world loaded"));
	}

	EnsureDelegateRegistered();

	FString LandscapeName;
	if (!Arguments->TryGetStringField(TEXT("landscape_name"), LandscapeName) || LandscapeName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: landscape_name"));
	}

	TArray<TPair<ULandscapeInfo*, ALandscapeProxy*>> Landscapes =
		ClaireonLandscapeHelpers::FindLandscapeInWorld(World, LandscapeName);

	if (Landscapes.Num() == 0)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("No landscape matching '%s' found in the current world"), *LandscapeName));
	}
	if (Landscapes.Num() > 1)
	{
		TArray<FString> Names;
		for (const auto& L : Landscapes)
		{
			Names.Add(L.Value->GetActorLabel());
		}
		return MakeErrorResult(FString::Printf(
			TEXT("Multiple landscapes match '%s': %s. Provide a more specific name."),
			*LandscapeName, *FString::Join(Names, TEXT(", "))));
	}

	ULandscapeInfo* LandscapeInfo = Landscapes[0].Key;
	ALandscapeProxy* Proxy = Landscapes[0].Value;

	// Get or create splines component
	ULandscapeSplinesComponent* SplinesComp = Proxy->GetSplinesComponent();
	if (!IsValid(SplinesComp))
	{
		Proxy->CreateSplineComponent();
		SplinesComp = Proxy->GetSplinesComponent();
		if (!IsValid(SplinesComp))
		{
			return MakeErrorResult(TEXT("Failed to create splines component on landscape"));
		}
	}

	const FString ActorPath = Proxy->GetPathName();
	FMCPOpenSessionResult SessionResult = FClaireonSessionManager::Get().OpenSession(ActorPath, LandscapeSplineSessionToolName);
	if (SessionResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		FString BlockInfo = TEXT("another tool");
		if (SessionResult.BlockingSession.IsSet())
		{
			BlockInfo = FString::Printf(TEXT("%s (session %s)"),
				*SessionResult.BlockingSession->ToolName, *SessionResult.BlockingSession->SessionId);
		}
		return MakeErrorResult(FString::Printf(TEXT("Landscape splines locked by %s"), *BlockInfo));
	}
	// Defect guard: this used to handle only BlockedByOtherTool. On
	// InvalidAssetPath (OpenSession's CanonicalizePath rejected the path) SessionId
	// is empty, and falling through returned a SUCCESS state response carrying an
	// empty session_id -- an unusable handle with no error. This is reachable in
	// normal use: CanonicalizePath rejects anything not under /Game/, and actors in
	// an unsaved map live under /Temp/Untitled_N, so "File > New Level, then call
	// this tool" used to yield a bogus success. Every non-success result must
	// produce an error here.
	if (SessionResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *ActorPath));
	}
	if (SessionResult.Result != EOpenSessionResult::Success && SessionResult.Result != EOpenSessionResult::ReusedExistingSession)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Failed to open a session for %s (unexpected OpenSession result %d)"),
			*ActorPath, static_cast<int32>(SessionResult.Result)));
	}

	const FString SessionId = SessionResult.SessionId;
	FLandscapeSplineEditToolData& Data = ToolData.FindOrAdd(SessionId);
	Data.SplinesComponent = SplinesComp;
	Data.LandscapeProxy = Proxy;
	Data.LandscapeInfo = LandscapeInfo;
	Data.FocusedControlPointIndex = INDEX_NONE;
	Data.LastOperationStatus = TEXT("Session opened");

	bool bSuppressOutput = false;
	Arguments->TryGetBoolField(TEXT("suppress_output"), bSuppressOutput);
	Data.bSuppressOutput = bSuppressOutput;

	// World-scoped tool: mode-focus (landscape edit mode) deferred per Q-AUTO-OPEN-WORLD-MODE;
	// EM_Landscape activation requires LevelEditor module not available in Claireon plugin scope.
	UE_LOG(LogClaireon, Verbose, TEXT("[LandscapeSplineTool_Open] Session opened; landscape edit-mode focus deferred (world-scoped)"));

	return BuildStateResponse(SessionId, &Data);
}
