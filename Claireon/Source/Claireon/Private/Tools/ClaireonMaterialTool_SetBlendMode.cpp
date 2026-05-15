// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialTool_SetBlendMode.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/Material.h"

using FToolResult = IClaireonTool::FToolResult;

namespace
{
	static bool ParseBlendMode_SetBlendMode(const FString& Str, EBlendMode& OutMode)
	{
		const UEnum* Enum = StaticEnum<EBlendMode>();
		if (!Enum) return false;
		const int64 Val = Enum->GetValueByNameString(Str);
		if (Val == INDEX_NONE) return false;
		OutMode = static_cast<EBlendMode>(Val);
		return true;
	}
}

FString ClaireonMaterialTool_SetBlendMode::GetName() const
{
	return TEXT("claireon.material_set_blend_mode");
}

FString ClaireonMaterialTool_SetBlendMode::GetDescription() const
{
	return TEXT("Set the material's blend mode (e.g. BLEND_Opaque, BLEND_Translucent).");
}

TSharedPtr<FJsonObject> ClaireonMaterialTool_SetBlendMode::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("blend_mode"), TEXT("Blend mode string (e.g. BLEND_Opaque, BLEND_Translucent)."), true);
	return Builder.Build();
}

FToolResult ClaireonMaterialTool_SetBlendMode::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FMaterialEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString BlendStr;
	if (!Arguments->TryGetStringField(TEXT("blend_mode"), BlendStr) || BlendStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("'set_blend_mode' requires 'blend_mode'"));
	}

	EBlendMode BM;
	if (!ParseBlendMode_SetBlendMode(BlendStr, BM))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid blend_mode: '%s'"), *BlendStr));
	}

	UMaterial* Material = Data->Material.Get();
	FString BmErr;
	if (!ClaireonMaterialHelpers::SetBlendMode(Material, BM, BmErr))
	{
		return MakeErrorResult(BmErr);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("Effect: set blend_mode to %s"), *BlendStr);
	return BuildStateResponse(SessionId, Data);
}
