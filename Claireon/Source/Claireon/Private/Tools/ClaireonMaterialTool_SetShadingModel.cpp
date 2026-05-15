// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialTool_SetShadingModel.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/Material.h"

using FToolResult = IClaireonTool::FToolResult;

namespace
{
	static bool ParseShadingModel_SetShadingModel(const FString& Str, EMaterialShadingModel& OutModel)
	{
		const UEnum* Enum = StaticEnum<EMaterialShadingModel>();
		if (!Enum) return false;
		const int64 Val = Enum->GetValueByNameString(Str);
		if (Val == INDEX_NONE) return false;
		OutModel = static_cast<EMaterialShadingModel>(Val);
		return true;
	}
}

FString ClaireonMaterialTool_SetShadingModel::GetName() const
{
	return TEXT("claireon.material_set_shading_model");
}

FString ClaireonMaterialTool_SetShadingModel::GetDescription() const
{
	return TEXT("Set the material's shading model (e.g. MSM_DefaultLit, MSM_Unlit).");
}

TSharedPtr<FJsonObject> ClaireonMaterialTool_SetShadingModel::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("shading_model"), TEXT("Shading model string (e.g. MSM_DefaultLit, MSM_Unlit)."), true);
	return Builder.Build();
}

FToolResult ClaireonMaterialTool_SetShadingModel::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FMaterialEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString ShadingStr;
	if (!Arguments->TryGetStringField(TEXT("shading_model"), ShadingStr) || ShadingStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("'set_shading_model' requires 'shading_model'"));
	}

	EMaterialShadingModel SM;
	if (!ParseShadingModel_SetShadingModel(ShadingStr, SM))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid shading_model: '%s'"), *ShadingStr));
	}

	UMaterial* Material = Data->Material.Get();
	FString SmErr;
	if (!ClaireonMaterialHelpers::SetShadingModel(Material, SM, SmErr))
	{
		return MakeErrorResult(SmErr);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("Effect: set shading_model to %s"), *ShadingStr);
	return BuildStateResponse(SessionId, Data);
}
