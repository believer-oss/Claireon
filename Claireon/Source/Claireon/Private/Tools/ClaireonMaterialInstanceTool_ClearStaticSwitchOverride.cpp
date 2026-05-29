// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialInstanceTool_ClearStaticSwitchOverride.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/MaterialInstanceConstant.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialInstanceTool_ClearStaticSwitchOverride::GetOperation() const { return TEXT("instance_clear_static_switch_override"); }

FString ClaireonMaterialInstanceTool_ClearStaticSwitchOverride::GetDescription() const
{
    return TEXT("Clear a static-switch parameter override on a UMaterialInstanceConstant; the parameter falls through to the parent material. Session-mode tool: open via material_instance_open first.");
}

TSharedPtr<FJsonObject> ClaireonMaterialInstanceTool_ClearStaticSwitchOverride::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("parameter_name"), TEXT("Name of the static-switch parameter whose override to clear."), true);
	return Builder.Build();
}

FToolResult ClaireonMaterialInstanceTool_ClearStaticSwitchOverride::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	if (!ClaireonMaterialHelpers::ClearMICOverride(Instance, FName(*ParameterName), EMaterialParameterType::StaticSwitch, Err))
	{
		return MakeErrorResult(Err);
	}

	Instance->MarkPackageDirty();
	Data->LastOperationStatus = FString::Printf(TEXT("Effect: cleared static-switch override '%s'"), *ParameterName);
	return BuildStateResponse(SessionId, Data);
}
