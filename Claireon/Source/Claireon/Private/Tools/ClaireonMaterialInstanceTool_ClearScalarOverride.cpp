// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialInstanceTool_ClearScalarOverride.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/MaterialInstanceConstant.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialInstanceTool_ClearScalarOverride::GetOperation() const { return TEXT("instance_clear_scalar_override"); }

FString ClaireonMaterialInstanceTool_ClearScalarOverride::GetDescription() const
{
    return TEXT("Clear a scalar parameter override on a UMaterialInstanceConstant; the parameter falls through to the parent material. Session-mode tool: open via material_instance_open first.");
}

TSharedPtr<FJsonObject> ClaireonMaterialInstanceTool_ClearScalarOverride::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("parameter_name"), TEXT("Name of the scalar parameter whose override to clear."), true);
	return Builder.Build();
}

FToolResult ClaireonMaterialInstanceTool_ClearScalarOverride::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	UMaterialInstanceConstant* Instance = Data->Instance.Get();
	FString Err;
	if (!ClaireonMaterialHelpers::ClearMICOverride(Instance, FName(*ParameterName), EMaterialParameterType::Scalar, Err))
	{
		return MakeErrorResult(Err);
	}

	Instance->MarkPackageDirty();
	Data->LastOperationStatus = FString::Printf(TEXT("Effect: cleared scalar override '%s'"), *ParameterName);
	return BuildStateResponse(SessionId, Data);
}
