// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialTool_SetParameterDefault.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Materials/Material.h"
#include "Engine/Texture.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/SoftObjectPath.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialTool_SetParameterDefault::GetOperation() const { return TEXT("set_parameter_default"); }

FString ClaireonMaterialTool_SetParameterDefault::GetDescription() const
{
    return TEXT("Set the default value of a parameter expression in the material (scalar/vector/texture/static_switch/static_component_mask). Session-mode tool: open via material_open first.");
}

TSharedPtr<FJsonObject> ClaireonMaterialTool_SetParameterDefault::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("parameter_name"), TEXT("Name of the parameter."), true);
	TArray<FString> TypeValues = {
		TEXT("scalar"), TEXT("vector"), TEXT("texture"),
		TEXT("static_switch"), TEXT("static_component_mask")};
	Builder.AddEnum(TEXT("parameter_type"), TEXT("Type of the parameter."), TypeValues, true);
	// value is type-polymorphic; schema keeps it unconstrained
	TSharedPtr<FJsonObject> ValueProp = MakeShared<FJsonObject>();
	ValueProp->SetStringField(TEXT("description"), TEXT("Value to set. Format depends on parameter_type: number (scalar), array/object RGBA (vector), string texture path (texture), bool (static_switch), array/object RGBA bools (static_component_mask)."));
	// Schema already captures required; add value as required separately via a fallback -- AddObject without required
	Builder.AddObject(TEXT("value"), TEXT("Value to set (type depends on parameter_type)."), true);
	return Builder.Build();
}

FToolResult ClaireonMaterialTool_SetParameterDefault::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FMaterialEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString ParamName, ParamType;
	if (!Arguments->TryGetStringField(TEXT("parameter_name"), ParamName) || ParamName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'set_parameter_default' requires 'parameter_name'"));
	}
	if (!Arguments->TryGetStringField(TEXT("parameter_type"), ParamType) || ParamType.IsEmpty())
	{
		return MakeErrorResult(TEXT("'set_parameter_default' requires 'parameter_type'"));
	}

	const TSharedPtr<FJsonValue> ValueField = Arguments->TryGetField(TEXT("value"));
	if (!ValueField.IsValid())
	{
		return MakeErrorResult(TEXT("'set_parameter_default' requires 'value'"));
	}

	UMaterial* Material = Data->Material.Get();
	const FName ParamFName(*ParamName);
	FString SetErr;
	bool bOk = false;

	if (ParamType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
	{
		bOk = ClaireonMaterialHelpers::SetScalarParameterDefault(
			Material, ParamFName, static_cast<float>(ValueField->AsNumber()), SetErr);
	}
	else if (ParamType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
	{
		FLinearColor LC(ForceInit);
		if (ValueField->Type == EJson::Array)
		{
			TArray<TSharedPtr<FJsonValue>> Arr = ValueField->AsArray();
			if (Arr.Num() >= 3)
			{
				LC.R = static_cast<float>(Arr[0]->AsNumber());
				LC.G = static_cast<float>(Arr[1]->AsNumber());
				LC.B = static_cast<float>(Arr[2]->AsNumber());
				LC.A = Arr.Num() >= 4 ? static_cast<float>(Arr[3]->AsNumber()) : 1.0f;
			}
			else
			{
				return MakeErrorResult(TEXT("vector value must be array of length 3 or 4"));
			}
		}
		else if (ValueField->Type == EJson::Object)
		{
			TSharedPtr<FJsonObject> Col = ValueField->AsObject();
			double R = 0, G = 0, B = 0, A = 1;
			Col->TryGetNumberField(TEXT("R"), R);
			Col->TryGetNumberField(TEXT("G"), G);
			Col->TryGetNumberField(TEXT("B"), B);
			Col->TryGetNumberField(TEXT("A"), A);
			LC.R = static_cast<float>(R);
			LC.G = static_cast<float>(G);
			LC.B = static_cast<float>(B);
			LC.A = static_cast<float>(A);
		}
		else
		{
			return MakeErrorResult(TEXT("vector value must be JSON array or object"));
		}
		bOk = ClaireonMaterialHelpers::SetVectorParameterDefault(Material, ParamFName, LC, SetErr);
	}
	else if (ParamType.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
	{
		const FString TexPath = ValueField->AsString();
		UTexture* Tex = nullptr;
		if (!TexPath.IsEmpty() && !TexPath.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			FSoftObjectPath SoftPath(TexPath);
			Tex = Cast<UTexture>(SoftPath.TryLoad());
			if (!Tex)
			{
				return MakeErrorResult(FString::Printf(TEXT("Failed to load texture '%s'"), *TexPath));
			}
		}
		bOk = ClaireonMaterialHelpers::SetTextureParameterDefault(Material, ParamFName, Tex, SetErr);
	}
	else if (ParamType.Equals(TEXT("static_switch"), ESearchCase::IgnoreCase))
	{
		bOk = ClaireonMaterialHelpers::SetStaticSwitchParameterDefault(
			Material, ParamFName, ValueField->AsBool(), SetErr);
	}
	else if (ParamType.Equals(TEXT("static_component_mask"), ESearchCase::IgnoreCase))
	{
		bool R = false, G = false, B = false, A = false;
		if (ValueField->Type == EJson::Object)
		{
			TSharedPtr<FJsonObject> Mask = ValueField->AsObject();
			Mask->TryGetBoolField(TEXT("R"), R);
			Mask->TryGetBoolField(TEXT("G"), G);
			Mask->TryGetBoolField(TEXT("B"), B);
			Mask->TryGetBoolField(TEXT("A"), A);
		}
		else if (ValueField->Type == EJson::Array)
		{
			TArray<TSharedPtr<FJsonValue>> Arr = ValueField->AsArray();
			if (Arr.Num() >= 1) R = Arr[0]->AsBool();
			if (Arr.Num() >= 2) G = Arr[1]->AsBool();
			if (Arr.Num() >= 3) B = Arr[2]->AsBool();
			if (Arr.Num() >= 4) A = Arr[3]->AsBool();
		}
		bOk = ClaireonMaterialHelpers::SetStaticComponentMaskParameterDefault(Material, ParamFName, R, G, B, A, SetErr);
	}
	else
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Unknown parameter_type '%s' (expected scalar|vector|texture|static_switch|static_component_mask)"),
			*ParamType));
	}

	if (!bOk)
	{
		return MakeErrorResult(SetErr);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("Effect: set %s default for parameter '%s'"), *ParamType, *ParamName);
	return BuildStateResponse(SessionId, Data);
}
