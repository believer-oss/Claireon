// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeSplineEditToolBase.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonSessionManager.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeSplineSegment.h"
#include "LandscapeSplinesComponent.h"

using FToolResult = IClaireonTool::FToolResult;

// ============================================================================
// Static members
// ============================================================================

const TCHAR* ClaireonLandscapeSplineEditToolBase::LandscapeSplineSessionToolName = TEXT("landscape_spline_edit");
TMap<FString, FLandscapeSplineEditToolData> ClaireonLandscapeSplineEditToolBase::ToolData;
bool ClaireonLandscapeSplineEditToolBase::bDelegateRegistered = false;

void ClaireonLandscapeSplineEditToolBase::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == LandscapeSplineSessionToolName)
	{
		ToolData.Remove(Info.SessionId);
	}
}

void ClaireonLandscapeSplineEditToolBase::EnsureDelegateRegistered()
{
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonLandscapeSplineEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}
}

// ============================================================================
// Session helpers
// ============================================================================

bool ClaireonLandscapeSplineEditToolBase::RequireSession(
	const TSharedPtr<FJsonObject>& Arguments,
	FString& OutSessionId,
	FLandscapeSplineEditToolData*& OutData,
	FString& OutError)
{
	if (!Arguments->TryGetStringField(TEXT("session_id"), OutSessionId) || OutSessionId.IsEmpty())
	{
		OutError = TEXT("Missing required parameter: session_id");
		return false;
	}

	FMCPSession* Session = FClaireonSessionManager::Get().FindSession(OutSessionId);
	if (!Session)
	{
		OutError = FString::Printf(TEXT("Session not found or expired: %s"), *OutSessionId);
		return false;
	}

	OutData = ToolData.Find(OutSessionId);
	if (!OutData)
	{
		OutError = TEXT("Session tool data not found");
		return false;
	}

	if (!OutData->SplinesComponent.IsValid())
	{
		OutError = TEXT("Splines component no longer valid. Reopen session.");
		return false;
	}

	bool bSuppressOutput = false;
	if (Arguments->HasField(TEXT("suppress_output")))
	{
		bSuppressOutput = Arguments->GetBoolField(TEXT("suppress_output"));
	}
	OutData->bSuppressOutput = bSuppressOutput;

	return true;
}

// ============================================================================
// State response
// ============================================================================

FToolResult ClaireonLandscapeSplineEditToolBase::BuildStateResponse(const FString& SessionId, FLandscapeSplineEditToolData* Data)
{
	ULandscapeSplinesComponent* SplinesComp = Data->SplinesComponent.Get();
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("session_id"), SessionId);
	ResultData->SetStringField(TEXT("status"), Data->LastOperationStatus);

	if (Data->bSuppressOutput)
	{
		return MakeSuccessResult(ResultData, Data->LastOperationStatus);
	}

	// Control points
	const auto& ControlPoints = SplinesComp->GetControlPoints();
	TArray<TSharedPtr<FJsonValue>> CPArray;
	for (int32 i = 0; i < ControlPoints.Num(); ++i)
	{
		ULandscapeSplineControlPoint* CP = ControlPoints[i];
		TSharedPtr<FJsonObject> CPJson = MakeShared<FJsonObject>();
		CPJson->SetNumberField(TEXT("index"), i);

		TSharedPtr<FJsonObject> Loc = MakeShared<FJsonObject>();
		Loc->SetNumberField(TEXT("x"), CP->Location.X);
		Loc->SetNumberField(TEXT("y"), CP->Location.Y);
		Loc->SetNumberField(TEXT("z"), CP->Location.Z);
		CPJson->SetObjectField(TEXT("location"), Loc);

		TSharedPtr<FJsonObject> Rot = MakeShared<FJsonObject>();
		Rot->SetNumberField(TEXT("pitch"), CP->Rotation.Pitch);
		Rot->SetNumberField(TEXT("yaw"), CP->Rotation.Yaw);
		Rot->SetNumberField(TEXT("roll"), CP->Rotation.Roll);
		CPJson->SetObjectField(TEXT("rotation"), Rot);

		CPJson->SetNumberField(TEXT("width"), CP->Width);
		CPJson->SetNumberField(TEXT("side_falloff"), CP->SideFalloff);
		CPArray.Add(MakeShared<FJsonValueObject>(CPJson));
	}
	ResultData->SetArrayField(TEXT("control_points"), CPArray);

	// Segments
	const auto& Segments = SplinesComp->GetSegments();
	TArray<TSharedPtr<FJsonValue>> SegArray;
	for (int32 i = 0; i < Segments.Num(); ++i)
	{
		ULandscapeSplineSegment* Seg = Segments[i];
		TSharedPtr<FJsonObject> SegJson = MakeShared<FJsonObject>();
		SegJson->SetNumberField(TEXT("index"), i);

		// Find control point indices
		for (int32 EndIdx = 0; EndIdx < 2; ++EndIdx)
		{
			ULandscapeSplineControlPoint* ConnPoint = Seg->Connections[EndIdx].ControlPoint;
			int32 CPIndex = INDEX_NONE;
			if (ConnPoint)
			{
				CPIndex = ControlPoints.Find(ConnPoint);
			}
			SegJson->SetNumberField(
				EndIdx == 0 ? TEXT("start_index") : TEXT("end_index"), CPIndex);
		}

		SegJson->SetStringField(TEXT("layer_name"), Seg->LayerName.ToString());
		SegJson->SetBoolField(TEXT("raise_terrain"), Seg->bRaiseTerrain);
		SegJson->SetBoolField(TEXT("lower_terrain"), Seg->bLowerTerrain);
		SegArray.Add(MakeShared<FJsonValueObject>(SegJson));
	}
	ResultData->SetArrayField(TEXT("segments"), SegArray);

	ResultData->SetNumberField(TEXT("focused_control_point_index"), Data->FocusedControlPointIndex);

	const FString SplineProxyPath = Data->LandscapeProxy.IsValid() ? Data->LandscapeProxy->GetPathName() : FString();
	FString SessionHintSummaryTag;
	ClaireonAssetUtils::EmitSessionHintIfNeeded(ResultData, Data->ConsecutiveAssetPathCalls, SplineProxyPath, SessionId, SessionHintSummaryTag);

	const FString Summary = FString::Printf(
		TEXT("Session %s: %d control points, %d segments -- %s"),
		*SessionId, ControlPoints.Num(), Segments.Num(), *Data->LastOperationStatus);
	return MakeSuccessResult(ResultData, Summary + SessionHintSummaryTag);
}
