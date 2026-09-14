// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeTool_AddLayer.h"
#include "Tools/FToolSchemaBuilder.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "ClaireonPathResolver.h"
#include "Misc/EngineVersionComparison.h"
#if !UE_VERSION_OLDER_THAN(5, 7, 0)
#include "LandscapeEditTypes.h"
#endif

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeTool_AddLayer::GetOperation() const { return TEXT("add_layer"); }

FString ClaireonLandscapeTool_AddLayer::GetDescription() const
{
    return TEXT("Add a weight layer to the landscape in the current session, either reusing an "
                "existing LandscapeLayerInfoObject asset (layer_info_path) or creating one in the "
                "landscape's own package. The layer is registered on the runtime landscape info AND "
                "on the proxy's serialized EditorLayerSettings, so it survives a save/reload. "
                "Session-mode tool: open via landscape_open first.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeTool_AddLayer::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("layer_name"), TEXT("Name of the new weight layer."), true);
	Builder.AddBoolean(TEXT("no_weight_blend"), TEXT("If true, layer uses no weight blending."));
	Builder.AddString(TEXT("layer_info_path"),
		TEXT("Optional path to an existing LandscapeLayerInfoObject asset to reuse, e.g. one shared "
			 "by another map's landscape. When omitted a new one is created inside the landscape's "
			 "package. Reuse is what lets two maps share painted layer identity."));
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

	ULandscapeInfo* LandscapeInfo = Data->LandscapeInfo.Get();
	ALandscapeProxy* Proxy = Data->LandscapeProxy.Get();
	if (!IsValid(LandscapeInfo) || !IsValid(Proxy))
	{
		return MakeErrorResult(TEXT("Landscape session is no longer valid"));
	}

	// Reuse the same layer-info object to share painted-layer identity across maps.
	ULandscapeLayerInfoObject* LayerInfo = nullptr;
	FString LayerInfoPath;
	if (Arguments->TryGetStringField(TEXT("layer_info_path"), LayerInfoPath) && !LayerInfoPath.IsEmpty())
	{
		const auto Resolved = ClaireonPathResolver::Resolve(LayerInfoPath);
		const FString LoadPath = Resolved.bSuccess ? Resolved.ResolvedPath.Path : LayerInfoPath;
		LayerInfo = LoadObject<ULandscapeLayerInfoObject>(nullptr, *LoadPath);
		if (!IsValid(LayerInfo))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("No LandscapeLayerInfoObject at '%s'"), *LayerInfoPath));
		}
	}
	else
	{
		// Keep the layer info in the landscape package so it can be saved.
		LayerInfo = NewObject<ULandscapeLayerInfoObject>(
			Proxy, MakeUniqueObjectName(Proxy, ULandscapeLayerInfoObject::StaticClass(), FName(*LayerName)),
			RF_Public | RF_Standalone | RF_Transactional);
		if (!IsValid(LayerInfo))
		{
			return MakeErrorResult(TEXT("Failed to create the layer info object"));
		}
#if UE_VERSION_OLDER_THAN(5, 7, 0)
		LayerInfo->LayerName = FName(*LayerName);
		LayerInfo->bNoWeightBlend = bNoWeightBlend;
#else
		LayerInfo->SetLayerName(FName(*LayerName), /*bInModify=*/false);
		LayerInfo->SetBlendMethod(bNoWeightBlend ? ELandscapeTargetLayerBlendMethod::None : ELandscapeTargetLayerBlendMethod::FinalWeightBlending, /*bInModify=*/false);
#endif
	}

	LandscapeInfo->Layers.Add(FLandscapeInfoLayerSettings(LayerInfo, Proxy));

	// Persist the layer in TargetLayers; LandscapeInfo::Layers is rebuilt on load.
	// EditorLayerSettings is a deprecated reflected alias, not active storage.
	Proxy->Modify();
	if (!Proxy->HasTargetLayer(LayerInfo))
	{
		Proxy->AddTargetLayer(FName(*LayerName), FLandscapeTargetLayerSettings(LayerInfo));
	}

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
