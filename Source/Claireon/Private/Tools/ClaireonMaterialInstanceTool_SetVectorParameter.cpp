// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialInstanceTool_SetVectorParameter.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/MaterialInstanceConstant.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialInstanceTool_SetVectorParameter::GetOperation() const { return TEXT("instance_set_vector_parameter"); }

FString ClaireonMaterialInstanceTool_SetVectorParameter::GetDescription() const
{
    return TEXT("Set a vector (LinearColor) parameter override on a UMaterialInstanceConstant. Session-mode tool: open via material_instance_open first.");
}

TSharedPtr<FJsonObject> ClaireonMaterialInstanceTool_SetVectorParameter::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("parameter_name"), TEXT("Name of the vector parameter."), true);
	Builder.AddNumber(TEXT("r"), TEXT("Red component (default 0)."));
	Builder.AddNumber(TEXT("g"), TEXT("Green component (default 0)."));
	Builder.AddNumber(TEXT("b"), TEXT("Blue component (default 0)."));
	Builder.AddNumber(TEXT("a"), TEXT("Alpha component (default 1)."));
	return Builder.Build();
}

FToolResult ClaireonMaterialInstanceTool_SetVectorParameter::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	double R = 0.0, G = 0.0, B = 0.0, A = 1.0;
	Arguments->TryGetNumberField(TEXT("r"), R);
	Arguments->TryGetNumberField(TEXT("g"), G);
	Arguments->TryGetNumberField(TEXT("b"), B);
	Arguments->TryGetNumberField(TEXT("a"), A);

	const FLinearColor Value(static_cast<float>(R), static_cast<float>(G), static_cast<float>(B), static_cast<float>(A));

	UMaterialInstanceConstant* Instance = Data->Instance.Get();
	FString Err;
	if (!ClaireonMaterialHelpers::SetMICVector(Instance, FName(*ParameterName), Value, Err))
	{
		return MakeErrorResult(Err);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("Effect: set vector override '%s' = (%f, %f, %f, %f)"),
		*ParameterName, Value.R, Value.G, Value.B, Value.A);
	return BuildStateResponse(SessionId, Data);
}
