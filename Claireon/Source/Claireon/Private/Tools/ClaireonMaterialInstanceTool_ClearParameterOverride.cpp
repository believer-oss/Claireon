// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialInstanceTool_ClearParameterOverride.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/MaterialInstanceConstant.h"

using FToolResult = IClaireonTool::FToolResult;

namespace
{
	static bool ParseMICParameterTypeString(const FString& Type, EMaterialParameterType& OutType)
	{
		if (Type.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
		{
			OutType = EMaterialParameterType::Scalar;
			return true;
		}
		if (Type.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
		{
			OutType = EMaterialParameterType::Vector;
			return true;
		}
		if (Type.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
		{
			OutType = EMaterialParameterType::Texture;
			return true;
		}
		if (Type.Equals(TEXT("static_switch"), ESearchCase::IgnoreCase))
		{
			OutType = EMaterialParameterType::StaticSwitch;
			return true;
		}
		if (Type.Equals(TEXT("static_component_mask"), ESearchCase::IgnoreCase))
		{
			OutType = EMaterialParameterType::StaticComponentMask;
			return true;
		}
		return false;
	}
}

FString ClaireonMaterialInstanceTool_ClearParameterOverride::GetOperation() const { return TEXT("instance_clear_parameter_override"); }

FString ClaireonMaterialInstanceTool_ClearParameterOverride::GetDescription() const
{
	return TEXT("Clear a parameter override on a UMaterialInstanceConstant (parameter will fall through to parent).");
}

TSharedPtr<FJsonObject> ClaireonMaterialInstanceTool_ClearParameterOverride::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("parameter_name"), TEXT("Name of the parameter whose override to clear."), true);
	TArray<FString> TypeValues = {
		TEXT("scalar"), TEXT("vector"), TEXT("texture"),
		TEXT("static_switch"), TEXT("static_component_mask")};
	Builder.AddEnum(TEXT("parameter_type"), TEXT("Type of the parameter."), TypeValues, true);
	return Builder.Build();
}

FToolResult ClaireonMaterialInstanceTool_ClearParameterOverride::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FMaterialInstanceEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString ParameterName, ParameterType;
	if (!Arguments->TryGetStringField(TEXT("parameter_name"), ParameterName) || ParameterName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: parameter_name"));
	}
	if (!Arguments->TryGetStringField(TEXT("parameter_type"), ParameterType) || ParameterType.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: parameter_type"));
	}

	EMaterialParameterType Type;
	if (!ParseMICParameterTypeString(ParameterType, Type))
	{
		return MakeErrorResult(FString::Printf(TEXT("Unknown parameter_type '%s' (expected scalar|vector|texture|static_switch|static_component_mask)"), *ParameterType));
	}

	UMaterialInstanceConstant* Instance = Data->Instance.Get();
	FString Err;
	if (!ClaireonMaterialHelpers::ClearMICOverride(Instance, FName(*ParameterName), Type, Err))
	{
		return MakeErrorResult(Err);
	}

	Instance->MarkPackageDirty();
	Data->LastOperationStatus = FString::Printf(TEXT("Effect: cleared %s override '%s'"), *ParameterType, *ParameterName);
	return BuildStateResponse(SessionId, Data);
}
