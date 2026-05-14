// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialInstanceTool_SetTextureParameter.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Engine/Texture.h"
#include "UObject/SoftObjectPath.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialInstanceTool_SetTextureParameter::GetOperation() const { return TEXT("instance_set_texture_parameter"); }

FString ClaireonMaterialInstanceTool_SetTextureParameter::GetDescription() const
{
	return TEXT("Set a texture parameter override on a UMaterialInstanceConstant. Pass empty texture_path or 'None' to clear.");
}

TSharedPtr<FJsonObject> ClaireonMaterialInstanceTool_SetTextureParameter::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("parameter_name"), TEXT("Name of the texture parameter."), true);
	Builder.AddString(TEXT("texture_path"), TEXT("Path to the texture asset. Empty or 'None' clears the override."), true);
	return Builder.Build();
}

FToolResult ClaireonMaterialInstanceTool_SetTextureParameter::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FMaterialInstanceEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString ParameterName, TexturePath;
	if (!Arguments->TryGetStringField(TEXT("parameter_name"), ParameterName) || ParameterName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: parameter_name"));
	}
	if (!Arguments->TryGetStringField(TEXT("texture_path"), TexturePath))
	{
		return MakeErrorResult(TEXT("Missing required parameter: texture_path"));
	}

	UTexture* Tex = nullptr;
	if (!TexturePath.IsEmpty() && !TexturePath.Equals(TEXT("None"), ESearchCase::IgnoreCase))
	{
		FSoftObjectPath SoftPath(TexturePath);
		Tex = Cast<UTexture>(SoftPath.TryLoad());
		if (!Tex)
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to load texture '%s'"), *TexturePath));
		}
	}

	UMaterialInstanceConstant* Instance = Data->Instance.Get();
	FString Err;
	if (!ClaireonMaterialHelpers::SetMICTexture(Instance, FName(*ParameterName), Tex, Err))
	{
		return MakeErrorResult(Err);
	}

	const FString TexLabel = Tex ? Tex->GetPathName() : FString(TEXT("(none)"));
	Data->LastOperationStatus = FString::Printf(TEXT("Effect: set texture override '%s' = %s"), *ParameterName, *TexLabel);
	return BuildStateResponse(SessionId, Data);
}
