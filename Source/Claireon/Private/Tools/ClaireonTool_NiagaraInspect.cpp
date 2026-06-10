// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_NiagaraInspect.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "ClaireonLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraRendererProperties.h"

FString ClaireonTool_NiagaraInspect::GetCategory() const { return TEXT("niagara"); }
FString ClaireonTool_NiagaraInspect::GetOperation() const { return TEXT("inspect"); }

FString ClaireonTool_NiagaraInspect::GetDescription() const
{
	return TEXT("Read the structure of a Niagara System asset. "
				"Displays emitters (with renderers and properties), user parameters, "
				"and system-level settings. Useful for understanding particle effect "
				"configuration and identifying what can be edited.");
}

TSharedPtr<FJsonObject> ClaireonTool_NiagaraInspect::GetInputSchema() const
{
	// Follow the exact pattern from EQS inspect
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Unreal asset path to the Niagara System (e.g. /Game/Effects/NS_FireEffect)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// detail_level - optional
	TSharedPtr<FJsonObject> DetailProp = MakeShared<FJsonObject>();
	DetailProp->SetStringField(TEXT("type"), TEXT("string"));
	DetailProp->SetStringField(TEXT("description"), TEXT("Level of detail: 'summary' for structure overview, 'full' for complete property details (default: full)"));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("summary")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("full")));
		DetailProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("detail_level"), DetailProp);

	// emitter_index - optional
	TSharedPtr<FJsonObject> EmitterIdxProp = MakeShared<FJsonObject>();
	EmitterIdxProp->SetStringField(TEXT("type"), TEXT("integer"));
	EmitterIdxProp->SetStringField(TEXT("description"), TEXT("Optional: focus on a specific emitter by index (0-based). If omitted, all emitters are shown."));
	Properties->SetObjectField(TEXT("emitter_index"), EmitterIdxProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_NiagaraInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	FString Error;
	UNiagaraSystem* System = ClaireonNiagaraHelpers::LoadNiagaraSystemAsset(AssetPath, Error);
	if (!System)
	{
		return MakeErrorResult(Error);
	}

	FString DetailLevel = TEXT("full");
	Arguments->TryGetStringField(TEXT("detail_level"), DetailLevel);

	int32 EmitterIndexFilter = -1;
	Arguments->TryGetNumberField(TEXT("emitter_index"), EmitterIndexFilter);

	// Build emitters array
	const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

	TArray<TSharedPtr<FJsonValue>> EmittersArray;
	int32 TotalModules = 0;

	for (int32 i = 0; i < Handles.Num(); ++i)
	{
		if (EmitterIndexFilter >= 0 && i != EmitterIndexFilter)
		{
			continue;
		}

		const FNiagaraEmitterHandle& Handle = Handles[i];

		TSharedPtr<FJsonObject> EmitterObj = MakeShared<FJsonObject>();
		EmitterObj->SetNumberField(TEXT("index"), i);
		EmitterObj->SetStringField(TEXT("name"), Handle.GetName().ToString());
		EmitterObj->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());

		// Renderer info
		TArray<TSharedPtr<FJsonValue>> RenderersArray;
		if (FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData())
		{
			const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
			TotalModules += Renderers.Num();
			for (int32 r = 0; r < Renderers.Num(); ++r)
			{
				if (Renderers[r])
				{
					TSharedPtr<FJsonObject> RendObj = MakeShared<FJsonObject>();
					RendObj->SetNumberField(TEXT("index"), r);
					RendObj->SetStringField(TEXT("type"), ClaireonNiagaraHelpers::GetRendererTypeName(Renderers[r]));

					if (DetailLevel == TEXT("full"))
					{
						FString PropsText = ClaireonNiagaraHelpers::FormatRendererProperties(Renderers[r], r);
						RendObj->SetStringField(TEXT("properties"), PropsText);
					}
					RenderersArray.Add(MakeShared<FJsonValueObject>(RendObj));
				}
			}
		}
		EmitterObj->SetArrayField(TEXT("renderers"), RenderersArray);
		EmittersArray.Add(MakeShared<FJsonValueObject>(EmitterObj));
	}

	// Parameters text
	FString ParametersText = ClaireonNiagaraHelpers::FormatUserParameters(System);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetArrayField(TEXT("emitters"), EmittersArray);
	Data->SetStringField(TEXT("parameters"), ParametersText);

	FString AssetName = FPaths::GetBaseFilename(AssetPath);
	const FString Summary = FString::Printf(TEXT("%s: %d emitters, %d modules"),
		*AssetName, Handles.Num(), TotalModules);

	return MakeSuccessResult(Data, Summary);
}
