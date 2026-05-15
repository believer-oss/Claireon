// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_MaterialInstanceInspect.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"

#include "Materials/MaterialInstanceConstant.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_MaterialInstanceInspect::GetName() const
{
	return TEXT("claireon.material_instance_inspect");
}

FString ClaireonTool_MaterialInstanceInspect::GetDescription() const
{
	return TEXT("Read a UMaterialInstanceConstant's parent chain and per-parameter "
				"override vs inherited values. Emits one table per parameter type "
				"(Scalar, Vector, Texture, Static Switch, Static Component Mask). "
				"Override Value cell is empty when the MIC does not override that "
				"parameter.");
}

TSharedPtr<FJsonObject> ClaireonTool_MaterialInstanceInspect::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Unreal asset path to the UMaterialInstanceConstant (e.g. /Game/Art/Materials/MIC_Foo)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonTool_MaterialInstanceInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Missing arguments"));
	}

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	FString LoadError;
	UMaterialInstanceConstant* Instance = ClaireonMaterialHelpers::LoadMaterialInstanceAsset(AssetPath, LoadError);
	if (!Instance)
	{
		return MakeErrorResult(LoadError);
	}

	const FString Markdown = ClaireonMaterialHelpers::FormatMaterialInstance(Instance);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	if (Instance->Parent)
	{
		Data->SetStringField(TEXT("parent_path"), Instance->Parent->GetPathName());
	}
	Data->SetStringField(TEXT("structure"), Markdown);

	const FString AssetName = FPaths::GetBaseFilename(AssetPath);
	const int32 ScalarOverrides = Instance->ScalarParameterValues.Num();
	const int32 VectorOverrides = Instance->VectorParameterValues.Num();
	const int32 TextureOverrides = Instance->TextureParameterValues.Num();
	const int32 TotalOverrides = ScalarOverrides + VectorOverrides + TextureOverrides;
	const FString Summary = FString::Printf(TEXT("%s: %d overrides (S=%d V=%d T=%d)"),
		*AssetName, TotalOverrides, ScalarOverrides, VectorOverrides, TextureOverrides);

	return MakeSuccessResult(Data, Summary);
}
