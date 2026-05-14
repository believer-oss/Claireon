// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeTool_AddLayer.h"
#include "Tools/FToolSchemaBuilder.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeTool_AddLayer::GetOperation() const { return TEXT("add_layer"); }

FString ClaireonLandscapeTool_AddLayer::GetDescription() const
{
	return TEXT("Add a new weight layer to the landscape.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeTool_AddLayer::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("layer_name"), TEXT("Name of the new weight layer."), true);
	Builder.AddBoolean(TEXT("no_weight_blend"), TEXT("If true, layer uses no weight blending."));
	return Builder.Build();
}

FToolResult ClaireonLandscapeTool_AddLayer::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FLandscapeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString LayerName;
	if (!Arguments->TryGetStringField(TEXT("layer_name"), LayerName) || LayerName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: layer_name"));
	}

	bool bNoWeightBlend = false;
	Arguments->TryGetBoolField(TEXT("no_weight_blend"), bNoWeightBlend);

	ULandscapeLayerInfoObject* LayerInfo = NewObject<ULandscapeLayerInfoObject>();
	LayerInfo->LayerName = FName(*LayerName);
	LayerInfo->bNoWeightBlend = bNoWeightBlend;

	ULandscapeInfo* LandscapeInfo = Data->LandscapeInfo.Get();
	ALandscapeProxy* Proxy = Data->LandscapeProxy.Get();

	LandscapeInfo->Layers.Add(FLandscapeInfoLayerSettings(LayerInfo, Proxy));

	// Build updated layer list
	TArray<TSharedPtr<FJsonValue>> LayerArray;
	for (const FLandscapeInfoLayerSettings& LayerSettings : LandscapeInfo->Layers)
	{
		LayerArray.Add(MakeShared<FJsonValueString>(LayerSettings.GetLayerName().ToString()));
	}

	Data->LastOperationStatus = FString::Printf(TEXT("Added layer '%s'"), *LayerName);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("add_layer"));
	ResultData->SetStringField(TEXT("layer_name"), LayerName);
	ResultData->SetArrayField(TEXT("layers"), LayerArray);
	return MakeSuccessResult(ResultData, Data->LastOperationStatus);
}
