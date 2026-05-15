// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeSplineTool_Open.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonLandscapeHelpers.h"
#include "ClaireonSessionManager.h"
#include "Editor.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"
#include "LandscapeSplinesComponent.h"
#include "Engine/World.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeSplineTool_Open::GetName() const
{
	return TEXT("claireon.landscape_spline_open");
}

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
	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor not available"));
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
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
	if (!SplinesComp)
	{
		Proxy->CreateSplineComponent();
		SplinesComp = Proxy->GetSplinesComponent();
		if (!SplinesComp)
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

	return BuildStateResponse(SessionId, &Data);
}
