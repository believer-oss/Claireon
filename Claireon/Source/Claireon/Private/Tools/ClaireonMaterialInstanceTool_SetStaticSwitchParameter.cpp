// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialInstanceTool_SetStaticSwitchParameter.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/MaterialInstanceConstant.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialInstanceTool_SetStaticSwitchParameter::GetName() const
{
	return TEXT("claireon.material_instance_set_static_switch_parameter");
}

FString ClaireonMaterialInstanceTool_SetStaticSwitchParameter::GetDescription() const
{
	return TEXT("Set a static switch parameter override on a UMaterialInstanceConstant (triggers shader-map rebuild).");
}

TSharedPtr<FJsonObject> ClaireonMaterialInstanceTool_SetStaticSwitchParameter::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("parameter_name"), TEXT("Name of the static switch parameter."), true);
	Builder.AddBoolean(TEXT("value"), TEXT("Boolean value to set."), true);
	return Builder.Build();
}

FToolResult ClaireonMaterialInstanceTool_SetStaticSwitchParameter::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	bool Value = false;
	if (!Arguments->TryGetBoolField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value (bool)"));
	}

	UMaterialInstanceConstant* Instance = Data->Instance.Get();
	FString Err;
	if (!ClaireonMaterialHelpers::SetMICStaticSwitch(Instance, FName(*ParameterName), Value, Err))
	{
		return MakeErrorResult(Err);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("Effect: set static switch override '%s' = %s (triggers shader-map rebuild)"),
		*ParameterName, Value ? TEXT("true") : TEXT("false"));
	return BuildStateResponse(SessionId, Data);
}
