// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialInstanceTool_SetStaticComponentMaskParameter.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/MaterialInstanceConstant.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialInstanceTool_SetStaticComponentMaskParameter::GetName() const
{
	return TEXT("claireon.material_instance_set_static_component_mask_parameter");
}

FString ClaireonMaterialInstanceTool_SetStaticComponentMaskParameter::GetDescription() const
{
	return TEXT("Set a static component mask parameter override on a UMaterialInstanceConstant (triggers shader-map rebuild).");
}

TSharedPtr<FJsonObject> ClaireonMaterialInstanceTool_SetStaticComponentMaskParameter::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("parameter_name"), TEXT("Name of the static component mask parameter."), true);
	Builder.AddBoolean(TEXT("r"), TEXT("Red mask bit (default false)."));
	Builder.AddBoolean(TEXT("g"), TEXT("Green mask bit (default false)."));
	Builder.AddBoolean(TEXT("b"), TEXT("Blue mask bit (default false)."));
	Builder.AddBoolean(TEXT("a"), TEXT("Alpha mask bit (default false)."));
	return Builder.Build();
}

FToolResult ClaireonMaterialInstanceTool_SetStaticComponentMaskParameter::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FMaterialInstanceEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString ParameterName;
	if (!Arguments->TryGetStringField(TEXT("parameter_name"), ParameterName) || ParameterName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: parameter_name"));
	}
	bool R = false, G = false, B = false, A = false;
	Arguments->TryGetBoolField(TEXT("r"), R);
	Arguments->TryGetBoolField(TEXT("g"), G);
	Arguments->TryGetBoolField(TEXT("b"), B);
	Arguments->TryGetBoolField(TEXT("a"), A);

	UMaterialInstanceConstant* Instance = Data->Instance.Get();
	FString Err;
	if (!ClaireonMaterialHelpers::SetMICStaticComponentMask(Instance, FName(*ParameterName), R, G, B, A, Err))
	{
		return MakeErrorResult(Err);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("Effect: set static component mask override '%s' = (R=%d, G=%d, B=%d, A=%d) (triggers shader-map rebuild)"),
		*ParameterName, R ? 1 : 0, G ? 1 : 0, B ? 1 : 0, A ? 1 : 0);
	return BuildStateResponse(SessionId, Data);
}
