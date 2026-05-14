// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialInstanceTool_SetScalarParameter.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/MaterialInstanceConstant.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialInstanceTool_SetScalarParameter::GetOperation() const { return TEXT("instance_set_scalar_parameter"); }

FString ClaireonMaterialInstanceTool_SetScalarParameter::GetDescription() const
{
	return TEXT("Set a scalar parameter override on a UMaterialInstanceConstant.");
}

TSharedPtr<FJsonObject> ClaireonMaterialInstanceTool_SetScalarParameter::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("parameter_name"), TEXT("Name of the scalar parameter."), true);
	Builder.AddNumber(TEXT("value"), TEXT("Scalar value to set."), true);
	return Builder.Build();
}

FToolResult ClaireonMaterialInstanceTool_SetScalarParameter::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	double Value = 0.0;
	if (!Arguments->TryGetNumberField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	UMaterialInstanceConstant* Instance = Data->Instance.Get();
	FString Err;
	if (!ClaireonMaterialHelpers::SetMICScalar(Instance, FName(*ParameterName), static_cast<float>(Value), Err))
	{
		return MakeErrorResult(Err);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("Effect: set scalar override '%s' = %f"), *ParameterName, Value);
	return BuildStateResponse(SessionId, Data);
}
