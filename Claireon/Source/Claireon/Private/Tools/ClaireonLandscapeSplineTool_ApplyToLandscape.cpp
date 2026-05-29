// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeSplineTool_ApplyToLandscape.h"
#include "Tools/FToolSchemaBuilder.h"
#include "LandscapeInfo.h"
#include "LandscapeComponent.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeSplineTool_ApplyToLandscape::GetOperation() const { return TEXT("spline_apply_to_landscape"); }

FString ClaireonLandscapeSplineTool_ApplyToLandscape::GetDescription() const
{
    return TEXT("Apply spline deformation to the landscape heightmap and weightmap. Session-mode tool: open via landscape_spline_open first.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeSplineTool_ApplyToLandscape::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonLandscapeSplineTool_ApplyToLandscape::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FLandscapeSplineEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	ULandscapeInfo* LandscapeInfo = Data->LandscapeInfo.Get();
	if (!LandscapeInfo)
	{
		return MakeErrorResult(TEXT("No landscape info available for spline application"));
	}

	TSet<TObjectPtr<ULandscapeComponent>> ModifiedComponents;
	const bool bResult = LandscapeInfo->ApplySplines(false /*bOnlySelected*/, &ModifiedComponents);

	const int32 ModifiedCount = ModifiedComponents.Num();
	Data->LastOperationStatus = FString::Printf(
		TEXT("Applied splines to landscape (%d components modified)"), ModifiedCount);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("apply_to_landscape"));
	ResultData->SetBoolField(TEXT("success"), bResult);
	ResultData->SetNumberField(TEXT("modified_components"), ModifiedCount);
	return MakeSuccessResult(ResultData, Data->LastOperationStatus);
}
